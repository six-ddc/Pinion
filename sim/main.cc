// pi_sim — desktop (SDL2) simulator for the pi_screen UI.
//
// Replicates main/main.cc's boot chain minus hardware: an LVGL SDL window
// stands in for mhal::Init()'s display bring-up; screen creation, lifecycle
// attach and screen load are verbatim. The agent path is the real one —
// pi_agent_task.c + pi-c POSIX port (libcurl) → real DeepSeek API.
//
// Controls:
//   F1 (hold)        PWR_KEY 按住说话 — hold to record, release to send; a
//                    quick tap does nothing; tap during generation = interrupt
//   typing           speech while listening (types the "utterance")
//   Backspace        delete last codepoint of the "utterance"
//   F12              screenshot (BMP, path from PI_SIM_SHOT or pi_sim_shot.bmp)
//   mouse            touch (drag down from the status bar = quick panel)
//
// Env knobs: PI_SIM_SAY=1 (speak TTS via macOS `say`), PI_SIM_AUTODEMO=<text>
// (scripted demo: press key, type text, send), PI_SIM_SHOT / PI_SIM_SHOT_MS /
// PI_SIM_EXIT_MS (unattended screenshot + exit, for CI/self-test).
#include <SDL.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"

#include "cJSON.h"
#include "IOExpander.hpp"
#include "settings.h"  // P1 grid rehydrate 测试：直写 standby pin 封套
#include "pi_theme.h"  // T1 主题往返测试：pi_theme::Set
#include "device_config.h"  // mediastage 测试命令：运行时电台列表（Web 后台可配 or 种子）
#include "media_player/media_player.h"
#include "pi_card/pi_card_data.h"
#include "pi_card/pi_card_host.h"
#include "pi_card/pi_card_media.h"
#include "pi/pi_partial_json.h"  // previewscenechunks：真实字节级流式回放用
#include "pi_card/pi_card_preview.h"
#include "pi_card/pi_card_tools.h"
#include "pi_ui_bridge.h"
#include "stock/stock_tool.h"
#include "device_config.h"
#include "web_admin_httpd.h"
#include "pi_screen.h"
#include "screen_util.h"
#include "sim_hooks.h"

namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_pwr_key_held{false};  // F1 held = PWR_KEY pressed (press-and-hold)
std::atomic<bool> g_shot_pending{false};
std::atomic<bool> g_demo_pending{false};  // F9 = 渲染一张 pi_card 演示卡（overlay，任何视图可见）
// TEMP SCAFFOLD: previewscene 延迟截图倒计时 — feed 完最后一帧后，布局要再跑几个主循环
// 迭代才稳定，所以不能在 ExecCmd 里立即截图。-1 = 空闲；PollCmdFile/ExecCmd 与 Pump() 都在
// 主线程跑，不需要原子/锁。
int g_previewscene_countdown = -1;
std::string g_previewscene_shot_path;

// pi_card v2 演示卡语料：不再硬编码 v1 JSON——F9/PI_SIM_CARD_MS 播放的 26 张卡直接来自
// sim/tests/corpus/*.json（与 card_solver_test/preview_prefix_test 共用同一份语料，避免第
// 三份漂移的硬编码卡）。CorpusDir() 在几个常见运行目录候选里探测哪个能读到语料（不依赖
// cmake 传入宏——main.cc 不在本轮授权改动 CMakeLists.txt 的范围内）。v1 的两张 justify/align
// 演示卡已随 justify/align 属性一起删除，不迁移（docs/CARD_V2.md §5.2 idx20/21：废弃删除，
// 非需求变更）。
std::string CorpusDir() {
    static std::string dir;
    if (!dir.empty()) return dir;
    static const char* kCandidates[] = {
        "sim/tests/corpus/",     // 从仓库根运行（README 示例用法）
        "../tests/corpus/",      // 从 sim/build 运行
        "tests/corpus/",         // 从 sim/ 运行
        "../sim/tests/corpus/",  // 从其它构建目录布局运行
    };
    for (const char* c : kCandidates) {
        std::ifstream probe(std::string(c) + "00_device_ctl.json");
        if (probe.good()) {
            dir = c;
            return dir;
        }
    }
    dir = kCandidates[0];  // 探测全失败：回落第一个候选（后续读文件会失败并打日志，不静默崩）
    return dir;
}

std::string ReadCorpusFile(const std::string& name) {
    std::ifstream f(CorpusDir() + name);
    if (!f.good()) {
        std::fprintf(stderr, "[sim] corpus file not found: %s (tried dir '%s')\n", name.c_str(),
                     CorpusDir().c_str());
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 26 张正例语料（docs/CARD_V2.md §5.2 全量迁移映射表 + n1/n2 新增验收卡），F9/PI_SIM_CARD_IDX
// 按下标播放；顺序与 sim/tests/corpus/README.md 一致。
constexpr const char* kCorpusFiles[] = {
    "00_device_ctl.json", "01_confirm.json",   "02_menu.json",      "03_status.json",
    "04_wrap_stress.json", "05_degenerate.json", "07_arc.json",      "08_qrcode.json",
    "09_choice.json",     "10_patch.json",      "11_telemetry.json", "12_charts.json",
    "13_sensors.json",    "14_gps.json",        "15_multicol.json",  "16_stock_bind.json",
    "17_media_ctl.json",  "18_stock_chart.json", "19_style_family.json", "22_table.json",
    "23_grid_auto.json",  "24_grid_ctl.json",   "25_grid_tall.json", "f0_form.json",
    "n1_cells_wrap.json", "n2_rows_align.json",
    // XML 线格式语料（docs/CARD_XML.md §7）：追加在末尾保证既有下标稳定。.xml 文件由
    // CorpusCards() 包成 {"xml":"…"} args 走 xml 通道；x0-x4 是 XML 提示词的五个示例原文
    // （红线：过管线零 hints），x5-x7 是伤疒话术卡（仪表盘/HTML 容错/列表点选）。
    "x0_ctl.xml",         "x1_table.xml",       "x2_menu.xml",       "x3_fold.xml",
    "x4_confirm.xml",     "x5_dashboard.xml",   "x6_html.xml",       "x7_alarms.xml",
};
constexpr int kNumCorpusFiles = static_cast<int>(sizeof(kCorpusFiles) / sizeof(kCorpusFiles[0]));

// 11 张负例语料（sim/tests/corpus/negative/*.json），badcards 命令逐个渲染 + 断言全部被拒。
constexpr const char* kCorpusNegFiles[] = {
    "neg_bind_rows_missing_item.json",     "neg_choice_single_option.json",
    "neg_fmt_percent_s_numeric_bind.json", "neg_grid_multiple_forms.json",
    "neg_nested_container.json",           "neg_numeric_control_string_bind.json",
    "neg_qrcode_too_long.json",            "neg_root_not_array.json",
    "neg_too_many_grids.json",             "neg_too_many_nodes.json",
    "neg_unknown_bind.json",
};
constexpr int kNumCorpusNegFiles = static_cast<int>(sizeof(kCorpusNegFiles) / sizeof(kCorpusNegFiles[0]));

// .xml 语料 → ui_render args：整个文件内容进 {"xml":"…"}（cJSON 负责转义），与真机上
// LLM 走 xml 通道的 args 形状一字不差。
std::string WrapXmlArgs(const std::string& xml) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "xml", xml.c_str());
    char* s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    free(s);
    cJSON_Delete(o);
    return out;
}

// 惰性加载 + 缓存全部正例文本（F9 demo 每次按需读一次即可，量很小，不必每帧重读）。
std::vector<std::string>& CorpusCards() {
    static std::vector<std::string> cards;
    if (cards.empty()) {
        cards.reserve(kNumCorpusFiles);
        for (int i = 0; i < kNumCorpusFiles; i++) {
            std::string body = ReadCorpusFile(kCorpusFiles[i]);
            const std::string name(kCorpusFiles[i]);
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0 && !body.empty())
                body = WrapXmlArgs(body);
            cards.push_back(std::move(body));
        }
    }
    return cards;
}

// v2 版 hints 演示卡（B 验收 §4 断言 7 的迁移版）：2 个 primary 按钮 + 无 on_click 的死按钮 +
// 无 text/bind/bind_data 的空 label + on_change 挂 report 的死 slider——一次触发 Lint 的多条
// 规则（primary>1 / 空 label / 死控件 / on_change 挂 report），验证 hints 数组非阻断地搭在
// render 返回值里（见 pi_card_render.cc 重写后的 Lint()，F3）。
constexpr const char* kCardHints =
    "{\"display\":\"overlay\",\"root\":["
    "{\"cells\":[{\"type\":\"label\",\"text\":\"\"},"
    "{\"type\":\"slider\",\"on_change\":[{\"do\":\"report\",\"text\":\"{v}\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"A\"},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"B\"}]}"
    "]}";

// TEMP SCAFFOLD（B 验收 §4 断言 18 的可写 bind 半支，v2 迁移版）：choice 绑到可写 bool 路径
// ui.theme（0=dark/1=light，正好落在 choice 的 2 项区间内），点第二段应立即回写主题、屏幕跟着
// 变色。
constexpr const char* kCardChoiceBind =
    "{\"display\":\"overlay\",\"root\":["
    "{\"cells\":[{\"type\":\"label\",\"role\":\"title\",\"text\":\"主题\"}]},"
    "{\"cells\":[{\"type\":\"choice\",\"bind\":\"ui.theme\",\"options\":[\"深色\",\"浅色\"]}]}"
    "]}";

// TEMP SCAFFOLD (overlay reflow re-entrancy check): a 20-row overlay card where every
// row carries an id + starts individually hidden/visible, so a single `showrows <n>`
// command can grow/shrink the rendered content across the 86%-height cap through the
// real ui_update tool path — exercising pi_card::ReflowOverlay's reentrant branch
// (fixed-height+scroll <-> SIZE_CONTENT) the same way ApplyProps would trigger it.
std::string BuildGrowCard() {
    static const char* icons[] = {"volume", "sun", "battery", "wifi", "gear", "clock", "info", "music"};
    std::string s = "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":[";
    s += "{\"type\":\"label\",\"role\":\"title\",\"text\":\"reflow 重入测试\"},";
    const int rows = 20;  // 全部展开时自然高度 > 86% 屏高；前 8 行默认可见时远低于封顶。
    for (int i = 0; i < rows; i++) {
        char row[220];
        std::snprintf(row, sizeof(row),
                      "{\"type\":\"row\",\"id\":\"row%d\",\"hidden\":%s,\"children\":"
                      "[{\"type\":\"icon\",\"icon\":\"%s\"},{\"type\":\"label\",\"role\":\"value\","
                      "\"text\":\"ROW %02d\"}]}%s",
                      i, i < 8 ? "false" : "true", icons[i % 8], i, i < rows - 1 ? "," : "");
        s += row;
    }
    s += "]}}";
    return s;
}

void RenderGrowCard() {
    std::string spec = BuildGrowCard();
    cJSON* args = cJSON_Parse(spec.c_str());
    if (!args) {
        std::fprintf(stderr, "[sim] growcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] growcard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD: show rows [0,n) and hide [n,20) of the growcard via the real
// ui_update tool path (pi_ui_queue -> DrainQueueTick on the LVGL thread, same route
// the LLM's ui_update calls take) — not a shortcut into pi_card_host internals.
void ShowRows(int n) {
    for (int i = 0; i < 20; i++) {
        cJSON* args = cJSON_CreateObject();
        char id[16];
        std::snprintf(id, sizeof(id), "row%d", i);
        cJSON_AddStringToObject(args, "id", id);
        cJSON* props = cJSON_AddObjectToObject(args, "props");
        cJSON_AddBoolToObject(props, "hidden", i >= n);
        bool is_err = false;
        char* res = pi_card_tool_update(args, &is_err);
        free(res);
        cJSON_Delete(args);
    }
}

// TEMP SCAFFOLD (改造10 acceptance): render a card with a label bound to the writable Int path
// audio.volume — used with `numset` below to observe the manual-observer + 250ms interpolation
// added in pi_card_render.cc's ApplyBind (replacing the native lv_label_bind_text one-shot jump
// for non-String binds) — plus a second label bound to the String path net.ssid, to confirm the
// String branch still uses the untouched native lv_label_bind_text (no animation, no crash/tofu).
// Also exercises the entrance fade+slide (PlayCardEntrance, pi_card_render.cc) since it's an
// overlay render, with id "vol" left on the card root so `ui_close` can target it for the
// mid-animation-delete regression check.
void RenderNumAnimCard() {
    static const char* kSpec =
        "{\"display\":\"overlay\",\"card\":\"vol\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
        "{\"type\":\"label\",\"role\":\"title\",\"text\":\"数值滚动测试\"},"
        "{\"type\":\"label\",\"id\":\"vol\",\"role\":\"value\",\"bind\":\"audio.volume\","
        "\"fmt\":\"%d%%\",\"mono\":true},"
        "{\"type\":\"label\",\"role\":\"caption\",\"bind\":\"net.ssid\",\"fmt\":\"%s\"}]}}";
    cJSON* args = cJSON_Parse(kSpec);
    if (!args) {
        std::fprintf(stderr, "[sim] numanimcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] numanimcard render: %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (改造10 acceptance): numset <path> <value> — writes the DataHub subject
// directly (bypassing touch/slider drag) so the manual observer's 250ms interpolation fires
// deterministically at a known moment for screenshot timing.
void ExecNumSet(const std::string& path, int value) {
    lv_subject_t* subj = pi_card::DataHub::Instance().Acquire(path);
    if (!subj) {
        std::fprintf(stderr, "[sim] numset: unknown path '%s'\n", path.c_str());
        return;
    }
    lv_subject_set_int(subj, value);
    std::fprintf(stderr, "[sim] numset %s -> %d\n", path.c_str(), value);
}

// TEMP SCAFFOLD (改造10 acceptance c): strset <path> <text> — direct subject write for String
// paths (net.ssid has no setter, so ExecNumSet's DataHub::Write path doesn't apply here);
// confirms the untouched native lv_label_bind_text branch still renders String binds correctly
// (including CJK, exercising the SafeFont mono->puhui fallback) with no animation.
void ExecStrSet(const std::string& path, const std::string& text) {
    lv_subject_t* subj = pi_card::DataHub::Instance().Acquire(path);
    if (!subj) {
        std::fprintf(stderr, "[sim] strset: unknown path '%s'\n", path.c_str());
        return;
    }
    lv_subject_copy_string(subj, text.c_str());
    std::fprintf(stderr, "[sim] strset %s -> '%s'\n", path.c_str(), text.c_str());
}

// TEMP SCAFFOLD (改造1 acceptance #2): previewfeed <file> — feeds a sequence of partial-JSON
// snapshots (one per line, each the FULL accumulated args-so-far, matching how
// pi_agent_task.c's UI_TOOL_ARGS actually behaves) straight into
// pi_card::PreviewOnArgs, bypassing the queue/drain-tick entirely — deterministic, no real LLM
// needed. Calls PreviewOnToolStart("ui_render") once up front (NOT per line — doing it per line
// would tear down the very session we're trying to grow). After each line, logs the tree root
// and its child-0 pointer so a human/script can diff across frames and confirm committed
// subtrees keep the same lv_obj_t* (the acceptance criterion).
void DumpPreviewNode(lv_obj_t* obj, int depth);  // 前向声明：定义见下方（迟到属性取证要逐帧转储）

void ExecPreviewFeed(const std::string& path, int max_lines = -1) {
    std::ifstream f(path);
    if (!f.good()) {
        std::fprintf(stderr, "[sim] previewfeed: can't open '%s'\n", path.c_str());
        return;
    }
    pi_card::PreviewOnToolStart("ui_render");
    uint32_t gen = pi_agent_task_session_gen();  // 传真实当前代次，避免下个 drain tick 被判过期撤除
    std::string line;
    int frame = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (max_lines > 0 && frame >= max_lines) break;  // previewscene 截断喂帧数
        frame++;
        pi_card::PreviewOnArgs(line.c_str(), gen);
        lv_obj_t* tree = pi_card::PreviewDebugTree();
        lv_obj_t* child0 = (tree && lv_obj_get_child_count(tree) > 0) ? lv_obj_get_child(tree, 0) : nullptr;
        std::fprintf(stderr, "[sim][previewfeed] frame %d: tree=%p child0=%p children=%u\n", frame,
                     static_cast<void*>(tree), static_cast<void*>(child0),
                     tree ? lv_obj_get_child_count(tree) : 0);
        // 迟到属性修复取证（任务分配 #2）：每帧转储一次，逐帧 diff 才能看出 role/grow/justify
        // 这些容器/叶子属性是不是"到帧就生效"而不是憋到 adopt 才跳变。
        std::fprintf(stderr, "[sim][previewfeed] frame %d dump:\n", frame);
        DumpPreviewNode(tree, 0);
    }
}

// TEMP SCAFFOLD (verify-m1 counterexample; extended for the 迟到属性 fix acceptance): recursively
// dump the live preview tree so a human can eyeball structure (order/duplicates/rebuilds) AND the
// layout-affecting properties that are supposed to now update mid-growth instead of only at
// adopt: label font pointer (role → font swap), computed pixel width + flex_grow (grow:1), and
// the container's flex main/cross place enum (justify/align).
void DumpPreviewNode(lv_obj_t* obj, int depth) {
    if (!obj) return;
    const char* kind = "obj";
    char extra[192] = "";
    int off = 0;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        kind = "label";
        off += std::snprintf(extra + off, sizeof(extra) - off, " text=\"%s\" font=%p",
                              lv_label_get_text(obj),
                              static_cast<const void*>(lv_obj_get_style_text_font(obj, LV_PART_MAIN)));
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        kind = "button";
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        kind = "slider";
    }
    off += std::snprintf(extra + off, sizeof(extra) - off,
                          " w=%d grow=%u main=%d cross=%d radius=%d hidden=%d",
                          static_cast<int>(lv_obj_get_width(obj)),
                          static_cast<unsigned>(lv_obj_get_style_flex_grow(obj, LV_PART_MAIN)),
                          static_cast<int>(lv_obj_get_style_flex_main_place(obj, LV_PART_MAIN)),
                          static_cast<int>(lv_obj_get_style_flex_cross_place(obj, LV_PART_MAIN)),
                          static_cast<int>(lv_obj_get_style_radius(obj, LV_PART_MAIN)),
                          lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);
    std::fprintf(stderr, "[sim][previewdump] %*s%s %p%s\n", depth * 2, "", kind,
                 static_cast<void*>(obj), extra);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) DumpPreviewNode(lv_obj_get_child(obj, i), depth + 1);
}
void ExecPreviewDump() {
    lv_obj_t* tree = pi_card::PreviewDebugTree();
    std::fprintf(stderr, "[sim][previewdump] === tree root=%p ===\n", static_cast<void*>(tree));
    DumpPreviewNode(tree, 0);
}

// TEMP SCAFFOLD (改造1 acceptance #4/#5): previewend / bargein — drive the two non-adopt
// teardown paths without needing a real LLM round-trip. previewend calls PreviewOnToolEnd
// directly (simulates "tool execute finished, no UI_CARD_RENDER followed" — e.g. validation
// failed); bargein calls pi_agent_task_new_session() directly (simulates a barge-in/new session
// bumping the session gen mid-stream) — the actual teardown then happens on the next
// DrainQueueTick via PreviewCheckGen, same as the real path.
void ExecPreviewEnd() {
    pi_card::PreviewOnToolEnd();
    std::fprintf(stderr, "[sim] previewend: tree=%p\n", static_cast<void*>(pi_card::PreviewDebugTree()));
}
void ExecBargeIn() {
    pi_agent_task_new_session();
    std::fprintf(stderr, "[sim] bargein: session gen bumped\n");
}

// TEMP SCAFFOLD: previewscene <frames_file> <n_lines> <shot_path> — "流式渲染到一半 -> 截图"
// 一条龙取证：先兜底撤除任何存活预览（PreviewTeardown 内部已判断 active，未活着时是空操作），
// 强制切到 Chat 视图（预览树只在 Chat 可见），喂前 n_lines 帧（<=0 = 全部）后不收尾
// （预览树留在屏上），然后交给 Pump() 里的延迟计数器在几个主循环迭代后截图 —— 喂完当帧
// LVGL 布局还没跑，立即截图会拍到上一帧。
void ExecPreviewScene(const std::string& path, int n_lines, const std::string& shot_path) {
    pi_card::PreviewTeardown();
    PiScreen::DebugGoChat();
    ExecPreviewFeed(path, n_lines);
    g_previewscene_shot_path = shot_path;
    g_previewscene_countdown = 4;
}

// TEMP SCAFFOLD (CARD V2 §8 步骤4 预览验收): previewscenechunks <json_file> <shot_path> — 直接
// 对一份**完整** ui_render 信封 JSON 文件（如 sim/tests/corpus/*.json）做真实字节级流式回放：
// 用 pi-c 的 pi_partial_json_parse_streaming（与真机 pi_agent_task.c 走同一份实现，见
// pi_provider_util.c）对文本的每个前缀重新解析出一棵 partial 快照，喂给 pi_card::PreviewOnArgs
// ——比 previewfeed 更贴近真实场景：不需要人工准备逐帧 frames 文件，直接从完整 JSON 反推。
// 语料卡目前都是 display:"overlay"（sim/tests/corpus 迁移自 v1 demo 卡，用于 solver/校验测试），
// 但 v2 预览只在 chat 模式生长（overlay/standby 走各自的一次性渲染，没有流式预览）——这里把
// display 字段临时改写成 "chat" 再喂流，专门用来验证"预览 v2 生长"这个特性本身的视觉效果，
// 不代表这些卡在产品里真的以 chat 形态出现（如实记录于任务报告 e）。
void ExecPreviewSceneChunks(const std::string& path, const std::string& shot_path) {
    std::ifstream f(path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        std::fprintf(stderr, "[sim] previewscenechunks: can't read '%s'\n", path.c_str());
        return;
    }
    cJSON* envelope = cJSON_Parse(text.c_str());
    if (!envelope) {
        std::fprintf(stderr, "[sim] previewscenechunks: invalid JSON in '%s'\n", path.c_str());
        return;
    }
    cJSON_ReplaceItemInObject(envelope, "display", cJSON_CreateString("chat"));
    char* forced = cJSON_PrintUnformatted(envelope);
    std::string chat_text = forced ? forced : text;
    if (forced) cJSON_free(forced);
    cJSON_Delete(envelope);

    pi_card::PreviewTeardown();
    PiScreen::DebugGoChat();
    pi_card::PreviewOnToolStart("ui_render");
    uint32_t gen = pi_agent_task_session_gen();
    for (size_t i = 1; i <= chat_text.size(); ++i) {
        std::string prefix = chat_text.substr(0, i);
        cJSON* snap = pi_partial_json_parse_streaming(pi_alloc_default(), prefix.c_str());
        char* s = cJSON_PrintUnformatted(snap);
        if (s) {
            pi_card::PreviewOnArgs(s, gen);
            cJSON_free(s);
        }
        cJSON_Delete(snap);
    }
    lv_obj_t* tree = pi_card::PreviewDebugTree();
    std::fprintf(stderr, "[sim][previewscenechunks] %s: tree=%p children=%u\n", path.c_str(),
                 static_cast<void*>(tree), tree ? lv_obj_get_child_count(tree) : 0);
    g_previewscene_shot_path = shot_path;
    g_previewscene_countdown = 4;
}

// TEMP SCAFFOLD (merge regression: preview vs. formal render visual A/B): rendercard <file> —
// read one raw ui_render JSON spec (a "card" id + "root", same shape RenderBadCard uses) from a
// file and push it through the real (non-preview) pi_card_tool_render path, so a previewfeed
// frame's final JSON can be screenshotted twice — once via the preview tree, once via the formal
// tree — for a pixel comparison.
void ExecRenderJson(const std::string& path) {
    std::ifstream f(path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    cJSON* args = cJSON_Parse(text.c_str());
    if (!args) {
        std::fprintf(stderr, "[sim] rendercard: JSON parse failed for '%s'\n", path.c_str());
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim][rendercard] -> %s (%s)\n", res ? res : "(null)", is_err ? "ERR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (改造10 acceptance): closecard <id> — real ui_close, used to check that
// deleting a card while its entrance/num-scroll lv_anim_t is still in flight doesn't crash
// (regression for PlayCardEntrance/NumScrollObserverCb using the label/tree object itself as
// the anim var, relying on LVGL's auto-cleanup-on-delete instead of a manual DELETE callback).
void ExecCloseCard(const std::string& id) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", id.c_str());
    bool is_err = false;
    char* res = pi_card_tool_close(args, &is_err);
    std::fprintf(stderr, "[sim] closecard %s: %s (%s)\n", id.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (ui_update batch `patches` acceptance): one real ui_update call against
// BuildGrowCard()'s row0..row19 with a `patches` array — 2 existing ids (row0/row1, both
// currently visible) + 1 missing id, to see 2 nodes flip hidden in a single call plus one
// aggregated async error, via the same real tool path ShowRows() exercises per-id.
void ExecPatchTest() {
    cJSON* args = cJSON_CreateObject();
    cJSON* patches = cJSON_AddArrayToObject(args, "patches");
    auto add_patch = [&](const char* id, bool hidden) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", id);
        cJSON* props = cJSON_AddObjectToObject(p, "props");
        cJSON_AddBoolToObject(props, "hidden", hidden);
        cJSON_AddItemToArray(patches, p);
    };
    add_patch("row0", true);
    add_patch("row1", true);
    add_patch("rowMissing", true);  // does not exist -> aggregated async error
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] patchtest ui_update: %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (P1 report 状态快照 + P2 本地 toggle 披露)：一张表单卡——
//   * qty(slider) / urgent(switch)：无 bind 只有 id 的**纯本地表单控件**。既验证它们没被
//     「死控件兜底」DIM 掉（pi_card_render.cc 的 live 判据第 4 条），也验证 report 会自动
//     带回它们的值。
//   * vol(slider)：bind 到硬件路径，验证 bind 控件的值也搭 report 顺风车回传。
//   * 「查看详情」按钮 → {do:'toggle',target:'detail'}：纯本地展开/收起，零 LLM 往返，且会
//     触发 overlay 卡的 ReflowOverlay 重入。
//   * 「确认下单」按钮 → {do:'report'}：看注入文本是否带全状态。
constexpr const char* kFormCard =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"订单\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"确认下单\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"数量\",\"grow\":1},"
    "{\"type\":\"slider\",\"id\":\"qty\",\"min\":1,\"max\":10,\"value\":3,\"grow\":2}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"加急\",\"grow\":1},"
    "{\"type\":\"switch\",\"id\":\"urgent\",\"checked\":false}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"音量\",\"grow\":1},"
    "{\"type\":\"slider\",\"id\":\"vol\",\"bind\":\"audio.volume\",\"grow\":2}]},"
    "{\"type\":\"button\",\"text\":\"查看详情\",\"variant\":\"ghost\","
    "\"on_click\":[{\"do\":\"toggle\",\"target\":\"detail\"}]},"
    // 详情块特意做大：展开后自然高度**跨过** 86% 屏高的封顶，好让 toggle 走一遍
    // ReflowOverlay 的「SIZE_CONTENT ↔ 钉死+开滚动」切换。注意这条链路是
    // DispatchCb → ReflowOverlay，与 ui_update → OnUpdateEvent 那条（growcard/showrows 测的）
    // 是**不同的调用点**，两边都得验。
    "{\"type\":\"column\",\"id\":\"detail\",\"hidden\":true,\"gap\":6,\"children\":["
    "{\"type\":\"divider\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"预计送达 30 分钟\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"配送费 5 元\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"支持 7 天无理由退换\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"商家：茶话弄（软件园店）\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"骑手：王师傅 · 距您 1.2 公里\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"下单时间：今天 14:32\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"优惠：满 20 减 3\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"备注：少糖、去冰\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"发票：电子普通发票\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"客服电话：400-123-4567\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"订单号：2026071500391\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"配送方式：专送\"}]},"
    "{\"type\":\"button\",\"text\":\"确认下单\",\"variant\":\"primary\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"确认下单\"}]}]}}";

// ============================================================================
// Phase2 演示卡：spec/data 分离 + list 控件 + preset + 字符串回流。
// ============================================================================

// T1/T4（v2 迁移：list -> bind_rows，§6.2 修复表）：data 驱动的动态行——5 条待办，行模板
// {n}. {item.name} + role=value 的 {item.price}。固定 card id "datacard" 供 dataop 命令后续
// ui_update（bind_rows 的行为语义与 v1 list 完全一致——SubstRecord 复用，dataop 的
// append/remove/replace/set 不用变）。
constexpr const char* kCardList =
    "{\"card\":\"datacard\",\"display\":\"overlay\",\"data\":{\"items\":["
    "{\"name\":\"苹果\",\"price\":\"12\"},{\"name\":\"香蕉\",\"price\":\"6\"},"
    "{\"name\":\"橙子\",\"price\":\"9\"},{\"name\":\"葡萄\",\"price\":\"20\"},"
    "{\"name\":\"西瓜\",\"price\":\"30\"}]},"
    "\"root\":["
    "{\"cells\":[{\"type\":\"label\",\"role\":\"title\",\"text\":\"购物清单\"}]},"
    // max:8 显式高于初始 5 条：eff_max 在 render 期一次性算定，append 到第 6/7/8 条才会真正
    // 多出一行；不给 max 则默认 eff_max=初始长度，append 会被截断，但那样就演示不出"截图
    // 行数 +1"，故这里显式留出余量。
    "{\"item\":[{\"type\":\"label\",\"text\":\"{n}. {item.name}\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"¥{item.price}\"}],"
    "\"bind_rows\":\"items\",\"max\":8,\"empty\":\"清单为空\"}"
    "]}";

// 改造4（v2 迁移）：行模板不含 {i}/{n}（只用 {item.*}）——tpl_uses_index 应算 false，走行级
// fast path。max:4、初始 2 条：append 到 3/4 条正常新增行，第 3 次 append（会让底层数组到 5
// 条）应该"截断区不补行"（可见行数钉在 4，不再新增）。固定 card id "datacard2"，供
// listfastop 命令用。
constexpr const char* kCardListFast =
    "{\"card\":\"datacard2\",\"display\":\"overlay\",\"data\":{\"items\":["
    "{\"name\":\"苹果\",\"price\":\"12\"},{\"name\":\"香蕉\",\"price\":\"6\"}]},"
    "\"root\":["
    "{\"cells\":[{\"type\":\"label\",\"role\":\"title\",\"text\":\"快路径清单\"}]},"
    "{\"item\":[{\"type\":\"label\",\"text\":\"{item.name}\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"¥{item.price}\"}],"
    "\"bind_rows\":\"items\",\"max\":4,\"empty\":\"清单为空\"}"
    "]}";

// T7：choice 的 on_change 直接 report "你选了{label}"——{label} 只在**触发控件本身就是
// choice** 时有值（ChoiceLabel(target,...) 里 target=触发控件；隔壁按钮点它是取不到 choice
// 的 label 的，故本卡把 report 挂在 choice 自己身上，而非另一个确认按钮上）。
constexpr const char* kCardChoiceLabel =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"口味\"},"
    "{\"type\":\"choice\",\"id\":\"flavor\",\"options\":[\"甜\",\"辣\",\"酸\"],\"value\":0,"
    "\"on_change\":[{\"do\":\"report\",\"text\":\"你选了{label}\"}]}]}}";

// T8：data.status + bind_data label（text 带 {value} 内联模板），固定 id 供 dataop set。
constexpr const char* kCardDataLabel =
    "{\"card\":\"datalabel\",\"display\":\"overlay\",\"data\":{\"status\":\"备餐中\"},"
    "\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"订单状态\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind_data\":\"status\",\"text\":\"状态：{value}\"}]}}";

void RenderBadCard(const char* spec, const char* tag) {
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] %s JSON parse failed\n", tag);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim][negative] %-10s -> %s (%s)\n", tag, res ? res : "(null)",
                 is_err ? "已拒绝 ✓" : "竟然通过了 ✗");
    free(res);
    cJSON_Delete(args);
}

// ============================================================================
// Phase3 演示卡：常驻小组件（display:'standby'）+ invoke 命令 + chart。
// ============================================================================

// 常驻卡：电量数值 + battery.level 历史折线 + 两个 invoke 按钮（safe/confirm 各一）。
// 演示 display:'standby' 的完整能力面——pin host 布局、chart 历史绑定、invoke 分发。
constexpr const char* kCardStandby =
    "{\"display\":\"standby\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"电量\"},"
    "{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"battery.level\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"chart\",\"bind_history\":\"battery.level\",\"points\":30},"
    "{\"type\":\"row\",\"gap\":10,\"children\":["
    "{\"type\":\"button\",\"text\":\"重连网络\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"net.reconnect\"}]},"
    "{\"type\":\"button\",\"text\":\"切换网络\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"net.switch_type\"}]}"
    "]}]}}";

// TEMP SCAFFOLD（消息流「贴底跟随」验证）：往 chat feed 里追加一张普通卡片（非 overlay）。
// 每追加一张都会走 CardEndRow → ScrollFeedToBottom(false)——即模型侧输出的那条跟随分支，
// 于是不必真的调 LLM 就能验证「用户翻上去后新输出不抢视口」。
void RenderChatCard(int n) {
    char spec[512];
    std::snprintf(spec, sizeof(spec),
                  "{\"display\":\"chat\",\"root\":{\"type\":\"column\",\"gap\":6,\"children\":["
                  "{\"type\":\"label\",\"role\":\"title\",\"text\":\"消息 #%d\"},"
                  "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"这是第 %d 条追加进消息流的卡片\"}]}}",
                  n, n);
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] chatcard %d JSON parse failed\n", n);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    free(res);
    cJSON_Delete(args);
}

void RenderFormCard() {
    cJSON* args = cJSON_Parse(kFormCard);
    if (!args) {
        std::fprintf(stderr, "[sim] formcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] formcard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（按钮 icon 支持验收）：三个纯图标播控钮 + icon+text 并存钮 +
// 两个负例（生造图标名 / 空白按钮）——正例看渲染，负例看 hints 是否当场纠正。
void RenderIconButtonCard() {
    static const char* kSpec =
        "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":8,\"children\":["
        "{\"type\":\"label\",\"role\":\"title\",\"text\":\"图标按钮验证\"},"
        "{\"type\":\"row\",\"gap\":8,\"children\":["
        "{\"type\":\"button\",\"icon\":\"skip-back\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.prev\"}]},"
        "{\"type\":\"button\",\"icon\":\"play\",\"variant\":\"primary\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.toggle\"}]},"
        "{\"type\":\"button\",\"icon\":\"skip-forward\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.next\"}]}]},"
        "{\"type\":\"row\",\"gap\":8,\"children\":["
        "{\"type\":\"button\",\"icon\":\"list-music\",\"text\":\"列表\",\"variant\":\"ghost\"},"
        "{\"type\":\"button\",\"icon\":\"totally-made-up\",\"text\":\"坏名\"},"
        "{\"type\":\"button\"}]}]}}";
    cJSON* args = cJSON_Parse(kSpec);
    if (!args) {
        std::fprintf(stderr, "[sim] iconcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] iconcard render: %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// Phase3：渲染常驻小组件演示卡（display:'standby'）。
void RenderStandbyCard() {
    cJSON* args = cJSON_Parse(kCardStandby);
    if (!args) {
        std::fprintf(stderr, "[sim] standby JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] standby render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// Phase3 D8（verifier 修复后）：封套 >3072B 的 standby 现在在 worker 侧
// pi_card_tool_render 同步拒绝（入队前），不再入队、不再触碰既有 pin——构造一个超长
// label 文本把 spec 撑过 3KB，断言 is_error=true 且既有 pin（如果有）分毫未动。
void RenderStandbyOversized() {
    std::string huge(3200, 'x');
    std::string spec = "{\"display\":\"standby\",\"root\":{\"type\":\"column\",\"children\":["
                       "{\"type\":\"label\",\"text\":\"" + huge + "\"}]}}";
    cJSON* args = cJSON_Parse(spec.c_str());
    if (!args) {
        std::fprintf(stderr, "[sim] standbybig JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] standbybig render: %s (%s) — expect ERROR, synchronous, no NVS "
                        "write, no drain-side rollback\n",
                res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

void RenderDemoCard() {
    const char* idx_env = std::getenv("PI_SIM_CARD_IDX");
    int idx = idx_env ? std::atoi(idx_env) : 0;
    std::vector<std::string>& cards = CorpusCards();
    const int n_static = static_cast<int>(cards.size());
    if (n_static == 0) {
        std::fprintf(stderr, "[sim] demo card: corpus not found (CorpusDir='%s')\n",
                     CorpusDir().c_str());
        return;
    }
    // 越界（含原 v1 "超高卡" 兜底槽——25_grid_tall.json 已在语料第 22 位覆盖同一验收目的，
    // §5.2 idx25）：钳到最后一张，而不是渲染一张越界卡。
    if (idx < 0) idx = 0;
    if (idx >= n_static) idx = n_static - 1;
    const char* spec = cards[idx].c_str();
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] demo card %d JSON parse failed\n", idx);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] demo card %d render: %s (%s)\n", idx, res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T1/T4）：渲染 data 驱动的 kCardList（固定 card id "datacard"）。
void RenderDataCard() {
    cJSON* args = cJSON_Parse(kCardList);
    if (!args) {
        std::fprintf(stderr, "[sim] datacard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] datacard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance）：渲染 kCardListFast（固定 card id "datacard2"，行模板不含
// {i}/{n}，走行级 fast path）。
void RenderDataCardFast() {
    cJSON* args = cJSON_Parse(kCardListFast);
    if (!args) {
        std::fprintf(stderr, "[sim] datacard2 JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] datacard2 render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance）：listfastop <append|remove|replace|set> [index] — 走真实
// ui_update 的 data 通道，对准 "datacard2" 卡的 items 数组。append 尾插一条；remove/replace
// 需要 index（remove 缺省 0，replace 缺省 0）；set 整键换成全新数组（validate"退全量"用）。
void ExecListFastOp(const std::string& op, int index) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard2");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    if (op == "append") {
        cJSON* append = cJSON_AddObjectToObject(data, "append");
        cJSON_AddStringToObject(append, "key", "items");
        cJSON* item = cJSON_AddObjectToObject(append, "item");
        cJSON_AddStringToObject(item, "name", "新品");
        cJSON_AddStringToObject(item, "price", "1");
    } else if (op == "remove") {
        cJSON* remove = cJSON_AddObjectToObject(data, "remove");
        cJSON_AddStringToObject(remove, "key", "items");
        cJSON_AddNumberToObject(remove, "index", index);
    } else if (op == "replace") {
        cJSON* replace = cJSON_AddObjectToObject(data, "replace");
        cJSON_AddStringToObject(replace, "key", "items");
        cJSON_AddNumberToObject(replace, "index", index);
        cJSON* item = cJSON_AddObjectToObject(replace, "item");
        cJSON_AddStringToObject(item, "name", "换了");
        cJSON_AddStringToObject(item, "price", "99");
    } else if (op == "set") {
        cJSON* set = cJSON_AddObjectToObject(data, "set");
        cJSON* arr = cJSON_AddArrayToObject(set, "items");
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", "全新数组");
        cJSON_AddStringToObject(item, "price", "0");
        cJSON_AddItemToArray(arr, item);
    } else {
        std::fprintf(stderr, "[sim] listfastop 未知操作 '%s'（用 append|remove|replace|set）\n",
                     op.c_str());
        cJSON_Delete(args);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] listfastop %s idx=%d -> %s (%s)\n", op.c_str(), index,
                 res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance #6）：一次 ui_update 里同时塞 append+replace（同 key），验证
// "多 op 合并"——两条 op 都该在这一次调用里正确生效（append 尾插一条，replace 换掉指定行）。
void ExecListFastMultiAppendReplace() {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard2");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    cJSON* append = cJSON_AddObjectToObject(data, "append");
    cJSON_AddStringToObject(append, "key", "items");
    cJSON* aitem = cJSON_AddObjectToObject(append, "item");
    cJSON_AddStringToObject(aitem, "name", "多op追加");
    cJSON_AddStringToObject(aitem, "price", "2");
    cJSON* replace = cJSON_AddObjectToObject(data, "replace");
    cJSON_AddStringToObject(replace, "key", "items");
    cJSON_AddNumberToObject(replace, "index", 0);
    cJSON* ritem = cJSON_AddObjectToObject(replace, "item");
    cJSON_AddStringToObject(ritem, "name", "多op换首行");
    cJSON_AddStringToObject(ritem, "price", "3");
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] listfastmulti append+replace -> %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance #6）：一次 ui_update 里同时塞 append+set（同 key）——
// ApplyDataOps 按 set→append 的固定顺序落地两条操作（各自独立生效在 card->data 上：
// set 先把数组整个换成新内容，append 再往这个新数组末尾追一条，两条的数据效果都真实生效，
// 都能在最终数组里看到），但 RefreshDataConsumers 只因为出现了 SetWhole 就整个 key 走一次
// RefreshListFull，不会再额外为 append 单独跑一次 fast path——重建时直接读的是这一刻
// card->data 的最终值，天然已经含着 append 的效果，没有"跳过 append 的数据"这回事，只是
// "不会为它多做一次行级增量"。
void ExecListFastMultiAppendSet() {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard2");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    cJSON* append = cJSON_AddObjectToObject(data, "append");
    cJSON_AddStringToObject(append, "key", "items");
    cJSON* aitem = cJSON_AddObjectToObject(append, "item");
    cJSON_AddStringToObject(aitem, "name", "会被set吞掉");
    cJSON_AddStringToObject(aitem, "price", "4");
    cJSON* set = cJSON_AddObjectToObject(data, "set");
    cJSON* arr = cJSON_AddArrayToObject(set, "items");
    cJSON* sitem = cJSON_CreateObject();
    cJSON_AddStringToObject(sitem, "name", "set最终态");
    cJSON_AddStringToObject(sitem, "price", "5");
    cJSON_AddItemToArray(arr, sitem);
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] listfastmulti append+set -> %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T8）：渲染 bind_data label 演示卡（固定 card id "datalabel"）。
void RenderDataLabelCard() {
    cJSON* args = cJSON_Parse(kCardDataLabel);
    if (!args) {
        std::fprintf(stderr, "[sim] datalabel JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] datalabel render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T4）：走真实 ui_update 的 data 通道——dataop <append|remove>，
// 对准 "datacard" 卡的 items 数组。append 加一条，remove 删第 0 条。
void ExecDataOp(const std::string& op) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    if (op == "append") {
        cJSON* append = cJSON_AddObjectToObject(data, "append");
        cJSON_AddStringToObject(append, "key", "items");
        cJSON* item = cJSON_AddObjectToObject(append, "item");
        cJSON_AddStringToObject(item, "name", "新品");
        cJSON_AddStringToObject(item, "price", "1");
    } else if (op == "remove") {
        cJSON* remove = cJSON_AddObjectToObject(data, "remove");
        cJSON_AddStringToObject(remove, "key", "items");
        cJSON_AddNumberToObject(remove, "index", 0);
    } else {
        std::fprintf(stderr, "[sim] dataop 未知操作 '%s'（用 append|remove）\n", op.c_str());
        cJSON_Delete(args);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] dataop %s -> %s (%s)\n", op.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// Commit 3 G4：list 行模板 = grid（固定 id "gridlist"），验模板记账 + 重渲 GridDsc 无泄漏。
constexpr const char* kCardGridList =
    "{\"card\":\"gridlist\",\"display\":\"overlay\",\"data\":{\"rows\":["
    "{\"k\":\"甲\",\"v\":\"1\"},{\"k\":\"乙\",\"v\":\"2\"}]},"
    "\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"list-of-grid\"},"
    "{\"type\":\"list\",\"bind_data\":\"rows\",\"max\":8,\"item\":{\"type\":\"grid\",\"cols\":[1,1],"
    "\"children\":[{\"type\":\"label\",\"text\":\"{item.k}\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"{item.v}\"}]}}]}}";

// 渲染 gridlist 后跑 N 轮 {append,remove} ui_update（每轮触发 list 重渲 = 删旧 grid 行 +
// 建新 grid 行）。GridDsc 净值应保持有界（≈当前行数），绝不随轮次单调增长——[griddsc]
// 日志给出 alloc/free 净值证据。
void ExecG4Leak(int rounds) {
    cJSON* rc = cJSON_Parse(kCardGridList);
    bool is_err = false;
    char* r0 = pi_card_tool_render(rc, &is_err);
    std::fprintf(stderr, "[sim] g4leak render -> %s (%s)\n", r0 ? r0 : "(null)", is_err ? "ERR" : "ok");
    free(r0);
    cJSON_Delete(rc);
    for (int i = 0; i < rounds; i++) {
        for (const char* op : {"append", "remove"}) {
            cJSON* args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "card", "gridlist");
            cJSON* data = cJSON_AddObjectToObject(args, "data");
            if (std::strcmp(op, "append") == 0) {
                cJSON* ap = cJSON_AddObjectToObject(data, "append");
                cJSON_AddStringToObject(ap, "key", "rows");
                cJSON* item = cJSON_AddObjectToObject(ap, "item");
                cJSON_AddStringToObject(item, "k", "丙");
                cJSON_AddStringToObject(item, "v", "9");
            } else {
                cJSON* rm = cJSON_AddObjectToObject(data, "remove");
                cJSON_AddStringToObject(rm, "key", "rows");
                cJSON_AddNumberToObject(rm, "index", 0);
            }
            bool e = false;
            char* res = pi_card_tool_update(args, &e);
            free(res);
            cJSON_Delete(args);
        }
    }
    std::fprintf(stderr, "[sim] g4leak done: %d rounds of {append,remove}\n", rounds);
}

// TEMP SCAFFOLD（Phase2 T8）：走真实 ui_update 的 data.set，改 "datalabel" 卡的 status 值。
void ExecDataLabelSet(const std::string& status) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datalabel");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    cJSON* set = cJSON_AddObjectToObject(data, "set");
    cJSON_AddStringToObject(set, "status", status.c_str());
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] datalabelset '%s' -> %s (%s)\n", status.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T5）：preset <confirm|form|dashboard|menu> —— 走真实 ui_render 的
// preset+slots 展开路径。四种各给一份最小可行 slots；传参非法名走负向分支验证具名错误串。
void RenderPreset(const std::string& name) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "display", "overlay");
    cJSON_AddStringToObject(args, "preset", name.c_str());
    cJSON* slots = cJSON_AddObjectToObject(args, "slots");
    if (name == "confirm") {
        cJSON_AddStringToObject(slots, "title", "清空历史?");
        cJSON_AddStringToObject(slots, "body", "该操作不可撤销。");
        cJSON* confirm = cJSON_AddObjectToObject(slots, "confirm");
        cJSON_AddStringToObject(confirm, "text", "清空");
        cJSON_AddStringToObject(confirm, "report", "确认清空历史");
    } else if (name == "form") {
        cJSON_AddStringToObject(slots, "title", "下单");
        cJSON* fields = cJSON_AddArrayToObject(slots, "fields");
        cJSON* qty = cJSON_CreateObject();
        cJSON_AddStringToObject(qty, "type", "slider");
        cJSON_AddStringToObject(qty, "id", "qty");
        cJSON_AddStringToObject(qty, "label", "数量");
        cJSON_AddNumberToObject(qty, "min", 1);
        cJSON_AddNumberToObject(qty, "max", 10);
        cJSON_AddNumberToObject(qty, "value", 3);
        cJSON_AddItemToArray(fields, qty);
        cJSON* urgent = cJSON_CreateObject();
        cJSON_AddStringToObject(urgent, "type", "switch");
        cJSON_AddStringToObject(urgent, "id", "urgent");
        cJSON_AddStringToObject(urgent, "label", "加急");
        cJSON_AddItemToArray(fields, urgent);
    } else if (name == "dashboard") {
        cJSON_AddStringToObject(slots, "title", "设备状态");
        cJSON* metrics = cJSON_AddArrayToObject(slots, "metrics");
        cJSON* m1 = cJSON_CreateObject();
        cJSON_AddStringToObject(m1, "label", "音量");
        cJSON_AddStringToObject(m1, "bind", "audio.volume");
        cJSON_AddStringToObject(m1, "kind", "bar");
        cJSON_AddStringToObject(m1, "fmt", "%d%%");
        cJSON_AddStringToObject(m1, "icon", "volume");
        cJSON_AddItemToArray(metrics, m1);
        cJSON* m2 = cJSON_CreateObject();
        cJSON_AddStringToObject(m2, "label", "电量");
        cJSON_AddStringToObject(m2, "bind", "battery.level");
        cJSON_AddStringToObject(m2, "fmt", "%d%%");
        cJSON_AddItemToArray(metrics, m2);
    } else if (name == "menu") {
        cJSON_AddStringToObject(slots, "title", "选择操作");
        cJSON* items = cJSON_AddArrayToObject(slots, "items");
        cJSON* i1 = cJSON_CreateObject();
        cJSON_AddStringToObject(i1, "text", "新建对话");
        cJSON_AddItemToArray(items, i1);
        cJSON* i2 = cJSON_CreateObject();
        cJSON_AddStringToObject(i2, "text", "导出记录");
        cJSON_AddItemToArray(items, i2);
    }
    // preset/slots 已随 v2 重构整删（docs/CARD_V2.md §3 决策 A）：这条命令与下面的
    // "presetbad" 命令都是 v1 遗留，如今只会走 Repair() 的"顶层 preset/slots 残留→拒绝"分支，
    // 不再有具名的 preset 校验错误串——保留命令仅为观察这条拒绝路径本身，不在本轮 v2 迁移
    // 范围内（sim/main.cc 授权改动清单未列出 preset/presetbad）。
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] preset %s -> %s (%s)\n", name.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

uint32_t TickCb() { return SDL_GetTicks(); }

// Runs inside SDL_PumpEvents — i.e. on the LVGL thread, under the LVGL lock
// (the SDL driver pumps from an lv_timer). No LVGL calls here; just record.
int EventWatch(void*, SDL_Event* ev) {
    switch (ev->type) {
        case SDL_QUIT:
            g_quit = true;
            break;
        case SDL_KEYDOWN:
            if (ev->key.keysym.sym == SDLK_F1) {
                g_pwr_key_held = true;  // PWR_KEY 按下（按住说话）
            } else if (ev->key.keysym.sym == SDLK_F12) {
                g_shot_pending = true;
            } else if (ev->key.keysym.sym == SDLK_F9) {
                g_demo_pending = true;  // pi_card 演示卡
            } else if (sim_asr_session_active()) {
                if (ev->key.keysym.sym == SDLK_BACKSPACE) sim_asr_backspace();
            }
            break;
        case SDL_KEYUP:
            if (ev->key.keysym.sym == SDLK_F1) g_pwr_key_held = false;  // 松开发送
            break;
        case SDL_TEXTINPUT:
            if (sim_asr_session_active()) sim_asr_type(ev->text.text);
            break;
        default:
            break;
    }
    return 0;
}

void Put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

// 32bpp top-down BMP; pixel data is LVGL XRGB8888 (B,G,R,X little-endian),
// which is exactly BMP's BGRX byte order.
bool WriteBmp32(const char* path, const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const uint32_t row_bytes = w * 4;
    const uint32_t img_bytes = row_bytes * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    Put32(hdr + 2, 54 + img_bytes);
    Put32(hdr + 10, 54);
    Put32(hdr + 14, 40);
    Put32(hdr + 18, w);
    Put32(hdr + 22, static_cast<uint32_t>(-static_cast<int32_t>(h)));
    hdr[26] = 1;
    hdr[28] = 32;
    Put32(hdr + 34, img_bytes);
    bool ok = std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (uint32_t y = 0; ok && y < h; y++) {
        ok = std::fwrite(data + static_cast<size_t>(y) * stride, 1, row_bytes, f) == row_bytes;
    }
    std::fclose(f);
    return ok;
}

const char* ShotPath() {
    const char* p = std::getenv("PI_SIM_SHOT");
    return (p != nullptr && p[0] != '\0') ? p : "pi_sim_shot.bmp";
}

void TakeScreenshot(const char* path) {
    lv_lock();
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
    if (buf != nullptr) {
        if (WriteBmp32(path, buf->data, buf->header.w, buf->header.h, buf->header.stride)) {
            std::fprintf(stderr, "[sim] screenshot -> %s\n", path);
        } else {
            std::fprintf(stderr, "[sim] screenshot write failed: %s\n", path);
        }
        lv_draw_buf_destroy(buf);
    } else {
        std::fprintf(stderr, "[sim] lv_snapshot_take failed\n");
    }
    lv_unlock();
}

// ---- virtual touch: a second pointer indev, driven by PI_SIM_CMDFILE ----
// Equivalent of the GT911: lets tests press/hold/move/release at exact pixels.

struct VirtTouch {
    bool pressed = false;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t release_at = 0;  // 0 = hold until an explicit release
};
VirtTouch g_touch;
std::mutex g_touch_mu;

// Smooth fling: interpolate the virtual touch (x0,y0)->(x1,y1) over dur_ms on the
// sim's own clock, emitting an intermediate point every Pump iteration. Coarse
// cmdfile `move` steps (>=100ms apart) can't reproduce a continuous fast swipe, so
// they race screen gestures against short press-and-hold timers (hold-to-talk).
// This delivers real fling-like motion for deterministic gesture tests.
struct Swipe {
    bool active = false;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t start_ms = 0, dur_ms = 0;
};
Swipe g_swipe;

void VirtTouchRead(lv_indev_t*, lv_indev_data_t* data) {
    static bool last_reported = false;
    static uint32_t reads = 0, last_ms = 0;
    if (std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr) {
        uint32_t now = SDL_GetTicks();
        if (++reads % 16 == 1) {
            std::fprintf(stderr, "[sim][vtouch] read#%u dt=%ums\n", reads, now - last_ms);
        }
        last_ms = now;
    }
    std::lock_guard<std::mutex> lk(g_touch_mu);
    if (g_touch.pressed && g_touch.release_at != 0 && SDL_GetTicks() >= g_touch.release_at) {
        g_touch.pressed = false;
    }
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    data->state = g_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (g_touch.pressed != last_reported) {
        last_reported = g_touch.pressed;
        std::fprintf(stderr, "[sim][vtouch] %s (%d,%d)\n", g_touch.pressed ? "press" : "release",
                     (int)g_touch.x, (int)g_touch.y);
    }
}

// Dump the current overlay card's root ("tree" in pi_card_host.cc) geometry —
// useful whenever an overlay-height bug is suspected (the scroll-shrink bug this
// was built for is fixed by pi_card::ReflowOverlay, but the numbers are handy for
// any future height/scroll regression). Tree walk mirrors BuildOverlay():
// screen_active's last child = scrim, scrim[0] = wrap, wrap[0] = tree.
void DumpCardGeom(const char* tag) {
    lv_lock();
    lv_obj_t* scr = lv_screen_active();
    uint32_t n = lv_obj_get_child_count(scr);
    lv_obj_t* scrim = (n > 0) ? lv_obj_get_child(scr, n - 1) : nullptr;
    lv_obj_t* wrap = (scrim && lv_obj_get_child_count(scrim) > 0) ? lv_obj_get_child(scrim, 0) : nullptr;
    lv_obj_t* tree = (wrap && lv_obj_get_child_count(wrap) > 0) ? lv_obj_get_child(wrap, 0) : nullptr;
    if (tree != nullptr) {
        std::fprintf(stderr,
                     "[sim][cardh] %-10s tree_h=%4d tree_w=%4d content_h=%4d scroll_y=%4d "
                     "scroll_top=%4d scroll_bottom=%4d wrap_h=%4d\n",
                     tag, (int)lv_obj_get_height(tree), (int)lv_obj_get_width(tree),
                     (int)lv_obj_get_content_height(tree), (int)lv_obj_get_scroll_y(tree),
                     (int)lv_obj_get_scroll_top(tree), (int)lv_obj_get_scroll_bottom(tree),
                     (int)lv_obj_get_height(wrap));
    } else {
        std::fprintf(stderr, "[sim][cardh] %-10s no overlay card found\n", tag);
    }
    lv_unlock();
}

// TEMP SCAFFOLD (pi_card grid 行内竖直居中取证)：递归打印 overlay 卡树里每个 lv_obj 的
// x/y/w/h + 竖直中心线 y+h/2，用于量化验证同一行内 icon/slider/value 等控件是否真的
// 居中对齐（肉眼看截图看不出 1px 级偏差，得读数字）。
void DumpGeomNode(lv_obj_t* obj, int depth) {
    if (!obj) return;
    const char* kind = "obj";
    if (lv_obj_check_type(obj, &lv_label_class))
        kind = "label";
    else if (lv_obj_check_type(obj, &lv_slider_class))
        kind = "slider";
    else if (lv_obj_check_type(obj, &lv_switch_class))
        kind = "switch";
    else if (lv_obj_check_type(obj, &lv_bar_class))
        kind = "bar";
    else if (lv_obj_check_type(obj, &lv_arc_class))
        kind = "arc";
    else if (lv_obj_check_type(obj, &lv_button_class))
        kind = "button";
    int x = (int)lv_obj_get_x(obj), y = (int)lv_obj_get_y(obj);
    int w = (int)lv_obj_get_width(obj), h = (int)lv_obj_get_height(obj);
    std::fprintf(stderr, "[sim][geomtree] %*s%s x=%d y=%d w=%d h=%d cy=%d\n", depth * 2, "", kind, x, y,
                 w, h, y + h / 2);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) DumpGeomNode(lv_obj_get_child(obj, i), depth + 1);
}
void ExecGeomTree(const char* tag) {
    lv_lock();
    lv_obj_t* scr = lv_screen_active();
    uint32_t n = lv_obj_get_child_count(scr);
    lv_obj_t* scrim = (n > 0) ? lv_obj_get_child(scr, n - 1) : nullptr;
    lv_obj_t* wrap = (scrim && lv_obj_get_child_count(scrim) > 0) ? lv_obj_get_child(scrim, 0) : nullptr;
    lv_obj_t* tree = (wrap && lv_obj_get_child_count(wrap) > 0) ? lv_obj_get_child(wrap, 0) : nullptr;
    if (!tree) {
        std::fprintf(stderr, "[sim][geomtree] %s no overlay card found\n", tag);
        lv_unlock();
        return;
    }
    std::fprintf(stderr, "[sim][geomtree] === %s ===\n", tag);
    DumpGeomNode(tree, 0);
    lv_unlock();
}

// TEMP SCAFFOLD: 找到 chat 的消息流容器（pi_screen 的 s_feed 是 static，外部拿不到，故按
// 特征识别：全屏宽、竖向可滚的那个容器）。
lv_obj_t* FindFeed(lv_obj_t* parent) {
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_dir(c) == LV_DIR_VER &&
            lv_obj_get_width(c) >= 700) {
            return c;
        }
        if (lv_obj_t* r = FindFeed(c)) return r;
    }
    return nullptr;
}

// TEMP SCAFFOLD: 打印消息流的滚动位置——验证「贴底跟随」时看 scroll_y 有没有被抢走。
void DumpFeedGeom(const char* tag) {
    lv_lock();
    lv_obj_t* feed = FindFeed(lv_screen_active());
    if (feed != nullptr) {
        std::fprintf(stderr, "[sim][feedh] %-12s scroll_y=%5d scroll_top=%5d scroll_bottom=%5d\n", tag,
                     (int)lv_obj_get_scroll_y(feed), (int)lv_obj_get_scroll_top(feed),
                     (int)lv_obj_get_scroll_bottom(feed));
    } else {
        std::fprintf(stderr, "[sim][feedh] %-12s feed 未找到\n", tag);
    }
    lv_unlock();
}

// TEMP SCAFFOLD: 按按钮上的文本找到该 button（递归找 label，再往上找最近的 button 祖先）。
// 祖先判定放宽成"任意 CLICKABLE 对象"而非严格 lv_button_class——sbar 的 mode_btn（ZEN/FLOW
// 切换）是 lv_obj_create + 手动挂 CLICKABLE，不是真正的 lv_button，原判定找不到它。
lv_obj_t* FindBtnByLabel(lv_obj_t* parent, const char* text) {
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            const char* t = lv_label_get_text(c);
            if (t != nullptr && std::strcmp(t, text) == 0) {
                for (lv_obj_t* p = lv_obj_get_parent(c); p != nullptr; p = lv_obj_get_parent(p)) {
                    if (lv_obj_check_type(p, &lv_button_class) || lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE))
                        return p;
                }
            }
        }
        if (lv_obj_t* r = FindBtnByLabel(c, text)) return r;
    }
    return nullptr;
}

// TEMP SCAFFOLD: 点一个按钮——查出它的屏幕坐标后走**真实触摸**（同 click 命令的通路），
// 而不是直接 lv_obj_send_event 伪造事件，这样 action 分发链路是真的被走了一遍。
void ClickBtnByLabel(const char* text) {
    lv_lock();
    lv_obj_t* btn = FindBtnByLabel(lv_screen_active(), text);
    int cx = -1, cy = -1;
    bool hidden = false;
    if (btn != nullptr) {
        lv_area_t a;
        lv_obj_get_coords(btn, &a);
        cx = (int)(a.x1 + a.x2) / 2;
        cy = (int)(a.y1 + a.y2) / 2;
        hidden = lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_unlock();
    if (cx < 0) {
        std::fprintf(stderr, "[sim][clickbtn] '%s' 未找到\n", text);
        return;
    }
    std::fprintf(stderr, "[sim][clickbtn] '%s' @(%d,%d)%s\n", text, cx, cy, hidden ? " [HIDDEN!]" : "");
    std::lock_guard<std::mutex> lk(g_touch_mu);
    g_touch.pressed = true;
    g_touch.x = cx;
    g_touch.y = cy;
    g_touch.release_at = SDL_GetTicks() + 250;
}

// TEMP SCAFFOLD: 打印 form 卡里各控件的可交互状态——验证「无 bind 只有 id」的纯本地表单
// 控件没被死控件兜底 DIM 掉（pi_card_render.cc 的 live 判据）。走 LVGL 树按类型找。
void DumpWidgetLive(lv_obj_t* parent, const char* tag) {
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_slider_class) || lv_obj_check_type(c, &lv_switch_class)) {
            std::fprintf(stderr, "[sim][live] %-8s %-7s clickable=%d opa=%d\n", tag,
                         lv_obj_check_type(c, &lv_slider_class) ? "slider" : "switch",
                         lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) ? 1 : 0,
                         (int)lv_obj_get_style_opa(c, LV_PART_MAIN));
        }
        DumpWidgetLive(c, tag);
    }
}

// One command per line: keydown | keyup | type <text> | backspace |
// click <x> <y> | press <x> <y> | move <x> <y> | release | shot <path> | quit
// (PWR_KEY is press-and-hold: keydown ... type ... keyup to record+send.)
void ExecCmd(const std::string& line) {
    std::fprintf(stderr, "[sim][cmd] %s\n", line.c_str());
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "keydown") {
        g_pwr_key_held = true;
    } else if (cmd == "keyup") {
        g_pwr_key_held = false;
    } else if (cmd == "backspace") {
        sim_asr_backspace();
    } else if (cmd == "type") {
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        sim_asr_type(rest.c_str());
    } else if (cmd == "click" || cmd == "press") {
        int x = 0, y = 0;
        ss >> x >> y;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = true;
        g_touch.x = x;
        g_touch.y = y;
        g_touch.release_at = (cmd == "click") ? SDL_GetTicks() + 250 : 0;
    } else if (cmd == "move") {
        int x = 0, y = 0;
        ss >> x >> y;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.x = x;
        g_touch.y = y;
    } else if (cmd == "release") {
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = false;
        g_touch.release_at = 0;
    } else if (cmd == "swipe") {  // swipe <x0> <y0> <x1> <y1> [dur_ms=180]
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, ms = 180;
        ss >> x0 >> y0 >> x1 >> y1 >> ms;
        g_swipe.x0 = x0; g_swipe.y0 = y0; g_swipe.x1 = x1; g_swipe.y1 = y1;
        g_swipe.dur_ms = (ms > 0) ? (uint32_t)ms : 180;
        g_swipe.start_ms = SDL_GetTicks();
        g_swipe.active = true;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = true;
        g_touch.x = x0;
        g_touch.y = y0;
        g_touch.release_at = 0;
    } else if (cmd == "mediadump") {  // Stage B: 打印 MediaController 快照，观察 invoke/set 联动
        auto& mc = media::MediaController::Instance();
        const char* st = "stopped";
        switch (mc.state()) {
            case media::MediaState::Loading: st = "loading"; break;
            case media::MediaState::Playing: st = "playing"; break;
            case media::MediaState::Paused: st = "paused"; break;
            case media::MediaState::Error: st = "error"; break;
            default: break;
        }
        std::fprintf(stderr, "[sim][mediadump] state=%s index=%d pos=%ds title=%s\n", st, mc.index(),
                     mc.position_s(), mc.current().title.c_str());
    } else if (cmd == "shot") {  // Stage B: shot <path> — 立即截图到指定路径
        std::string p;
        std::getline(ss, p);
        if (!p.empty() && p[0] == ' ') p.erase(0, 1);
        TakeScreenshot(p.empty() ? ShotPath() : p.c_str());
    } else if (cmd == "gochat") {  // TEMP SCAFFOLD: 直接切到 Chat 视图（媒体卡验收截图用）
        PiScreen::DebugGoChat();
    } else if (cmd == "mediastage") {  // TEMP SCAFFOLD: mediastage <n> — 灌 n 个电台进播放列表（不起播），
                                       // 供 tracks 兜底注入（MaybeFillTracks）验收用
        int n = 0;
        ss >> n;
        const std::vector<device_config::RadioStation>& stations = device_config::RadioStations();
        std::vector<media::MediaItem> items;
        for (int i = 0; i < n && i < static_cast<int>(stations.size()); i++) {
            media::MediaItem m;
            m.title = stations[i].name;
            m.subtitle = stations[i].genre;
            m.path_or_url = stations[i].url;
            m.is_stream = true;
            items.push_back(std::move(m));
        }
        media::MediaController::Instance().StagePlaylist(items, -1);
        std::fprintf(stderr, "[sim][mediastage] staged %u stations (no autoplay)\n",
                     static_cast<unsigned>(items.size()));
    } else if (cmd == "mediaplay") {  // TEMP SCAFFOLD: mediaplay <path_or_url> — 单曲起播（金标 WAV
                                      // dump / HLS 验收用）；.m3u8 或 http 前缀按流处理
        std::string p;
        std::getline(ss, p);
        if (!p.empty() && p[0] == ' ') p.erase(0, 1);
        if (!p.empty()) {
            media::MediaItem m;
            m.title = p;
            m.path_or_url = p;
            m.is_stream = (p.rfind("http://", 0) == 0 || p.rfind("https://", 0) == 0);
            media::MediaController::Instance().StagePlaylist({std::move(m)}, -1);
            media::MediaController::Instance().PlayIndex(0);
            std::fprintf(stderr, "[sim][mediaplay] start %s\n", p.c_str());
        }
    } else if (cmd == "growcard") {  // TEMP SCAFFOLD: render the reflow re-entrancy test card
        RenderGrowCard();
    } else if (cmd == "showrows") {  // TEMP SCAFFOLD: showrows <n> — real ui_update, drives reflow
        int n = 0;
        ss >> n;
        ShowRows(n);
    } else if (cmd == "patchtest") {  // TEMP SCAFFOLD: real ui_update batch `patches` path
        ExecPatchTest();
    } else if (cmd == "numanimcard") {  // TEMP SCAFFOLD: render the num-anim acceptance card
        RenderNumAnimCard();
    } else if (cmd == "numset") {  // TEMP SCAFFOLD: numset <path> <value> — direct subject write
        std::string path;
        int v = 0;
        ss >> path >> v;
        ExecNumSet(path, v);
    } else if (cmd == "closecard") {  // TEMP SCAFFOLD: closecard <id> — real ui_close
        std::string id;
        ss >> id;
        ExecCloseCard(id);
    } else if (cmd == "previewfeed") {  // TEMP SCAFFOLD: previewfeed <file> — feed partial-JSON frames
        std::string path;
        std::getline(ss, path);
        if (!path.empty() && path[0] == ' ') path.erase(0, 1);
        ExecPreviewFeed(path);
    } else if (cmd == "previewdump") {  // TEMP SCAFFOLD (verify): recursively dump preview tree
        ExecPreviewDump();
    } else if (cmd == "previewend") {  // TEMP SCAFFOLD: simulate UI_TOOL_END without a card render
        ExecPreviewEnd();
    } else if (cmd == "previewscene") {  // TEMP SCAFFOLD: previewscene <frames_file> <n_lines> <shot_path>
        std::string path, shot_path;
        int n_lines = -1;
        ss >> path >> n_lines >> shot_path;
        ExecPreviewScene(path, n_lines, shot_path);
    } else if (cmd == "previewscenechunks") {  // TEMP SCAFFOLD: previewscenechunks <json_file> <shot_path>
        std::string path, shot_path;
        ss >> path >> shot_path;
        ExecPreviewSceneChunks(path, shot_path);
    } else if (cmd == "bargein") {  // TEMP SCAFFOLD: simulate a barge-in/new-session gen bump
        ExecBargeIn();
    } else if (cmd == "rendercard") {  // TEMP SCAFFOLD: rendercard <file> — real ui_render from raw JSON file
        std::string path;
        std::getline(ss, path);
        if (!path.empty() && path[0] == ' ') path.erase(0, 1);
        ExecRenderJson(path);
    } else if (cmd == "strset") {  // TEMP SCAFFOLD: strset <path> <text> — direct String subject write
        std::string path, text;
        ss >> path;
        std::getline(ss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        ExecStrSet(path, text);
    } else if (cmd == "chatcard") {  // TEMP SCAFFOLD: chatcard <n> — append card #n to the chat feed
        int n = 1;
        ss >> n;
        RenderChatCard(n);
    } else if (cmd == "feedh") {  // TEMP SCAFFOLD: feedh <tag> — dump the chat feed's scroll position
        std::string tag;
        ss >> tag;
        DumpFeedGeom(tag.empty() ? "-" : tag.c_str());
    } else if (cmd == "iconcard") {  // TEMP SCAFFOLD: 按钮 icon 支持 + 负例 hints 验收卡
        RenderIconButtonCard();
    } else if (cmd == "formcard") {  // TEMP SCAFFOLD: render the report-snapshot / toggle test card
        RenderFormCard();
    } else if (cmd == "badcards") {  // v2: sim/tests/corpus/negative/*.json 全部 11 个文件，
        // 逐个渲染，断言全部同步被 Validate 拒绝（is_error==true）。取代 v1 各种手写负例常量
        // （bad target / list 超预算 / grid 结构性负例等——v1 shape 本身已被 v2 Validate 拒绝，
        // 不再是"这条具体规则"的针对性负例，语料目录下的 11 个才是 v2 Validate 的权威负例集）。
        for (int i = 0; i < kNumCorpusNegFiles; i++) {
            std::string spec = ReadCorpusFile(std::string("negative/") + kCorpusNegFiles[i]);
            if (spec.empty()) continue;
            RenderBadCard(spec.c_str(), kCorpusNegFiles[i]);
        }
    } else if (cmd == "p1grid") {  // Commit 3 P1: standby grid pin persists → RehydratePin re-renders
        // 直写一张 standby grid 卡的 pin 封套到 NVS（模拟上次会话已持久化），再调
        // RehydratePin（模拟重启回灌）：grid 应 Validate 通过并重渲，不被 discard/erase。
        const char* env =
            "{\"v\":1,\"root\":{\"type\":\"grid\",\"cols\":[1,1],\"gap\":6,\"children\":["
            "{\"type\":\"label\",\"role\":\"section\",\"text\":\"CPU\"},"
            "{\"type\":\"label\",\"role\":\"value\",\"text\":\"42%\"},"
            "{\"type\":\"label\",\"role\":\"section\",\"text\":\"MEM\"},"
            "{\"type\":\"label\",\"role\":\"value\",\"text\":\"61%\"}]}}";
        Settings("ui", true).SetString("pin", env);
        std::fprintf(stderr, "[sim] p1grid: wrote standby-grid pin envelope to NVS\n");
        pi_card::RehydratePin();  // 读 NVS + Validate + OnRenderEvent（drain 下一拍渲染）
        Settings ui_ro("ui", false);
        const bool kept = !ui_ro.GetString("pin", "").empty();
        std::fprintf(stderr, "[sim] p1grid: after RehydratePin pin-in-NVS=%s (kept=Validate通过未被erase)\n",
                     kept ? "true ✓" : "false ✗");
    } else if (cmd == "g4leak") {  // Commit 3 G4: list-of-grid, N rounds ui_update, GridDsc leak check
        int n = 20;
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty()) n = std::atoi(rest.c_str());
        ExecG4Leak(n);
    } else if (cmd == "standby") {  // Phase3: render the standby pin-widget demo card
        RenderStandbyCard();
    } else if (cmd == "standbybig") {  // Phase3 D8: oversized standby envelope (>3072B) rejection
        RenderStandbyOversized();
    } else if (cmd == "unpincard") {  // Phase3: exercise pi_card::UnpinCard() directly (EraseKey+delete)
        pi_card::UnpinCard();
        std::fprintf(stderr, "[sim] unpincard: HasPin()=%s\n", pi_card::HasPin() ? "true" : "false");
    } else if (cmd == "clickbtn") {  // TEMP SCAFFOLD: clickbtn <text> — real touch on a button
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        ClickBtnByLabel(rest.c_str());
    } else if (cmd == "live") {  // TEMP SCAFFOLD: live <tag> — dump slider/switch interactivity
        std::string tag;
        ss >> tag;
        lv_lock();
        DumpWidgetLive(lv_screen_active(), tag.empty() ? "-" : tag.c_str());
        lv_unlock();
    } else if (cmd == "cardh") {  // cardh <tag> — dump the current overlay's height/scroll geometry
        std::string tag;
        ss >> tag;
        DumpCardGeom(tag.empty() ? "-" : tag.c_str());
    } else if (cmd == "geomtree") {  // geomtree <tag> — dump overlay card tree x/y/w/h + center-y
        std::string tag;
        ss >> tag;
        ExecGeomTree(tag.empty() ? "-" : tag.c_str());
    } else if (cmd == "hintcard") {  // TEMP SCAFFOLD（B 验收 §4 断言 7）：触发多条 Lint 规则
        RenderBadCard(kCardHints, "hints");
    } else if (cmd == "choicebind") {  // TEMP SCAFFOLD（B 验收断言 18 可写 bind 半支）
        RenderBadCard(kCardChoiceBind, "choicebind");
    } else if (cmd == "desc") {  // TEMP SCAFFOLD（B 验收 §4 断言 8）：打印动态 ui_render 描述
        std::fprintf(stderr, "[sim][desc] %s\n", pi_card_render_desc());
    } else if (cmd == "datacard") {  // Phase2 T1/T4: render kCardList (fixed card id "datacard")
        RenderDataCard();
    } else if (cmd == "dataop") {  // Phase2 T4: dataop <append|remove> — real ui_update data ops
        std::string op;
        ss >> op;
        ExecDataOp(op);
    } else if (cmd == "datacard2") {  // 改造4: render kCardListFast (fixed card id "datacard2")
        RenderDataCardFast();
    } else if (cmd == "listfastop") {  // 改造4: listfastop <append|remove|replace|set> [index]
        std::string op;
        int idx = 0;
        ss >> op >> idx;
        ExecListFastOp(op, idx);
    } else if (cmd == "listfastmulti_ar") {  // 改造4: 一次 ui_update 里 append+replace 合并
        ExecListFastMultiAppendReplace();
    } else if (cmd == "listfastmulti_as") {  // 改造4: 一次 ui_update 里 append+set 合并
        ExecListFastMultiAppendSet();
    } else if (cmd == "datalabel") {  // Phase2 T8: render kCardDataLabel (fixed card id "datalabel")
        RenderDataLabelCard();
    } else if (cmd == "datalabelset") {  // Phase2 T8: datalabelset <text> — real ui_update data.set
        std::string val;
        std::getline(ss, val);
        if (!val.empty() && val[0] == ' ') val.erase(0, 1);
        ExecDataLabelSet(val);
    } else if (cmd == "choicelabelcard") {  // Phase2 T7: choice + report "{label}" token
        RenderBadCard(kCardChoiceLabel, "choicelabel");
    } else if (cmd == "preset") {  // Phase2 T5: preset <confirm|form|dashboard|menu>
        std::string name;
        ss >> name;
        RenderPreset(name);
    } else if (cmd == "presetbad") {  // Phase2 T5 负向：form 缺 fields，应报具名错误串
        cJSON* args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "preset", "form");
        cJSON_AddObjectToObject(args, "slots");  // 空 slots：无 title/fields
        bool is_err = false;
        char* res = pi_card_tool_render(args, &is_err);
        std::fprintf(stderr, "[sim][negative] preset(form缺字段) -> %s (%s)\n", res ? res : "(null)",
                     is_err ? "已拒绝 ✓" : "竟然通过了 ✗");
        free(res);
        cJSON_Delete(args);
    } else if (cmd == "budget") {  // Phase2 T9: system prompt + ui_render desc 字节预算
        const char* sysp = pi_card_system_prompt();
        const char* desc = pi_card_render_desc();
        unsigned sys_len = static_cast<unsigned>(std::strlen(sysp));
        unsigned desc_len = static_cast<unsigned>(std::strlen(desc));
        std::fprintf(stderr, "[sim][budget] sys=%u desc=%u sum=%u (limit 9216) %s\n", sys_len,
                     desc_len, sys_len + desc_len, (sys_len + desc_len <= 9216) ? "OK" : "OVER!");
    } else if (cmd == "sysprompt") {  // Phase2 T9: 打印完整 system prompt 供目视核对
        std::fprintf(stderr, "[sim][sysprompt] %s\n", pi_card_system_prompt());
    } else if (cmd == "stockcard") {  // 股票: stockcard <symbol> [name] — 注入 stock_chart chat 卡
        std::string sym, name;
        ss >> sym;
        std::getline(ss, name);
        if (!name.empty() && name[0] == ' ') name.erase(0, 1);
        cJSON* args = cJSON_CreateObject();
        cJSON* root = cJSON_AddObjectToObject(args, "root");
        cJSON_AddStringToObject(root, "type", "stock_chart");
        cJSON_AddStringToObject(root, "symbol", sym.c_str());
        if (!name.empty()) cJSON_AddStringToObject(root, "name", name.c_str());
        bool is_err = false;
        char* res = pi_card_tool_render(args, &is_err);
        std::fprintf(stderr, "[sim] stockcard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
        free(res);
        cJSON_Delete(args);
    } else if (cmd == "stockq") {  // 股票: stockq <查询词|symbol,...> — 直调 stock tool（阻塞 ≤6s/请求）
        std::string q;
        std::getline(ss, q);
        if (!q.empty() && q[0] == ' ') q.erase(0, 1);
        cJSON* args = cJSON_CreateObject();
        if (q.find(',') != std::string::npos || (q.size() > 2 && std::isdigit((unsigned char)q[2]))) {
            cJSON* arr = cJSON_AddArrayToObject(args, "symbols");
            std::stringstream qs(q);
            std::string sym;
            while (std::getline(qs, sym, ',')) {
                if (!sym.empty()) cJSON_AddItemToArray(arr, cJSON_CreateString(sym.c_str()));
            }
        } else {
            cJSON_AddStringToObject(args, "query", q.c_str());
        }
        bool is_err = false;
        char* res = pi_stock_tool_run(args, &is_err);
        std::fprintf(stderr, "[sim][stockq] %s -> %s\n", is_err ? "ERR" : "OK", res ? res : "(null)");
        free(res);
        cJSON_Delete(args);
    } else if (cmd == "shot") {
        std::string p;
        ss >> p;
        TakeScreenshot(p.empty() ? ShotPath() : p.c_str());
    } else if (cmd == "quit") {
        g_quit = true;
    }
}

void PollCmdFile(uint32_t now) {
    static const char* cmdfile = std::getenv("PI_SIM_CMDFILE");
    static uint32_t last_poll = 0;
    if (cmdfile == nullptr || cmdfile[0] == '\0' || now - last_poll < 100) return;
    last_poll = now;
    std::ifstream f(cmdfile);
    if (!f.good()) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    f.close();
    ::unlink(cmdfile);
    for (const auto& l : lines) ExecCmd(l);
}

uint32_t EnvMs(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0;
}

// Post-lv_timer_handler pump on the main thread (LVGL lock NOT held here).
void Pump() {
    static const char* demo_text = std::getenv("PI_SIM_AUTODEMO");
    static const uint32_t shot_ms = EnvMs("PI_SIM_SHOT_MS");
    static const uint32_t exit_ms = EnvMs("PI_SIM_EXIT_MS");
    static const uint32_t card_ms = EnvMs("PI_SIM_CARD_MS");  // 到点渲染 pi_card 演示卡
    static int demo_phase = 0;
    static bool shot_done = false;
    static bool card_done = false;
    const uint32_t now = SDL_GetTicks();

    if (card_ms > 0 && !card_done && now > card_ms) {
        card_done = true;
        RenderDemoCard();
    }

    // T1：到点做一次主题往返（深→浅→深）。几何走共享 lv_style_t（无色）故不受影响；
    // 往返后应与从未切换的深色截图逐像素一致——证 pi_theme::Set 只改色、不动几何。
    static const uint32_t theme_swap_ms = EnvMs("PI_SIM_THEME_SWAP_MS");
    static bool theme_swapped = false;
    if (theme_swap_ms > 0 && !theme_swapped && now > theme_swap_ms) {
        theme_swapped = true;
        lv_lock();
        pi_theme::Set(true);   // → 浅色
        pi_theme::Set(false);  // → 回深色（往返）
        lv_unlock();
    }

    // Stage A 媒体管线无头测试钩子：PI_SIM_MEDIA_FILE / PI_SIM_MEDIA_URL 启动即
    // StagePlaylist 单曲播放（WAV dump 由 PI_SIM_MEDIA_WAV 在 pump 内部激活）。
    static const char* media_file = std::getenv("PI_SIM_MEDIA_FILE");
    static const char* media_url = std::getenv("PI_SIM_MEDIA_URL");
    static bool media_started = false;
    if (!media_started && now > 800 && ((media_file && media_file[0]) || (media_url && media_url[0]))) {
        media_started = true;
        media::MediaItem item;
        if (media_url && media_url[0]) {
            item.title = "sim stream";
            item.path_or_url = media_url;
            item.is_stream = true;
        } else {
            item.title = "sim file";
            item.path_or_url = media_file;
            item.is_stream = false;
        }
        std::vector<media::MediaItem> pl{item};
        std::fprintf(stderr, "[sim] media start: %s (%s)\n", item.path_or_url.c_str(),
                     item.is_stream ? "stream" : "file");
        media::MediaController::Instance().StagePlaylist(std::move(pl), 0);
    }

    // Stage D 硬化验收钩子：PI_SIM_MEDIA_STOP_MS=<ms> 在该时刻调 Stop()（多半命中
    // reader 线程正阻塞在文件/网络 Read 里的中途），Stop() 内部已记录 teardown 耗时
    // （"Stop: teardown took Xms" 日志），本处只负责在正确时刻触发。
    static const uint32_t stop_ms = EnvMs("PI_SIM_MEDIA_STOP_MS");
    static bool stop_done = false;
    if (stop_ms > 0 && !stop_done && now > stop_ms) {
        stop_done = true;
        std::fprintf(stderr, "[sim] media stop triggered at wall=%ums\n", now);
        media::MediaController::Instance().Stop();
    }

    // Stage B media 工具无头直测：PI_SIM_MEDIA_TOOL = 一段 args JSON（如
    // {"mode":"search"}），启动后调 pi_media_tool_run 一次并打印返回 JSON。
    static const char* media_tool = std::getenv("PI_SIM_MEDIA_TOOL");
    static bool media_tool_done = false;
    if (!media_tool_done && media_tool && media_tool[0] && now > 900) {
        media_tool_done = true;
        cJSON* args = cJSON_Parse(media_tool);
        if (args) {
            bool is_err = false;
            char* res = pi_media_tool_run(args, &is_err);
            std::fprintf(stderr, "[sim] media tool (%s) -> %s\n", is_err ? "ERROR" : "ok",
                         res ? res : "(null)");
            free(res);
            cJSON_Delete(args);
        } else {
            std::fprintf(stderr, "[sim] media tool: bad args JSON\n");
        }
    }

    // Advance an in-flight smooth swipe (continuous motion, sim-clock paced).
    if (g_swipe.active) {
        uint32_t el = now - g_swipe.start_ms;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        if (el >= g_swipe.dur_ms) {
            g_touch.x = g_swipe.x1;
            g_touch.y = g_swipe.y1;
            g_touch.pressed = false;  // fling ends in a release
            g_swipe.active = false;
        } else {
            float t = (float)el / (float)g_swipe.dur_ms;
            g_touch.x = g_swipe.x0 + (int32_t)((g_swipe.x1 - g_swipe.x0) * t);
            g_touch.y = g_swipe.y0 + (int32_t)((g_swipe.y1 - g_swipe.y0) * t);
            g_touch.pressed = true;
        }
    }

    PollCmdFile(now);

    // AUTODEMO: press-and-hold F1 (keydown), type while holding, release to send.
    if (demo_text != nullptr && demo_text[0] != '\0') {
        if (demo_phase == 0 && now > 1500) {
            g_pwr_key_held = true;  // 按住
            demo_phase = 1;
        } else if (demo_phase == 1 && now > 2600 && sim_asr_session_active()) {
            sim_asr_type(demo_text);
            demo_phase = 2;
        } else if (demo_phase == 2 && now > 3400) {
            g_pwr_key_held = false;  // 松开发送
            demo_phase = 3;
        }
    }

    // Mirror the physical PWR_KEY held state and run the edge state machines
    // (press / hold-to-talk / release) every iteration, under the LVGL lock.
    {
        lv_lock();
        IOExpander::getInstance().simSetPressed(IOExpander::Pin::PWR_KEY, g_pwr_key_held.load());
        IOExpander::getInstance().simPoll(now);
        lv_unlock();
    }
    if (g_demo_pending.exchange(false)) RenderDemoCard();  // 校验+入队；drain 下一拍渲染
    if (g_shot_pending.exchange(false)) TakeScreenshot(ShotPath());
    if (g_previewscene_countdown > 0 && --g_previewscene_countdown == 0) {
        TakeScreenshot(g_previewscene_shot_path.c_str());
        std::fprintf(stderr, "[sim][previewscene] shot -> %s\n", g_previewscene_shot_path.c_str());
    }
    if (shot_ms > 0 && !shot_done && now > shot_ms) {
        shot_done = true;
        TakeScreenshot(ShotPath());
    }
    if (exit_ms > 0 && now > exit_ms) g_quit = true;
}

}  // namespace

int main() {
    signal(SIGCHLD, SIG_IGN); /* auto-reap the optional `say` children */

#ifdef __APPLE__
    /* Inherited background QoS (e.g. launched from a script/daemon) stretches a
     * 10ms SDL_Delay to ~95ms via timer coalescing — the whole UI drops to ~10fps
     * and sub-100ms touch gestures get lost. Pin the LVGL thread to interactive. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    std::fprintf(stderr,
                 "pi_sim — Pinion pi_screen simulator (LVGL %d.%d SDL2)\n"
                 "  F1 按住   = PWR_KEY 按住说话(按住录音/松开发送; 轻点无反应; 生成中点按打断)\n"
                 "  打字      = 聆听时说话(退格删字)\n"
                 "  F9        = 渲染 pi_card 演示卡(overlay)\n"
                 "  F12       = 截图 BMP\n"
                 "  鼠标      = 触摸(状态栏下拉 = 快捷面板)\n",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);

    lv_init();
    lv_tick_set_cb(TickCb);

    lv_display_t* disp = lv_sdl_window_create(720, 720);
    lv_sdl_window_set_title(disp, "Pinion — pi_screen sim");
    lv_sdl_mouse_create();
    SDL_AddEventWatch(EventWatch, nullptr);
    SDL_StartTextInput();

    lv_indev_t* vtouch = lv_indev_create();
    lv_indev_set_type(vtouch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(vtouch, VirtTouchRead);

    if (std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr) {
        lv_timer_create(
            [](lv_timer_t*) {
                static uint32_t n = 0, last = 0;
                if (++n % 32 == 1) {
                    uint32_t now = SDL_GetTicks();
                    std::fprintf(stderr, "[sim][t33] fire#%u dt=%ums lv_tick=%u sdl=%u\n", n,
                                 now - last, lv_tick_get(), now);
                    last = now;
                }
            },
            33, nullptr);
    }

    // main/main.cc boot chain, hardware-free part, verbatim semantics.
    lv_lock();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* pi = PiScreen::Create();
    screen_attach_lifecycle(pi, [](screen_lifecycle_event_t e) { PiScreen::LifecycleCallback(e); });
    lv_screen_load(pi);
    if (old_scr != nullptr && old_scr != pi) lv_obj_delete(old_scr);
    lv_unlock();

    // PI_SIM_DUMP_PROMPT=<file>：把与固件完全一致的 system prompt + ui_render 工具描述 + schema
    // dump 到文件后立即退出（离线核验模型能否遵守 schema 用；boot chain 已跑完，DataHub 路径
    // 清单已注册齐）。
    if (const char* dp = std::getenv("PI_SIM_DUMP_PROMPT")) {
        FILE* f = std::fopen(dp, "w");
        if (f) {
            std::fprintf(f, "===SYSTEM_PROMPT===\n%s\n===RENDER_DESC===\n%s\n===RENDER_SCHEMA===\n%s\n",
                         pi_card_system_prompt(), pi_card_render_desc(), PI_CARD_RENDER_SCHEMA);
            std::fclose(f);
            std::fprintf(stderr, "[sim] dumped prompt+schema to %s\n", dp);
        }
        return 0;
    }

    // PI_SIM_ADMIN=1：起设备 Web 后台（POSIX socket 薄壳，http://127.0.0.1:8080），
    // 在 macOS 浏览器里真实走通整个前端（配置页 + 文件页）；文件落到 pi_sim_sd/，
    // 配置落到 pi_sim_settings.ini 的 cfg.* 键。
    if (std::getenv("PI_SIM_ADMIN") != nullptr) web_admin::httpd::Start();

    // 密钥不再编译进来：没配过就提示一次怎么配（设备上是扫码进后台，sim 里同一个页面）。
    if (!device_config::LlmReady()) {
        std::fprintf(stderr,
                     "[sim] 未配置大模型 API Key，agent 不会启动。配置方式：\n"
                     "      PI_SIM_ADMIN=1 ./sim/build/pi_sim → 浏览器开 http://127.0.0.1:8080\n");
    }

    const bool loop_debug = std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr;
    uint32_t stat_loops = 0, stat_handler = 0, stat_pump = 0, stat_last = SDL_GetTicks();
    while (!g_quit) {
        uint32_t t0 = SDL_GetTicks();
        uint32_t wait = lv_timer_handler();
        uint32_t t1 = SDL_GetTicks();
        Pump();
        uint32_t t2 = SDL_GetTicks();
        if (loop_debug) {
            stat_loops++;
            stat_handler += t1 - t0;
            stat_pump += t2 - t1;
            if (t2 - stat_last >= 2000) {
                std::fprintf(stderr, "[sim][loop] %.1f loops/s, handler avg %.1fms, pump avg %.1fms\n",
                             stat_loops * 1000.0 / (t2 - stat_last), (double)stat_handler / stat_loops,
                             (double)stat_pump / stat_loops);
                stat_loops = stat_handler = stat_pump = 0;
                stat_last = t2;
            }
        }
        if (wait == LV_NO_TIMER_READY || wait > 10) wait = 10;
        SDL_Delay(wait);
    }
    return 0;
}
