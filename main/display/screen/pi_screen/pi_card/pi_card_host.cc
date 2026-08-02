#include "pi_card_host.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "esp_log.h"

#include "pi_card_cmd.h"
#include "pi_card_data.h"
#include "pi_card_icons.h"
#include "pi_card_media.h"
#include "pi_card_render.h"
#include "pi_card_stock.h"
#include "pi_card_tools.h"
#include "pi_card_xml.h"
#include "pi_fonts.h"
#include "pi_theme.h"
#include "pi_ui_bridge.h"
#include "screen_util.h"
#include "settings.h"

#define TAG "pi_card_host"

namespace pi_card {

namespace {

// ---- 注册表 / 状态（cards_ 只在 LVGL 线程被动，无需锁；id 计数跨线程原子）----
std::map<std::string, std::unique_ptr<UiCard>> s_cards;
std::string s_last_id;
std::atomic<int> s_next_id{1};
int s_overlay_count = 0;
FeedHooks s_feed{};
bool s_screen_off = false;  // SetScreenOff 镜像；供 PlayCardEntrance/NumScrollObserverCb 息屏跳动画的门控查询

// overlay 护栏：浮层会阻止息屏（见 pi_screen 的 sleep 门控），无 ttl 的浮层若被 LLM
// 忘关会让屏幕永不熄灭、持续耗电，故每个 overlay 都有一个保底最大存活期；同时封顶
// 同屏叠加的浮层数，防失控刷屏把 scrim 层层堆满。
constexpr int kOverlayMaxTtlMs = 5 * 60 * 1000;  // 5min，与最长息屏档呼应
constexpr int kMaxOverlays = 3;

// ---- Phase3：常驻小组件（display:'standby'，单槽，固定 id "pin"）----
constexpr const char* kPinId = "pin";
constexpr size_t kPinMaxBytes = 3072;
constexpr int kPinVer = 1;
bool s_pin_active = false;

// 自增内部 id 用 "c." 前缀，与 LLM 惯用的 "c1"/"card1" 命名空间隔开，避免 LLM 显式传入
// 的 id 与将来自增出的 id 撞车（撞车会触发 OnRenderEvent 的删旧卡替换）。
std::string AllocId() { return std::string("c.") + std::to_string(s_next_id.fetch_add(1)); }

UiCard* FindCard(const std::string& id) {
    const std::string& key = id.empty() ? s_last_id : id;
    if (key.empty()) return nullptr;
    auto it = s_cards.find(key);
    return it == s_cards.end() ? nullptr : it->second.get();
}

// worker 线程：构造并入队一个 pi_ui_evt_t（strdup 的串由 drain 侧 free）。返回是否
// 入队成功——队列满是 worker 线程唯一能同步得知的执行期错误，据此回传给 LLM。
bool Enqueue(pi_ui_kind_t kind, char* s1, char* s2, char* s3, int i1, int i2) {
    pi_ui_evt_t evt;
    evt.kind = kind;
    evt.s1 = s1;
    evt.s2 = s2;
    evt.s3 = s3;
    evt.i1 = i1;
    evt.i2 = i2;
    /* 打上当前 run 代次：否则 evt.gen 是栈上未初始化值，几乎必被 drain 的代次过滤器丢弃，
     * 卡片声明式 UI 会整体失效。工具在 worker 线程 run 内同步执行，用 run 代次与本轮
     * 文本事件保持一致过滤（barge-in 打断后旧 run 的迟到卡片据此被正确丢弃）。 */
    evt.gen = pi_agent_task_run_gen();
    // 短阻塞等待而非立即放弃：ui_render 流式期间 UI_TOOL_ARGS 会挤满队列（深 32），
    // execute 紧随最后一个 delta 到来，0 等待几乎必撞满——渲染被拒、LLM 重试、重试的
    // 流式又挤满队列……活锁（牛市看板真机复现）。drain tick 每 80ms 清 64 条，等待
    // 500ms 必有空位；工具本就在 worker 线程同步执行，短阻塞无副作用。
    if (xQueueSend(pi_ui_queue(), &evt, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "pi_ui_queue full after 500ms wait, dropping card evt kind=%d",
                 static_cast<int>(kind));
        free(s1);
        free(s2);
        free(s3);
        return false;
    }
    return true;
}

char* Dup(const std::string& s) { return strdup(s.c_str()); }

// drain（LVGL 线程）阶段的执行失败无法走工具返回值回传——工具早已在 worker 线程
// 同步返回。这里把失败作为一条系统提示异步注入回 LLM（与交互事件 report 同一通路），
// 让助手知道「以为成功的操作其实失败了」，可据此纠正、重试或转告用户。
void ReportAsyncError(const std::string& msg) {
    std::string tagged = "「卡片错误」" + msg;
    pi_agent_task_inject(tagged.c_str());
    ESP_LOGW(TAG, "async error -> %s", msg.c_str());
}

// ------------------------------ overlay 骨架 -------------------------------
void OnRootDeleted(lv_event_t* e);

void OverlayCloseCb(lv_event_t* e) {
    auto* card = static_cast<UiCard*>(lv_event_get_user_data(e));
    if (card && card->root) {
        // 与播放器 X 同范式（pi_media OnStopBtn）：inject 告知 LLM 卡已被用户关掉（空闲
        // 起一轮、对话中插下一轮），免得它拿着一个不存在的 card id 继续 update/close。
        // 静默 note 用户侧看不到任何反应，已按用户要求改成 inject。
        std::string note = "「卡片」用户手动关闭了卡片 " + card->id;
        // new_session 后关旧会话残留的卡不该凭空起一轮，守卫一下。
        if (pi_agent_task_has_messages()) pi_agent_task_inject(note.c_str());
        lv_obj_delete_async(card->root);
    }
}

void OverlayTtlCb(lv_timer_t* t) {
    auto* card = static_cast<UiCard*>(lv_timer_get_user_data(t));
    if (card) {
        card->ttl_timer = nullptr;  // repeat_count=1，LVGL 自动回收本 timer
        if (card->root) lv_obj_delete_async(card->root);
    }
}

// scrim（挂当前屏顶层子节点、吞底层点击）+ 居中 wrapper（root 的父，透明；root
// 自身的 depth0 卡片外观即浮层观感，避免双层卡）。返回 scrim；*out = wrapper。
// 单屏固件里挂 lv_screen_active() 即置顶（同 quick_panel/settings 惯例，且被截图/
// 快照捕获），无需 lv_layer_top。
lv_obj_t* BuildOverlay(lv_obj_t** out_wrapper) {
    lv_obj_t* scrim = lv_obj_create(lv_screen_active());
    screen_strip_obj_chrome(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(scrim, 0, 0);
    pi_theme::ApplyScrim(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // 吞底层

    lv_obj_t* wrap = lv_obj_create(scrim);
    screen_strip_obj_chrome(wrap);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(wrap, LV_PCT(80));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_center(wrap);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    *out_wrapper = wrap;
    return scrim;
}

// 右上角 40px 圆形关闭按钮（防 LLM 忘留出口）。
void AddOverlayCloseButton(lv_obj_t* wrapper, UiCard* card) {
    lv_obj_t* btn = lv_button_create(wrapper);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    pi_theme::ApplyBg(btn, pi_theme::Tok::Card2);
    pi_theme::ApplyBg(btn, pi_theme::Tok::Line,
                      LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_t* icon = MakeIcon(btn, "close", 18, pi_theme::Tok::Dim);
    lv_obj_center(icon);
    lv_obj_add_event_cb(btn, OverlayCloseCb, LV_EVENT_CLICKED, card);
}

// 唯一清理通道：root 被删（ClearFeed / 新会话 / 屏卸载 / close / ttl）都到这里。
void OnRootDeleted(lv_event_t* e) {
    auto* card = static_cast<UiCard*>(lv_event_get_user_data(e));
    if (!card) return;
    if (card->ttl_timer) {
        lv_timer_delete(card->ttl_timer);
        card->ttl_timer = nullptr;
    }
    DataHub& hub = DataHub::Instance();
    for (const std::string& p : card->hub_paths) hub.Release(p);
    // 卡级 data 模型 + list 行模板池随卡片一起释放（Phase2 spec/data 分离）。
    if (card->data) cJSON_Delete(card->data);
    for (cJSON* p : card->json_pool) cJSON_Delete(p);
    if (card->display == Display::Overlay && s_overlay_count > 0) s_overlay_count--;
    // pin 卡被删（替换旧 pin / ui_close / 屏卸载 / ClearFeed 均可能触发）：只清运行态，
    // **不擦 NVS**——替换新 pin 或屏幕重建都不该丢失持久化，只有 UnpinCard()/OnCloseEvent
    // 显式移除时才擦 key。
    if (card->display == Display::Pin) {
        s_pin_active = false;
        if (s_feed.on_pin_changed) s_feed.on_pin_changed(false);
    }
    std::string id = card->id;
    if (s_last_id == id) s_last_id.clear();
    s_cards.erase(id);  // 释放 UiCard（lv_obj 由 LVGL 释放）
}

// pin 卡的屏上 ✕ 角标：点击 -> 通用固件确认 sheet（复用 CommandRegistry 的确认通道）->
// 确认才 UnpinCard()。取消=无副作用。
void PinRemoveCb(lv_event_t*) {
    CommandRegistry::Instance().ShowConfirm(
        "移除常驻组件？", "屏幕上将不再显示这个小组件", "移除", []() { UnpinCard(); });
}

void AddPinRemoveButton(lv_obj_t* wrapper) {
    lv_obj_t* btn = lv_button_create(wrapper);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(btn, 36, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    pi_theme::ApplyBg(btn, pi_theme::Tok::Card2);
    pi_theme::ApplyBg(btn, pi_theme::Tok::Line,
                      LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_t* icon = MakeIcon(btn, "close", 16, pi_theme::Tok::Dim);
    lv_obj_center(icon);
    lv_obj_add_event_cb(btn, PinRemoveCb, LV_EVENT_CLICKED, nullptr);
}

// NVS "ui"/"pin" 的封套（cJSON_PrintUnformatted 单行——sim 的 settings shim 按行存，换行
// 会截断，故整份必须无换行；spec_json/data_json 均已是 cJSON_PrintUnformatted 产物，天然
// 无原始换行）。>kPinMaxBytes 拒绝：ReportAsyncError 提示精简，调用方负责回滚已渲染的卡。
bool PinEnvelopeFits(const std::string& envelope) { return envelope.size() <= kPinMaxBytes; }

// 封套构造单一来源：worker 侧的前置尺寸校验（pi_card_tool_render）与 drain 侧持久化
// （OnRenderEvent 的 pin 分支）必须拼出字节数完全一致的串，否则两边判定的"超没超限"会
// 对不上。root_json/data_json 均为 cJSON_PrintUnformatted 产物（或 nullptr/空）。
std::string BuildPinEnvelope(const char* root_json, const char* data_json) {
    return std::string("{\"v\":") + std::to_string(kPinVer) + ",\"root\":" +
          (root_json && root_json[0] ? root_json : "{}") + ",\"data\":" +
          (data_json && data_json[0] ? data_json : "{}") + "}";
}

void PersistPin(const std::string& envelope) {
    Settings ui("ui", true);
    ui.SetString("pin", envelope);
}

}  // namespace

// ---------------------------------------------------------------------------
void Init() {
    DataHub::Instance().RegisterBuiltins();
    CommandRegistry::Instance().RegisterBuiltins();
    pi_card_stock::RegisterBindProvider();  // stock.<symbol>.<field> 动态绑定（Phase4）
    // media.* 数据路径/invoke 命令已随媒体卡一起删除：播控 UI 只走内置播放器
    //（Now-Playing 页 + dock 迷你条），LLM 侧不再渲染任何播放器卡片（拦截见 OnRenderEvent）。
}

void SetFeedHooks(const FeedHooks& hooks) { s_feed = hooks; }

// 转发 s_feed 的两个钩子给 pi_card_preview.cc（另一个 TU）建预览行用——预览不走
// OnRenderEvent 的完整流程（没有 UiCard、不入 s_cards 注册表），只需要一个空行 + 首次建行时
// 滚到可见，不必让它重复存一份 FeedHooks。
lv_obj_t* FeedBeginRow() { return s_feed.begin_row ? s_feed.begin_row() : nullptr; }
void FeedEndRowOnce() {
    if (s_feed.end_row) s_feed.end_row();
}

bool HasOpenOverlay() { return s_overlay_count > 0; }

void SetScreenOff(bool off) {
    s_screen_off = off;
    DataHub::Instance().SetLivePaused(off);
    pi_card_stock::SetScreenOff(off);
}

bool IsScreenOff() { return s_screen_off; }

bool AnyVisibleCardBindsPrefix(const std::string& prefix) {
    for (const auto& [id, card] : s_cards) {
        if (card->root == nullptr || !lv_obj_is_visible(card->root)) continue;
        for (const std::string& p : card->hub_paths) {
            if (p.compare(0, prefix.size(), prefix) == 0) return true;
        }
    }
    return false;
}

// overlay 高度稳定器——量出自然高度后二选一：矮于封顶就跟手收缩（SIZE_CONTENT，不滚动），
// 高于封顶就钉死为固定高度 + 内部竖向滚动。
//
// 不能用「SCROLLABLE + max_height，同时高度模式仍是 LV_SIZE_CONTENT」这个组合（之前的
// 写法）：managed_components/lvgl__lvgl/src/core/lv_obj_pos.c:1172-1220 的
// calc_content_height() 算 SIZE_CONTENT 的内容高度时，进来先把 obj->scroll.y 记账清零
// （1175 行），但循环里读子对象高度用的是子对象已被 lv_obj_scroll_y 平移过的**绝对坐标**
// child->coords.y2（1199/1212 行），退出前才把 scroll.y 记账恢复（1218 行）——即整个计算
// 过程中 child->coords.y2 已经是"滚动之后"的坐标，卡片被滚得越多，量出来的 content_h 就
// 越矮。content_h 矮了 → SIZE_CONTENT 的高度矮了 → 可滚动距离（h - content_h 的另一侧）
// 跟着变短，但当前的 scroll_y 并不会自动缩回，于是下一次滚动/布局用一个"偏大"的 scroll_y
// 再算一遍，content_h 又矮一截——正反馈失控（sim 实测：tree_h 619→496，scroll_y 一路涨到
// 204，远超真实可滚距离 109）。固定高度 + SCROLLABLE 没有这个耦合：高度是显式设定值，不
// 受 scroll_y 影响，滚多少次都纹丝不动。
void ReflowOverlay(UiCard* card) {
    lv_obj_t* tree = card ? card->overlay_tree : nullptr;
    if (!tree) return;
    // 先复位：撤掉上一轮 reflow 可能加过的固定高度/滚动/滚动位置，否则量出来的不是
    // 真正的自然高度（重入场景：ui_update 或本地 toggle 动作改了内容后会再次调用本函数）。
    lv_obj_remove_flag(tree, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_scroll_to_y(tree, 0, LV_ANIM_OFF);
    lv_obj_set_height(tree, LV_SIZE_CONTENT);
    lv_obj_update_layout(tree);  // 强制布局，读到的高度才是准的
    const int32_t h_nat = lv_obj_get_height(tree);
    const int32_t cap = lv_display_get_vertical_resolution(nullptr) * 86 / 100;
    if (h_nat > cap) {
        lv_obj_set_height(tree, cap);  // 固定高度，不是 max_height
        lv_obj_add_flag(tree, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(tree, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(tree, LV_SCROLLBAR_MODE_AUTO);
    }
    // h_nat <= cap：维持上面已复位好的 SIZE_CONTENT + 不可滚动状态。
}

// LVGL 线程：真正建控件。spec_json 是 root 子树；display/ttl/id 由事件带入。
// data_json：卡级 data（object 字面量的 JSON 串），走 pi_ui_evt_t.s3；无则 nullptr/空——data
// 解析失败或非 object 时退化成空 object（list/bind_data 消费者读不到值即为空数组/空文本，
// 不因数据缺失让整卡渲染失败）。
void OnRenderEvent(const char* spec_json, const char* card_id, int display_mode, int ttl_ms,
                   const char* data_json) {
    // 关键排障日志：卡片渲染的完整 spec/data 原文。每卡一次、量级 KB，115200 下可承受
    // （对比流式 args 每 delta 一条的红线场景）；真机排"渲染不对"类问题全靠它对现场。
    ESP_LOGI(TAG, "render spec: %s", spec_json ? spec_json : "(null)");
    if (data_json && data_json[0]) ESP_LOGI(TAG, "render data: %s", data_json);
    cJSON* root = cJSON_Parse(spec_json ? spec_json : "");
    // v2: root 是 grid 块数组（见 docs/CARD_V2.md §1.1），不再是单个 column/row 对象。
    if (!root || !(cJSON_IsObject(root) || cJSON_IsArray(root))) {
        ESP_LOGW(TAG, "render: bad root json");
        cJSON_Delete(root);
        return;
    }
    const bool overlay = display_mode == 1;
    const bool pin = display_mode == 2;
    if (overlay && s_overlay_count >= kMaxOverlays) {
        ESP_LOGW(TAG, "overlay cap reached (%d), dropping card render", s_overlay_count);
        ReportAsyncError("同屏浮层已达上限，本次渲染被丢弃；请先 ui_close 旧卡再重试");
        cJSON_Delete(root);
        return;
    }
    // 常驻卡：单槽固定 id "pin"——忽略 LLM 传入的 card_id（若有），再次 standby 渲染即替换。
    std::string id = pin ? kPinId : ((card_id && card_id[0]) ? card_id : AllocId());

    // 显式 id 撞车：先同步删旧卡（触发其 OnRootDeleted 清理），避免覆盖注册表
    // 时把旧 UiCard 释放而其 root 仍挂着指向它的 DELETE 回调。
    if (auto it = s_cards.find(id); it != s_cards.end()) {
        if (it->second->root) lv_obj_delete(it->second->root);
    }

    auto card = std::make_unique<UiCard>();
    card->id = id;
    card->display = pin ? Display::Pin : (overlay ? Display::Overlay : Display::Chat);
    // 卡级 data 就位于渲染前——RenderNode 遇 list/bind_data 需要读它。解析失败或非 object
    // 一律退化成空 object，不让数据问题拖垮整卡渲染。
    card->data = cJSON_Parse(data_json && data_json[0] ? data_json : "");
    if (!cJSON_IsObject(card->data)) {
        cJSON_Delete(card->data);
        card->data = cJSON_CreateObject();
    }
    // 媒体卡兜底：LLM 渲控件卡时常忘把 play 返回的 tracks 复制进 data，list 会空渲成占位
    // 白块。设备侧直接从 MediaController 补齐（与全屏抽屉同源），数据永远真实。
    pi_card_media::MaybeFillTracks(root, card->data);
    // 媒体卡不存在了：播控 UI 只走内置播放器（Now-Playing 页 + dock 迷你条），LLM 渲染
    // 播放器卡片没有意义（重复入口 + 1Hz 实时绑定刷新的重绘负载）。工具 desc 与 play 结果
    // hint 已不再引导画卡，这里是防 LLM 不听话的最后闸门（media.* 路径/命令也已注销，
    // 即便漏拦也只会渲出空绑定）。
    if (pi_card_media::SpecUsesMedia(root)) {
        ESP_LOGW(TAG, "media card blocked (built-in player only)");
        ReportAsyncError(
            "播放器卡片不可用：设备内置播放器界面会自动出现，请勿渲染任何 media.* 卡片，"
            "也不要重试；用文字确认播放状态即可");
        cJSON_Delete(root);
        return;
    }

    lv_obj_t* delete_root = nullptr;
    lv_obj_t* render_parent = nullptr;
    lv_obj_t* wrapper = nullptr;
    if (pin) {
        lv_obj_t* pin_host = s_feed.pin_host ? s_feed.pin_host() : nullptr;
        if (!pin_host) {
            ReportAsyncError("home widget unavailable");
            cJSON_Delete(root);
            return;
        }
        // 每次渲染都建一个新 wrapper（旧 pin 已在上面的 id 撞车分支同步删掉）：wrapper 既是
        // DELETE 清理挂点，也让 pin_host 本身保持长驻、可反复承载新卡。
        wrapper = lv_obj_create(pin_host);
        screen_strip_obj_chrome(wrapper);
        lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);  // 空白区按压穿透回 PTT（D3）
        // 大时钟让位后 pin 独占待机屏：收到 75% 宽（host flex 居中），全宽视觉太满。
        lv_obj_set_width(wrapper, LV_PCT(75));
        lv_obj_set_height(wrapper, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, LV_PART_MAIN);
        delete_root = wrapper;
        render_parent = wrapper;
    } else if (overlay) {
        delete_root = BuildOverlay(&wrapper);
        render_parent = wrapper;
    } else {
        if (!s_feed.begin_row) {
            ESP_LOGW(TAG, "render: no feed hooks");
            ReportAsyncError("聊天内联卡片当前不可用（feed 未就绪）");
            cJSON_Delete(root);
            return;
        }
        delete_root = s_feed.begin_row();
        render_parent = delete_root;
        if (!delete_root) {
            ReportAsyncError("聊天内联卡片当前不可用（feed 未就绪）");
            cJSON_Delete(root);
            return;
        }
    }
    card->root = delete_root;
    // 先挂清理回调再渲染：中途失败删 root 也能 Release 掉已 Acquire 的路径。
    lv_obj_add_event_cb(delete_root, OnRootDeleted, LV_EVENT_DELETE, card.get());

    RenderLimits limits;
    int node_count = 0;
    std::string rerr;
    lv_obj_t* tree = RenderNode(render_parent, root, card.get(), limits, 0, node_count, rerr);
    cJSON_Delete(root);
    if (!tree) {
        ESP_LOGW(TAG, "render failed: %s", rerr.c_str());
        // 干跑校验已在 worker 侧过一遍，drain 再失败属渲染器/校验器不同构的意外——回传
        // 让 LLM 换个更简单的卡重试，别拿着一个其实没渲出来的 card id 继续。
        ReportAsyncError(std::string("卡片渲染失败：") + rerr);
        lv_obj_delete(delete_root);  // 触发 OnRootDeleted → Release（未入表，erase 无副作用）
        return;
    }

    if (pin) {
        // 封套尺寸的权威判定在 worker 侧（pi_card_tool_render，入队之前）——超限的 standby
        // 根本不会入队，故这里理论上不可达。仍保留判定作兜底（防御 worker/drain 两侧对封套
        // 拼法未来漂移不同构），一旦真的命中说明前置校验有 bug，日志标注以便定位。
        std::string envelope = BuildPinEnvelope(spec_json, data_json);
        if (!PinEnvelopeFits(envelope)) {
            ESP_LOGE(TAG, "pin envelope oversized reached drain (should have been rejected by worker "
                         "pre-check) — %u bytes", static_cast<unsigned>(envelope.size()));
            ReportAsyncError("home widget too large to pin, simplify to ~3KB");
            lv_obj_delete(delete_root);  // 回滚已渲染的卡（触发 OnRootDeleted，未入表 erase 无副作用）
            return;
        }
        AddPinRemoveButton(wrapper);
        PersistPin(envelope);
        s_pin_active = true;
        if (s_feed.on_pin_changed) s_feed.on_pin_changed(true);
        s_cards[id] = std::move(card);
        ESP_LOGI(TAG, "rendered pinned card (%d nodes)", node_count);
        return;  // 单槽常驻卡：不设 TTL、不计入 overlay 上限、不改 s_last_id
    }

    if (overlay) {
        // 浮层卡片加柔和投影，从遮罩上"浮起"（聊天流内的卡片保持扁平，不加）。
        lv_obj_set_style_shadow_width(tree, 40, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(tree, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(tree, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(tree, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(tree, LV_OPA_40, LV_PART_MAIN);
        // 稳定性护栏：LLM 给出的卡片再高也不溢出屏幕——ReflowOverlay 量出自然高度，矮于
        // 86% 屏高就跟手收缩，超出就钉死高度 + 内部竖向滚动（该函数注释有为什么不能用
        // max_height + SIZE_CONTENT 的详细说明）。
        card->overlay_tree = tree;
        ReflowOverlay(card.get());
        AddOverlayCloseButton(wrapper, card.get());
        PlayCardEntrance(tree, /*adopted=*/false);
        // 保底 TTL：无 ttl（0）或 LLM 给的超长 ttl 一律封顶到 kOverlayMaxTtlMs，避免浮层
        // 永久阻止息屏。显式 ttl 在封顶内则照用。
        int ttl = (ttl_ms > 0 && ttl_ms < kOverlayMaxTtlMs) ? ttl_ms : kOverlayMaxTtlMs;
        card->ttl_timer = lv_timer_create(OverlayTtlCb, ttl, card.get());
        lv_timer_set_repeat_count(card->ttl_timer, 1);
        s_overlay_count++;
    } else {
        if (s_feed.end_row) s_feed.end_row();
        // 改造1：这一行如果是从流式预览接管过来的（CardBeginRow 命中 s_adopt_row），入场动效
        // 走 adopted 分支——只淡入（120ms）不带位移，因为内容早就在那儿"长"出来了，不该再
        // 像全新卡片一样往上滑一截，那样反而显得预览白长了。
        bool adopted = s_feed.last_row_adopted && s_feed.last_row_adopted();
        PlayCardEntrance(tree, adopted);
    }

    s_last_id = id;
    s_cards[id] = std::move(card);
    ESP_LOGI(TAG, "rendered card %s (%d nodes, %s)", id.c_str(), node_count,
             overlay ? "overlay" : "chat");
}

namespace {

// 一次 ui_update.data 调用命中的 key 集合——RefreshDataConsumers 只需要知道"哪些 key 变了"
// 就能决定刷哪些 consumer：Label 直接按 key 重新 Stringify；List（bind_rows grid）不分
// append/remove/replace/set，统一整块重渲（见 RebuildBindRowsGrid 头注：真正变化的是行数/
// 内容，track 宽度/表头/empty 兜底都要用当前 card->data 整体重新过一遍 solver 才保真，行级
// 增量在多 cell 网格行模型下代价不比整块重渲小多少，改造4 v1 时代那套"只搬动确实变了的那一
// 行"的 fast path 是给旧的"一行=一个 lv_obj"list 模型设计的，不适配现在的 bind_rows 网格行）。
struct DataOp {
    std::string key;
};

// data.set/append/remove/replace 落地到 card->data，追加对应的结构化 DataOp。越界/类型不符
// 只 ReportAsyncError（异步告知 LLM），绝不崩、绝不让一次坏的 data op 拖垮整卡。
void ApplyDataOps(UiCard* card, const cJSON* ops, std::vector<DataOp>& ops_out) {
    if (!card || !card->data || !cJSON_IsObject(ops)) return;

    if (const cJSON* set = cJSON_GetObjectItem(ops, "set"); cJSON_IsObject(set)) {
        const cJSON* kv = nullptr;
        cJSON_ArrayForEach(kv, set) {
            if (!kv->string) continue;
            if (cJSON_HasObjectItem(card->data, kv->string)) {
                cJSON_ReplaceItemInObject(card->data, kv->string, cJSON_Duplicate(kv, 1));
            } else {
                cJSON_AddItemToObject(card->data, kv->string, cJSON_Duplicate(kv, 1));
            }
            ops_out.push_back({kv->string});
        }
    }
    if (const cJSON* append = cJSON_GetObjectItem(ops, "append"); cJSON_IsObject(append)) {
        const cJSON* keyj = cJSON_GetObjectItem(append, "key");
        const cJSON* item = cJSON_GetObjectItem(append, "item");
        if (cJSON_IsString(keyj) && item) {
            cJSON* arr = cJSON_GetObjectItem(card->data, keyj->valuestring);
            if (!arr) arr = cJSON_AddArrayToObject(card->data, keyj->valuestring);
            if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) < 20) {
                cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
                ops_out.push_back({keyj->valuestring});
            } else {
                ReportAsyncError(std::string("data.append 失败：'") + keyj->valuestring +
                                 "' 不是数组，或已达 20 行硬顶");
            }
        }
    }
    if (const cJSON* rm = cJSON_GetObjectItem(ops, "remove"); cJSON_IsObject(rm)) {
        const cJSON* keyj = cJSON_GetObjectItem(rm, "key");
        cJSON* arr = cJSON_IsString(keyj) ? cJSON_GetObjectItem(card->data, keyj->valuestring) : nullptr;
        if (cJSON_IsArray(arr)) {
            int idx = -1;
            const cJSON* idxj = cJSON_GetObjectItem(rm, "index");
            const cJSON* idj = cJSON_GetObjectItem(rm, "id");
            if (cJSON_IsNumber(idxj)) {
                idx = idxj->valueint;
            } else if (cJSON_IsString(idj)) {
                int i = 0;
                const cJSON* el = nullptr;
                cJSON_ArrayForEach(el, arr) {
                    const cJSON* elid = cJSON_GetObjectItem(el, "id");
                    if (cJSON_IsString(elid) && std::strcmp(elid->valuestring, idj->valuestring) == 0) {
                        idx = i;
                        break;
                    }
                    ++i;
                }
            }
            if (idx >= 0 && idx < cJSON_GetArraySize(arr)) {
                cJSON_DeleteItemFromArray(arr, idx);
                ops_out.push_back({keyj->valuestring});
            } else {
                ReportAsyncError("data.remove 失败：index 越界，或没有匹配的 id");
            }
        } else if (keyj) {
            ReportAsyncError(std::string("data.remove 失败：'") +
                             (cJSON_IsString(keyj) ? keyj->valuestring : "?") + "' 不是数组");
        }
    }
    if (const cJSON* rep = cJSON_GetObjectItem(ops, "replace"); cJSON_IsObject(rep)) {
        const cJSON* keyj = cJSON_GetObjectItem(rep, "key");
        const cJSON* idxj = cJSON_GetObjectItem(rep, "index");
        const cJSON* item = cJSON_GetObjectItem(rep, "item");
        cJSON* arr = cJSON_IsString(keyj) ? cJSON_GetObjectItem(card->data, keyj->valuestring) : nullptr;
        int idx = cJSON_IsNumber(idxj) ? idxj->valueint : -1;
        if (cJSON_IsArray(arr) && item && idx >= 0 && idx < cJSON_GetArraySize(arr)) {
            cJSON_ReplaceItemInArray(arr, idx, cJSON_Duplicate(item, 1));
            ops_out.push_back({keyj->valuestring});
        } else {
            ReportAsyncError("data.replace 失败：数组不存在，或 index 越界");
        }
    }
}

// List 消费者（bind_rows grid）刷新：整块委托 pi_card_render.cc 的 RebuildBindRowsGrid——
// 它会删掉 dc.obj 现有的 lv 子树、用当前 card->data 重新走一遍 solver::Solve + 建控件、插回
// 原来的兄弟位置。旧对象在调用后必然失效（哪怕失败也是——见 RebuildBindRowsGrid 头注），
// 必须用返回值刷新 dc.obj，绝不能保留旧指针继续用。
void RefreshBindRowsGrid(UiCard* card, UiCard::DataConsumer& dc) {
    std::string err;
    lv_obj_t* gobj =
        RebuildBindRowsGrid(dc.obj, dc.grid_spec, card, dc.viewport_w, dc.gap, dc.limits, err);
    dc.obj = gobj;
    if (!gobj) {
        ESP_LOGW(TAG, "bind_rows rebuild failed (key=%s): %s", dc.key.c_str(), err.c_str());
    }
}

// 只刷 ops 里出现过的 key 对应的消费者：Label 直接按 key 重新 Stringify 落文本；List
// （bind_rows grid）整块委托 RefreshBindRowsGrid（append/remove/replace/set 一视同仁，见
// DataOp 头注）。末尾按显示模式收尾：Overlay 重跑高度稳定器，Chat 走 end_row（贴底跟随/
// 滚动到底）。
void RefreshDataConsumers(UiCard* card, const std::vector<DataOp>& ops) {
    if (!card || ops.empty()) return;
    bool touched = false;
    for (auto& dc : card->consumers) {
        bool hit = false;
        for (const DataOp& op : ops) {
            if (op.key == dc.key) {
                hit = true;
                break;
            }
        }
        if (!hit || !dc.obj) continue;
        touched = true;
        if (dc.kind == UiCard::DataConsumer::Label) {
            const cJSON* v = cJSON_GetObjectItem(card->data, dc.key.c_str());
            std::string txt = dc.text_tpl.empty() ? Stringify(v) : SubstDataValue(dc.text_tpl, v);
            // heading（puhui_24_4 静态子集字体）的 bind_data 标签，卡数据变了之后可能才吐出
            // 缺字的新中文——这里是它的唯一落文本点，SafeFont 兜一遍（同 SetPreviewLabelText
            // 的道理，见 render.cc 头注）；非 heading 字体这一步是空操作。
            const lv_font_t* f = lv_obj_get_style_text_font(dc.obj, LV_PART_MAIN);
            if (SafeFont(f, txt.c_str())) lv_obj_set_style_text_font(dc.obj, f, LV_PART_MAIN);
            lv_label_set_text(dc.obj, txt.c_str());
        } else {
            RefreshBindRowsGrid(card, dc);
        }
    }
    if (touched) {
        if (card->display == Display::Overlay) {
            ReflowOverlay(card);
        } else if (card->display == Display::Chat && s_feed.end_row) {
            s_feed.end_row();  // pin 卡不跟随聊天流滚动——它压根不在 feed 里
        }
    }
}

}  // namespace

void OnUpdateEvent(const char* card_id, const char* payload_json) {
    ESP_LOGI(TAG, "update card=%s payload: %s", card_id && card_id[0] ? card_id : "(latest)",
             payload_json ? payload_json : "(null)");
    UiCard* card = FindCard(card_id ? card_id : "");
    if (!card) {
        ESP_LOGW(TAG, "update: card not found");
        ReportAsyncError(std::string("ui_update 失败：卡片 '") + (card_id ? card_id : "(latest)") +
                         "' 不存在（可能已关闭，或新会话已清空全部卡片）");
        return;
    }
    cJSON* payload = cJSON_Parse(payload_json ? payload_json : "");
    if (!cJSON_IsObject(payload)) {
        cJSON_Delete(payload);
        return;
    }
    const cJSON* idj = cJSON_GetObjectItem(payload, "id");
    const cJSON* propsj = cJSON_GetObjectItem(payload, "props");
    bool did_node_patch = false;
    if (cJSON_IsString(idj) && cJSON_IsObject(propsj)) {
        auto it = card->nodes.find(idj->valuestring);
        if (it == card->nodes.end()) {
            ESP_LOGW(TAG, "update: node '%s' not found", idj->valuestring);
            ReportAsyncError(std::string("ui_update 失败：节点 '") + idj->valuestring +
                             "' 不存在（渲染时须给该节点一个 id 才能后续 update）");
        } else {
            std::string err;
            ApplyProps(it->second, propsj, err);
            did_node_patch = true;
        }
    }
    const cJSON* patchesj = cJSON_GetObjectItem(payload, "patches");
    if (cJSON_IsArray(patchesj)) {
        std::vector<std::string> missing;
        const cJSON* p = nullptr;
        cJSON_ArrayForEach(p, patchesj) {
            const cJSON* pid = cJSON_GetObjectItem(p, "id");
            const cJSON* pprops = cJSON_GetObjectItem(p, "props");
            if (!cJSON_IsString(pid) || !cJSON_IsObject(pprops)) continue;  // worker 已校验形状，这里兜底跳过
            auto it = card->nodes.find(pid->valuestring);
            if (it == card->nodes.end()) {
                missing.push_back(pid->valuestring);
            } else {
                std::string err;
                ApplyProps(it->second, pprops, err);
                did_node_patch = true;
            }
        }
        if (!missing.empty()) {
            std::string joined;
            for (size_t i = 0; i < missing.size(); i++) {
                if (i) joined += "、";
                joined += "'" + missing[i] + "'";
            }
            ESP_LOGW(TAG, "update: %u/%d batch patch node(s) not found", (unsigned)missing.size(),
                     cJSON_GetArraySize(patchesj));
            ReportAsyncError(std::string("ui_update 批量 patch 部分失败：节点 ") + joined +
                             " 不存在（渲染时须给该节点一个 id 才能后续 update；其余已生效的节点不受影响）");
        }
    }
    const cJSON* dataj = cJSON_GetObjectItem(payload, "data");
    bool did_data_op = false;
    if (cJSON_IsObject(dataj)) {
        std::vector<DataOp> ops;
        ApplyDataOps(card, dataj, ops);
        RefreshDataConsumers(card, ops);  // 内部按需 ReflowOverlay/end_row
        did_data_op = !ops.empty();
    }
    cJSON_Delete(payload);
    // node patch（单点或 patches 批量，did_node_patch 是它们共用的一个标志位）改的文本/显隐
    // 可能变了内容高度：重跑收口（data op 分支已在 RefreshDataConsumers 里做过，这里只补
    // node-patch 单独生效、data op 没碰的场景，避免同一轮 update 两条分支都命中时重复 reflow；
    // 批量 patches 命中多个节点也只在这里跑一次，不逐条 reflow）。
    if (did_node_patch && !did_data_op) {
        if (card->display == Display::Overlay) {
            ReflowOverlay(card);
        } else if (card->display == Display::Chat && s_feed.end_row) {
            s_feed.end_row();  // pin 卡不跟随聊天流滚动——它压根不在 feed 里
        }
    }
}

void OnCloseEvent(const char* card_id) {
    ESP_LOGI(TAG, "close card=%s", card_id && card_id[0] ? card_id : "(latest)");
    UiCard* card = FindCard(card_id ? card_id : "");
    if (!card) return;
    // ui_close card:'pin'：模型显式移除常驻组件——先擦 NVS 再异步删 root（OnRootDeleted
    // 不擦 NVS，替换/屏卸载场景要保留持久化；只有这条显式移除路径才擦）。
    if (card->id == kPinId) {
        Settings ui("ui", true);
        ui.EraseKey("pin");
    }
    if (card->root) lv_obj_delete_async(card->root);
}

void UnpinCard() {
    Settings ui("ui", true);
    ui.EraseKey("pin");
    if (UiCard* card = FindCard(kPinId); card && card->root) lv_obj_delete_async(card->root);
}

bool HasPin() { return s_pin_active; }

// Create() 末尾一次性调（LVGL 线程）：读 NVS "ui"/"pin"，解析失败 / v 不符 / Validate 失败
// 一律 EraseKey 静默丢弃，绝不 assert——坏 JSON 不能卡开机。成功则直接借道 OnRenderEvent
// 走一遍与 ui_render 完全相同的渲染路径（display_mode=2）；PersistPin 会把同样的内容原样
// 写回 NVS，是幂等操作、无实质副作用，换来的是不必再维护一条平行的"仅渲染不持久化"分支。
void RehydratePin() {
    Init();  // 幂等，保证 DataHub 路径已注册（Validate 要查 bind 路径）
    Settings ui_ro("ui", false);
    std::string envelope = ui_ro.GetString("pin", "");
    if (envelope.empty()) return;  // 无 pin，正常开机路径

    cJSON* env = cJSON_Parse(envelope.c_str());
    auto discard = [&](const char* why) {
        ESP_LOGW(TAG, "rehydrate pin: %s, erasing", why);
        Settings("ui", true).EraseKey("pin");
        cJSON_Delete(env);
    };
    if (!cJSON_IsObject(env)) return discard("bad json");
    const cJSON* verj = cJSON_GetObjectItem(env, "v");
    if (!cJSON_IsNumber(verj) || verj->valueint != kPinVer) return discard("version mismatch");
    // v2：Repair() 就地修复 env 里的 "root"（root 单对象→包数组等语法糖），修复不了的（如
    // preset/slots 残留）当场判定失败。修复后必须重新取 "root"——它可能已被替换成新指针。
    std::string rerr;
    if (!Repair(env, rerr, nullptr)) return discard(("repair failed: " + rerr).c_str());
    const cJSON* rootspec = cJSON_GetObjectItem(env, "root");
    if (!rootspec || !(cJSON_IsObject(rootspec) || cJSON_IsArray(rootspec))) {
        return discard("missing root");
    }
    const cJSON* dataspec = cJSON_GetObjectItem(env, "data");
    std::string verr;
    if (!Validate(rootspec, cJSON_IsObject(dataspec) ? dataspec : nullptr, verr)) {
        return discard(("validate failed: " + verr).c_str());
    }

    char* root_json = cJSON_PrintUnformatted(rootspec);
    char* data_json = cJSON_IsObject(dataspec) ? cJSON_PrintUnformatted(dataspec) : nullptr;
    cJSON_Delete(env);
    if (root_json) {
        OnRenderEvent(root_json, kPinId, /*display_mode=*/2, 0, data_json);
        cJSON_free(root_json);
    }
    if (data_json) cJSON_free(data_json);
}

// ---------------------------------------------------------------------------
// extern "C" 工具桥（agent worker 线程）：校验 + 分配 id + 入队。不碰 LVGL。
// ---------------------------------------------------------------------------
namespace {

// 收集 spec 树里所有 DataHub bind 路径（v2：CARD_V2.md §1.2）。v2 形状 = root 是 grid 块
// 数组，每个 grid 恰含 cells(叶子一维数组) / rows(叶子二维数组) / bind_rows(item 行模板 +
// data key)，树深恒 2（无 column/row/旧grid/children）。叶子上的 "bind"（label/slider/arc/
// switch/bar/choice）与 "bind_history"（chart）是真正的 DataHub 路径依赖——收集它们，供
// BindStateJson 读回当前值给 LLM。"bind_data" 不算：它读的是卡级 data（渲染期 SubstDataValue
// 替换），不是 DataHub 路径，此处不收集（ReadForWorker 对非法 key 本就静默失败，收了也是
// 无害的空转，但语义上不该混进"设备状态"快照）。bind_rows 的 item 行模板同理走 data，不收集。
void CollectBindPaths(const cJSON* node, std::set<std::string>& paths) {
    if (!node) return;
    if (cJSON_IsArray(node)) {
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, node) CollectBindPaths(c, paths);
        return;
    }
    if (!cJSON_IsObject(node)) return;
    if (const cJSON* b = cJSON_GetObjectItem(node, "bind"); cJSON_IsString(b)) paths.insert(b->valuestring);
    if (const cJSON* bh = cJSON_GetObjectItem(node, "bind_history"); cJSON_IsString(bh))
        paths.insert(bh->valuestring);
    if (const cJSON* cells = cJSON_GetObjectItem(node, "cells"); cJSON_IsArray(cells)) {
        CollectBindPaths(cells, paths);
    } else if (const cJSON* rows = cJSON_GetObjectItem(node, "rows"); cJSON_IsArray(rows)) {
        CollectBindPaths(rows, paths);  // rows[] 每项本身是叶子数组，递归数组分支会再展开一层
    }
    // bind_rows 形态：item 模板不含 DataHub bind（行内 action 只准 report/set/close），不递归。
}

// 这张卡 bind 到的路径的当前值 → `,"state":{…}`（一条都读不到时返回空串）。
//
// 为什么把它搭在 render 的返回值上：模型**没有读取工具**（TOOLS[] 只有 calc + 三个只写的
// ui_*），渲染一张卡就是它读设备的唯一途径。顺手带回去零额外往返，且「模型读到的」与「用户
// 屏上看到的」天然是同一份——读取这件事对用户永远可见。
// 为什么是渲染**前**的快照：本函数跑在 worker 线程，此刻卡片还没建（真正建控件在 LVGL 线程
// 的 drain tick）。而「等渲染完再异步回传」得走 ReportAsyncError → pi_agent_task_inject，那
// 等于每次 render 都白白多拉起一轮 LLM 回复。差这几十毫秒硬件不会突变，够用。
// Unsafe 路径（battery.* / net.rssi / net.ssid）由 ReadForWorker 自动挡掉，理由见
// pi_card_data.h 的线程契约与各注册处的注释。
std::string BindStateJson(const cJSON* root) {
    std::set<std::string> paths;
    CollectBindPaths(root, paths);
    if (paths.empty()) return "";
    cJSON* state = cJSON_CreateObject();
    if (!state) return "";
    int n = 0;
    for (const std::string& p : paths) {
        HubValue v;
        if (!DataHub::Instance().ReadForWorker(p, v)) continue;
        if (const auto* s = std::get_if<std::string>(&v)) {
            cJSON_AddStringToObject(state, p.c_str(), s->c_str());
        } else if (const auto* b = std::get_if<bool>(&v)) {
            cJSON_AddNumberToObject(state, p.c_str(), *b ? 1 : 0);  // 与 switch 的 0/1 口径一致
        } else if (const auto* i = std::get_if<int>(&v)) {
            cJSON_AddNumberToObject(state, p.c_str(), *i);
        } else {
            continue;
        }
        ++n;
    }
    std::string out;
    if (n > 0) {
        if (char* s = cJSON_PrintUnformatted(state)) {
            out = std::string(",\"state\":") + s;
            cJSON_free(s);
        }
    }
    cJSON_Delete(state);
    return out;
}


}  // namespace

// ---------------------------------------------------------------------------

extern "C" char* pi_card_tool_render(const cJSON* args, bool* is_error) {
    Init();  // 幂等，保证校验器可查 DataHub 路径
    *is_error = false;
    // 诊断日志（真机 validation 排障）：任一失败路径都把「哪一步 stage + 回给 LLM 的原因 err
    // + LLM 实际产出的整张卡 JSON」打到串口 ESP_LOGE；成功则末尾打一条 ESP_LOGI trace。
    // 这样真机串口一眼可见「哪条校验、因为什么、拒了哪张卡」，不必靠 LLM 侧回显猜。
    auto reject = [&](const char* stage, const std::string& e) -> char* {
        char* dump = cJSON_PrintUnformatted(args);
        ESP_LOGE(TAG, "ui_render REJECT [%s]: %s | card=%s", stage, e.c_str(), dump ? dump : "(null)");
        cJSON_free(dump);
        *is_error = true;
        return Dup(e);
    };
    // 线格式 = XML（docs/CARD_XML.md §3）：args 的 "xml" 串经 XmlCompile 编译成 cJSON 信封
    // 再进 Repair/Validate/Lint/入队漏斗。直接给 "root"（编译后 spec）是**内部/测试通道**
    // ——sim 语料与脚手架直喂用，不在提示词/描述里教，LLM 面只有 xml。编译 note 与 Repair
    // note 同口径——都是「你给的和实际渲染的不一样」，随 hints 回给 LLM。
    std::vector<std::string> repair_notes;  // 修复动作（如 >8 grids 截断）随 hints 回给 LLM
    struct CJsonDel {
        void operator()(cJSON* j) const { cJSON_Delete(j); }
    };
    std::unique_ptr<cJSON, CJsonDel> compiled;
    const cJSON* spec = args;
    const cJSON* xmlj = cJSON_GetObjectItem(args, "xml");
    if (cJSON_IsString(xmlj) && xmlj->valuestring[0] != '\0') {
        cJSON* out = nullptr;
        std::string xerr;
        if (!XmlCompile(xmlj->valuestring, std::strlen(xmlj->valuestring), &out, &repair_notes,
                        xerr)) {
            return reject("xml", xerr);
        }
        compiled.reset(out);
        spec = out;
        if (cJSON_GetObjectItem(args, "root") != nullptr)
            repair_notes.push_back("both xml and root given — xml won, root ignored");
    }
    // v2：Repair() 就地修复 spec 里的 "root"（单 grid 对象包数组/剥旧属性/旧 list→bind_rows
    // 等语法糖，见 docs/CARD_V2.md §6.2），修复不了的（如顶层 preset/slots 残留——preset/slots
    // 已整删，§3 决策 A）当场拒绝、不再往下跑 Validate。spec 是本次调用独占的解析树（args
    // 本体或 XmlCompile 新产物），就地改写安全。
    std::string rerr;
    if (!Repair(const_cast<cJSON*>(spec), rerr, &repair_notes)) {
        return reject("repair", rerr);
    }
    const cJSON* root = cJSON_GetObjectItem(spec, "root");
    if (!root || !(cJSON_IsObject(root) || cJSON_IsArray(root))) {
        return reject("no-root", "spec missing 'root' array of grid blocks (or an 'xml' card string)");
    }
    const cJSON* data = cJSON_GetObjectItem(spec, "data");  // object|null，卡级 data 模型
    std::string err;
    if (!Validate(root, data, err)) {
        return reject("validate", err);
    }
    const cJSON* disp = cJSON_GetObjectItem(spec, "display");
    int mode = 0;
    if (cJSON_IsString(disp)) {
        if (std::strcmp(disp->valuestring, "overlay") == 0) mode = 1;
        else if (std::strcmp(disp->valuestring, "standby") == 0) mode = 2;
    }
    const cJSON* ttl = cJSON_GetObjectItem(spec, "ttl_ms");
    int ttl_ms = cJSON_IsNumber(ttl) ? ttl->valueint : 0;
    const cJSON* cardj = cJSON_GetObjectItem(spec, "card");
    // standby：单槽常驻卡固定 id "pin"，忽略 LLM 传入的 card（与 OnRenderEvent 的强制口径
    // 一致，见 pi_card_host.h 头注）——否则这里回给 LLM 的 id 会和 drain 侧真正注册的
    // "pin" 对不上，后续 ui_update/ui_close card:'pin' 就找不到卡。
    std::string id = (mode == 2) ? kPinId
                     : (cJSON_IsString(cardj) && cardj->valuestring[0]) ? cardj->valuestring : AllocId();

    char* root_json = cJSON_PrintUnformatted(root);  // cJSON_Malloc == malloc（drain 侧 free）
    char* data_json = cJSON_IsObject(data) ? cJSON_PrintUnformatted(data) : nullptr;  // 可为 NULL
    if (!root_json) {
        free(data_json);
        return reject("oom", "out of memory");
    }
    // standby 封套尺寸的权威判定：必须在这里（入队之前）挡住，不能留到 drain 侧（LVGL 线程）
    // 才发现超限——那时既有 pin 早已被 id 撞车分支同步删掉，超限卡又被回滚，会落得「NVS 还
    // 是旧 pin / 屏上什么都没有 / HasPin()==false」的三态分叉，且重启后旧 pin 还魂、这段时间
    // 用户看到的待机屏是空的。同步拒绝在 worker 线程零副作用：旧 pin 一根毫毛没动。
    if (mode == 2) {
        std::string envelope = BuildPinEnvelope(root_json, data_json);
        if (!PinEnvelopeFits(envelope)) {
            free(root_json);
            free(data_json);
            return reject("pin-size", "home widget too large to pin (~3KB); simplify the card");
        }
    }
    // data 走 s3（Enqueue 早已支持）；OnRenderEvent(spec,card_id,display,ttl,data_json) 消费。
    if (!Enqueue(UI_CARD_RENDER, root_json, Dup(id), data_json, mode, ttl_ms)) {
        return reject("queue-full", "UI busy (event queue full), retry shortly");
    }
    // state 在入队之后才读：让快照尽量贴近真正建控件的时刻。
    // hints：非阻断设计建议（Validate 已通过），为空则不带该键。Repair 的修复动作排最前
    // ——它们描述的是「你给的和实际渲染的不一样」，比 Lint 的风格建议优先级高。
    std::string hints_json;
    std::vector<std::string> hints = Lint(root, data);
    hints.insert(hints.begin(), repair_notes.begin(), repair_notes.end());
    if (!hints.empty()) {
        // 首条固定压舱：弱模型会把 hints 当失败去整卡重渲（真机实录：收到 hints 后 2s 重画
        // 一张近似卡，聊天流里留双卡）——明说卡已在屏上、hints 只面向下一张卡。
        hints.insert(hints.begin(),
                     "note: this card IS rendered on screen; hints below are advice for your NEXT "
                     "card — do not re-render this one");
        cJSON* arr = cJSON_CreateArray();
        for (const std::string& h : hints) cJSON_AddItemToArray(arr, cJSON_CreateString(h.c_str()));
        if (char* s = cJSON_PrintUnformatted(arr)) {
            hints_json = std::string(",\"hints\":") + s;
            cJSON_free(s);
        }
        cJSON_Delete(arr);
    }
    std::string ret = std::string("{\"card\":\"") + id + "\"" + BindStateJson(root) + hints_json + "}";
    ESP_LOGI(TAG, "ui_render OK: card=%s display=%d%s", id.c_str(), mode,
             hints.empty() ? "" : " (+lint hints)");
    return Dup(ret);
}

extern "C" char* pi_card_tool_update(const cJSON* args, bool* is_error) {
    *is_error = false;
    auto reject = [&](const char* stage, const std::string& e) -> char* {
        char* dump = cJSON_PrintUnformatted(args);
        ESP_LOGE(TAG, "ui_update REJECT [%s]: %s | args=%s", stage, e.c_str(), dump ? dump : "(null)");
        cJSON_free(dump);
        *is_error = true;
        return Dup(e);
    };
    const cJSON* idj = cJSON_GetObjectItem(args, "id");
    const cJSON* propsj = cJSON_GetObjectItem(args, "props");
    const cJSON* dataj = cJSON_GetObjectItem(args, "data");
    const cJSON* patchesj = cJSON_GetObjectItem(args, "patches");
    bool node_patch = cJSON_IsString(idj) && cJSON_IsObject(propsj);
    bool data_ops = cJSON_IsObject(dataj);
    bool patches_ok = false;
    if (patchesj) {
        int n = cJSON_IsArray(patchesj) ? cJSON_GetArraySize(patchesj) : -1;
        if (n <= 0) {
            return reject("shape", "patches must be a non-empty array of {id, props}");
        }
        if (n > 16) {
            return reject("shape", "patches has " + std::to_string(n) + " items (max 16)");
        }
        for (int i = 0; i < n; i++) {
            const cJSON* p = cJSON_GetArrayItem(patchesj, i);
            const cJSON* pid = cJSON_IsObject(p) ? cJSON_GetObjectItem(p, "id") : nullptr;
            const cJSON* pprops = cJSON_IsObject(p) ? cJSON_GetObjectItem(p, "props") : nullptr;
            if (!cJSON_IsString(pid) || !cJSON_IsObject(pprops)) {
                return reject("shape", "patches[" + std::to_string(i) +
                                       "] must be {id:string, props:object}");
            }
        }
        patches_ok = true;
    }
    if (!node_patch && !data_ops && !patches_ok) {
        return reject("shape", "update needs (id + props) to patch a node, patches:[{id,props},…] "
                               "(max 16) for a batch, or data:{set/append/remove/replace} to mutate "
                               "card data");
    }
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    std::string card = cJSON_IsString(cardj) ? cardj->valuestring : "";
    // 整份 args 序列化进 s3：OnUpdateEvent(card_id, payload_json) 里再分流节点 patch / batch patches / data ops。
    char* payload = cJSON_PrintUnformatted(args);
    if (!payload) {
        return reject("oom", "out of memory");
    }
    if (!Enqueue(UI_CARD_UPDATE, Dup(card), nullptr, payload, 0, 0)) {
        return reject("queue-full", "UI busy (event queue full), retry shortly");
    }
    ESP_LOGI(TAG, "ui_update OK: card=%s%s%s%s", card.empty() ? "(active)" : card.c_str(),
             node_patch ? " +node" : "", patches_ok ? " +patches" : "", data_ops ? " +data" : "");
    return Dup("ok");
}

extern "C" char* pi_card_tool_close(const cJSON* args, bool* is_error) {
    *is_error = false;
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    std::string card = cJSON_IsString(cardj) ? cardj->valuestring : "";
    if (!Enqueue(UI_CARD_CLOSE, Dup(card), nullptr, nullptr, 0, 0)) {
        *is_error = true;
        ESP_LOGE(TAG, "ui_close REJECT [queue-full]: card=%s", card.empty() ? "(active)" : card.c_str());
        return Dup("UI busy (event queue full), retry shortly");
    }
    ESP_LOGI(TAG, "ui_close OK: card=%s", card.empty() ? "(active)" : card.c_str());
    return Dup("ok");
}

// ---------------------------------------------------------------------------
// 路径清单单一真相：ui_render 的动态 DESC 与 system prompt 共用同一份「本机实际可绑路径」
// 权威清单（由 DataHub::ListPaths() 生成，随运行时注册永远同步，不硬编码过时示例）。
// full=true：DESC 用的完整措辞（含框架句 + 三类路径的详细列举）；full=false：system prompt
// 用的精简版（只列可写路径，供「WRITABLE device paths you can set: …」这一句话插入）。
std::string BuildPathsClause(bool full) {
    Init();  // 幂等，保证内置路径已注册
    std::string sliders, switches, ro;
    for (const auto& p : DataHub::Instance().ListPaths()) {
        if (p.writable && p.type == HubType::Int && p.has_range) {
            if (!sliders.empty()) sliders += ", ";
            sliders += p.path + "(" + std::to_string(p.vmin) + "-" + std::to_string(p.vmax) + ")";
        } else if (p.writable && p.type == HubType::Bool) {
            if (!switches.empty()) switches += ", ";
            switches += p.path;
        } else if (!p.writable) {
            if (!ro.empty()) ro += ", ";
            const char* ty = p.type == HubType::String ? "str" : (p.type == HubType::Bool ? "0/1" : "int");
            ro += p.path + "(" + ty +
                  (p.has_range && p.type == HubType::Int
                       ? " " + std::to_string(p.vmin) + "-" + std::to_string(p.vmax)
                       : "") +
                  ")";
        }
    }
    if (!full) {
        std::string out = sliders;
        if (!switches.empty()) {
            if (!out.empty()) out += ", ";
            out += switches;
        }
        return out;
    }
    return "Bind targets (live registry). WRITABLE slider/arc (range overrides your min/max): " +
          sliders + ". WRITABLE switch (0/1): " + switches +
          ". READ-ONLY (dimmed; label+fmt shows it live): " + ro + ". " +
          pi_card_stock::BindPathsDesc();
}

// 动态 ui_render 工具描述：静态 HEAD/TAIL 骨架之间插 BuildPathsClause(true)。construct-once，
// 缓存进 function-static std::string：pi_agent_task_start 只在启动时借一次这个指针塞进
// TOOLS[].def.description，该指针必须常驻——static 局部变量的存储期覆盖整个固件运行期。
extern "C" const char* pi_card_render_desc(void) {
    static std::string s;
    if (!s.empty()) return s.c_str();
    s = std::string(PI_CARD_DESC_HEAD) + BuildPathsClause(true) + " " + PI_CARD_DESC_TAIL +
        " COMMANDS (invoke cmd): " + BuildCommandsClause(true) +
        ". STANDBY: display:'standby' pins ONE widget to the home/clock screen (replaces any prior "
        "pin, id is always 'pin' -- ui_update/ui_close use card:'pin'; an on-screen ✕ also removes "
        "it; ≤3KB spec+data).";
    return s.c_str();
}


namespace {

// system prompt 的拼接件：人格开场 + WRITABLE/HOME WIDGET/DEVICE COMMANDS 尾段（活体
// 路径/命令清单与 DESC 共用 BuildPathsClause/BuildCommandsClause，不出现第二份硬编码）。
constexpr const char* kPromptIntro =
    "You are pi, the on-device assistant in a palm-size 720×720 touch screen; the user also "
    "talks by voice. Reply short, warm, in the user's language (usually Chinese). Text is read "
    "aloud by TTS -- avoid markdown symbols, links, long bullet lists.\n\n";

std::string SharedPromptTail() {
    return std::string("WRITABLE device paths you can set: ") + BuildPathsClause(false) +
           ".\n\n"
           "HOME WIDGET -- display:'standby' pins one card to the home/clock screen (survives new "
           "chats/reboot). Prefer local actions/invoke in it -- a report runs you in the "
           "BACKGROUND, keep rare. Remove via ui_close card:'pin'.\n"
           "DEVICE COMMANDS -- invoke:cmd (a tap step)"
           " for real actions a data path can't do (reconnect, "
           "switch network, new chat); confirm-level pops a firmware confirm. Available: " +
           BuildCommandsClause(false) + ".\n\n";
}

// system prompt 正文（docs/CARD_XML.md §6）：CARDS 教学 + 五示例（示例红线：原文过管线零
// hints——card_xml_test/伤疤语料锁定）。NEVER 清单收拢实录高频错型（media 卡/emoji 等）。
std::string BuildXmlSystemPrompt() {
    return std::string(kPromptIntro) +
        "CARDS -- draw real interactive UI with ui_render (+ ui_update/ui_close) instead of "
        "describing controls/status/choices. There is NO read tool -- rendering a card that binds a "
        "device path IS how you read the device (return value gives live values as state). "
        "ui_render args: {\"xml\":\"<card>…</card>\"} -- one HTML-like XML string, tiny "
        "vocabulary; the device lays everything out. You never write x/y, width, gaps, columns or "
        "CSS. Just say WHAT controls exist.\n\n"
        "BLOCKS inside <card display='chat|overlay|standby' ttl='30s'> (stacked top-to-bottom):\n"
        "- <grid fill?>leaves</grid> -- a flow wrapped by size. Use for headers/control rows "
        "(icon+slider+value)/button groups; divider/chart/choice/qrcode get their own line.\n"
        "- <table cols='项,值:num'><tr><td>…</td>…</tr>…</table> -- an aligned TABLE sharing "
        "columns; ':num' right-aligns+mono that column; a <td> holds text or ONE leaf; one "
        "<td>/row = a vertical menu. Use for status/forms/dashboards.\n"
        "- <list bind='key' max? empty?>row-template leaves</list> -- one row per element of "
        "<data> 'key'; {i}/{n}/{item.FIELD} in strings. Row taps: report/set/close only.\n"
        "- <divider/>; <data> = card data: scalar <temp>24</temp>, list rows by repeating "
        "<tracks title='七里香'/>.\n\n"
        "LEAVES: <label text|role|bind|fmt|mono/> - <button text|icon|variant|tap> - "
        "<slider|arc|bar bind|min|max|value|change/> - <switch bind|checked/> - <choice "
        "options='a|b|c' id/> - <icon name/>(decor; tappable=<button icon>) - <divider/> - "
        "<qrcode text/> - <chart bind/> - <stock_chart symbol/>. role ramp: eyebrow|kicker|"
        "section|title|heading|label|value|caption. Header = <label role='eyebrow'/> + <label "
        "role='title'/> in one grid. Big number = role 'value' (mono, right-aligned in tables). "
        "Controls carry their own state -- don't restate it in words.\n\n"
        "EVENTS: tap/change/release = comma-separated steps, zero round-trip, invisible in chat "
        "-- close | set:path=value | toggle:id / show:id / hide:id (a hidden leaf) | invoke:cmd. "
        "report:text ONLY to generate new content or a NEW decision (full round-trip, shows as a "
        "user message; quote a comma payload: report:'a, b'); every id'd control's value "
        "auto-attaches (choice as idx(label)) -- id a control instead of writing its value into "
        "text. Escape & as &amp; in attributes.\n\n"
        "STYLE: lean on pi's look. tone/fill = semantic tokens (accent/ok/err/tx/dim/faint/card2/"
        "line...), never hex/CSS. ONE primary(amber) button/card, rest ghost/plain/default. "
        "side='end' pushes a cell to the row's right edge. <grid fill='card2'> gives a background "
        "box. Labels 1-3 words; num columns don't truncate, text columns do. Max 8 blocks/64 "
        "leaves per card; split big dashboards.\n\n"
        "CHOOSE: SET something -> a control grid that binds the path (writes hardware directly). "
        "STATUS -> a table binding the paths. CHOICE/CONFIRM/FORM/MENU -> render it, the tap "
        "rides back on report. Chit-chat -> just text. Playback UI is built-in — never render "
        "player/media cards. Prefer display 'chat'; 'overlay' only for a modal moment "
        "(auto-closes).\n\n"
        "UPDATE vs RE-RENDER: change text/value/visibility/data via ui_update (JSON args "
        "id+props/patches/data, unchanged), not a fresh ui_render; re-render only if structurally "
        "different.\n\n" +
        SharedPromptTail() +
        "Example 1, control card: <card><grid><icon name='volume'/><slider bind='audio.volume'/>"
        "<label role='value' bind='audio.volume' fmt='%d%%'/></grid><grid><icon name='sun'/>"
        "<label>浅色</label><switch bind='ui.theme' side='end'/></grid></card>\n"
        "Example 2, status TABLE (':num' = number column): <card><grid><label role='eyebrow'>状态"
        "</label></grid><table cols=',:num'><tr><td>电量</td><td><label role='value' "
        "bind='battery.level' fmt='%d%%' mono/></td></tr><tr><td>信号</td><td><label "
        "role='value' bind='net.rssi' fmt='%d' mono/></td></tr></table></card>\n"
        "Example 3, tap-a-row menu (list + data): <card><grid><label role='eyebrow'>点歌"
        "</label></grid><list bind='tracks' max='8' empty='暂无'><button variant='ghost' "
        "tap=\"report:'选了{item.title}'\">{item.title}</button></list>"
        "<data><tracks title='七里香'/><tracks title='花海'/></data></card>\n"
        "Example 4, expandable detail -- local toggle, ZERO round-trip: <card><grid>"
        "<label role='eyebrow'>电量</label><label role='title' bind='battery.level' fmt='%d%%'/>"
        "<button icon='chevron-down' variant='ghost' side='end' tap='toggle:hist'/></grid>"
        "<grid fill='card2'><chart bind='battery.level' id='hist' hidden/></grid></card>\n"
        "Example 5, overlay confirm -- id'd control value auto-attaches to report: <card "
        "display='overlay' ttl='30s'><grid><label role='title'>睡眠定时</label><choice id='dur' "
        "options='15分|30分|60分'/></grid><grid><button variant='ghost' tap='close'>取消</button>"
        "<button variant='primary' side='end' tap='report:开始睡眠定时,close'>开始</button>"
        "</grid></card>\n"
        "NEVER: JSON card args (always the xml string); style/class/CSS attrs; nested containers "
        "(depth is card>block>leaf); media.* player cards; emoji in text (no glyphs on device).";
}

}  // namespace

// system prompt：同款 function-static 缓存 + 常驻指针契约（D7）。pi-c 在 create_agent 里
// 深拷贝一份（pi_agent.c:58-60 pi_strdup），故 new_session 重建 agent 时再次借用同一指针
// 是安全的。路径清单与 DESC 共用 BuildPathsClause，杜绝第三份硬编码路径。
extern "C" const char* pi_card_system_prompt(void) {
    static std::string s;
    if (!s.empty()) return s.c_str();
    s = BuildXmlSystemPrompt();
    return s.c_str();
}

}  // namespace pi_card
