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
#include "pi_card/pi_card_tools.h"
#include "pi_screen.h"
#include "screen_util.h"
#include "sim_hooks.h"

namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_pwr_key_held{false};  // F1 held = PWR_KEY pressed (press-and-hold)
std::atomic<bool> g_shot_pending{false};
std::atomic<bool> g_demo_pending{false};  // F9 = 渲染一张 pi_card 演示卡（overlay，任何视图可见）

// pi_card 声明式 UI 演示卡：覆盖 icon/slider/bar/label(mono+puhui)/switch/button/
// divider/spacer + 双向 bind + tone 语义色 + 自适应布局。走完整管道（校验→入队→
// drain 渲染），是渲染器 + 主题 + 图标 + 自适应的单图核验。
constexpr const char* kCard0 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":22,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"PI CONTROL\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"设备控制\"}]},"
    "{\"type\":\"column\",\"gap\":18,\"children\":["
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"volume\"},"
    "{\"type\":\"slider\",\"bind\":\"audio.volume\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"audio.volume\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"sun\"},"
    "{\"type\":\"slider\",\"bind\":\"display.brightness\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"display.brightness\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"bar\",\"bind\":\"battery.level\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\",\"bind\":\"battery.level\","
    "\"fmt\":\"%d%%\"}]}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wifi\",\"tone\":\"ok\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"网络\"},{\"type\":\"spacer\"},"
    "{\"type\":\"switch\",\"checked\":true}]},"
    "{\"type\":\"row\",\"gap\":12,\"children\":["
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"取消\",\"on_click\":[{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"确认\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"确认\"},{\"do\":\"close\"}]}]}"
    "]}}";

// ---- 稳定性压力测试语料：多样 + 对抗性用例，验证"怎么拼都不崩不溢出不难看" ----
// 1 确认框（长正文换行 + ghost/primary 层级）
constexpr const char* kCard1 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"CONFIRM\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"切换到 4G?\"}]},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"当前使用 WiFi。切换到蜂窝网络会断开连接"
    "并重启设备,大约需要 30 秒,期间无法使用。确定继续吗?\"},"
    "{\"type\":\"row\",\"gap\":12,\"children\":["
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"取消\",\"on_click\":[{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"切换并重启\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"切换到4G\"},{\"do\":\"close\"}]}]}]}}";
// 2 菜单/列表（一列全宽按钮）
constexpr const char* kCard2 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"SELECT\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"选择操作\"},"
    "{\"type\":\"spacer\",\"h\":4},"
    "{\"type\":\"button\",\"text\":\"新建对话\",\"on_click\":[{\"do\":\"report\",\"text\":\"新建对话\"},{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"text\":\"导出记录\",\"on_click\":[{\"do\":\"report\",\"text\":\"导出记录\"},{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"text\":\"清空历史\",\"on_click\":[{\"do\":\"report\",\"text\":\"清空历史\"},{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"关闭\",\"on_click\":[{\"do\":\"close\"}]}]}}";
// 3 信息/状态卡（键值行 + spacer 对齐 + 只读 bind + 字符串 bind）
constexpr const char* kCard3 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"STATUS\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"设备状态\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"电量\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"battery.level\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"volume\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"音量\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"audio.volume\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wifi\",\"tone\":\"ok\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"网络\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"net.type\"}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"section\",\"text\":\"SIGNAL\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\",\"bind\":\"net.rssi\","
    "\"fmt\":\"%d dBm\"}]}]}}";
// 4 换行/分配压力（超长正文 + 一排 6 按钮）
constexpr const char* kCard4 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":14,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"换行与分配压力测试\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"这是一段刻意写得很长很长很长的说明文字,"
    "用来验证固定宽度卡片里长文本会正确折行显示完整,而不是溢出边界或被裁剪,连续无空格的中文"
    "也应当逐字换行铺满可用宽度。\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"button\",\"text\":\"一\"},{\"type\":\"button\",\"text\":\"二\"},"
    "{\"type\":\"button\",\"text\":\"三\"},{\"type\":\"button\",\"text\":\"四\"},"
    "{\"type\":\"button\",\"text\":\"五\"},{\"type\":\"button\",\"text\":\"六\"}]}]}}";
// 5 对抗/退化（min>max 滑块 / 无文字按钮 / 未知图标→圆点 / 空容器 / 极简）
constexpr const char* kCard5 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"退化输入兜底\"},"
    "{\"type\":\"slider\",\"min\":80,\"max\":20,\"value\":50},"
    "{\"type\":\"button\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wibblewobble\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"未知图标 → 圆点\"}]},"
    "{\"type\":\"column\",\"children\":[]},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"以上均应安全渲染,不崩不溢出\"}]}}";

// 6 对抗/崩溃根因：数值路径 label 误用 %s。真机(newlib-nano)上 lv_label_bind_text 立即
// 回调 → clib vsnprintf 把 net.rssi(断网=0) 当指针 strlen((char*)0) → Load access fault
// （即 docs 记录的那次渲染崩溃）。修复后 Validate 在工具期直接拒绝整卡：pi_card_tool_render
// 返回 (ERROR)，绝不进入渲染。注：本 sim 用 macOS libc，对 %s 套整数比 newlib 宽容、不复现
// 该崩溃，故此卡在 sim 里只核验「修复=Validate 同步拒绝」这一层（真机崩溃已由 panic 日志确证）。
constexpr const char* kCardBadFmt =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"畸形 fmt 对抗\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"net.rssi\",\"fmt\":\"%s\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"数值路径误用 %s: 应被拒绝而非崩溃\"}]}}";

constexpr const char* kCards[] = {kCard0, kCard1, kCard2,      kCard3,
                                  kCard4, kCard5, kCardBadFmt};

// 7 超高卡片（overlay 高度封顶 + 内部滚动的稳定性验证），运行时拼多行。
std::string BuildTallCard() {
    static const char* icons[] = {"volume", "sun", "battery", "wifi", "gear", "clock", "info", "music"};
    std::string s = "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":[";
    s += "{\"type\":\"label\",\"role\":\"title\",\"text\":\"很高的卡片 / 滚动\"},";
    const int rows = 20;  // 每行 3 节点 → 20 行 61 节点，压进 64 上限；总高超屏 → 滚动
    for (int i = 0; i < rows; i++) {
        char row[200];
        std::snprintf(row, sizeof(row),
                      "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"%s\"},"
                      "{\"type\":\"label\",\"role\":\"value\",\"text\":\"ITEM %02d      OK\"}]}%s",
                      icons[i % 8], i + 1, i < rows - 1 ? "," : "");
        s += row;
    }
    s += "]}}";
    return s;
}

void RenderDemoCard() {
    const char* idx_env = std::getenv("PI_SIM_CARD_IDX");
    int idx = idx_env ? std::atoi(idx_env) : 0;
    std::string tall;
    const char* spec;
    const int n_static = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    if (idx >= 0 && idx < n_static) {
        spec = kCards[idx];
    } else {
        tall = BuildTallCard();  // idx == n_static（=6）→ 超高卡
        spec = tall.c_str();
    }
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
                 "pi_sim — Metalio Claw pi_screen simulator (LVGL %d.%d SDL2)\n"
                 "  F1 按住   = PWR_KEY 按住说话(按住录音/松开发送; 轻点无反应; 生成中点按打断)\n"
                 "  打字      = 聆听时说话(退格删字)\n"
                 "  F9        = 渲染 pi_card 演示卡(overlay)\n"
                 "  F12       = 截图 BMP\n"
                 "  鼠标      = 触摸(状态栏下拉 = 快捷面板)\n",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);

    lv_init();
    lv_tick_set_cb(TickCb);

    lv_display_t* disp = lv_sdl_window_create(720, 720);
    lv_sdl_window_set_title(disp, "Metalio Claw — pi_screen sim");
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
