#include "pi_card_host.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "esp_log.h"

#include "pi_card_data.h"
#include "pi_card_icons.h"
#include "pi_card_render.h"
#include "pi_card_tools.h"
#include "pi_theme.h"
#include "pi_ui_bridge.h"
#include "screen_util.h"

#define TAG "pi_card_host"

namespace pi_card {

namespace {

// ---- 注册表 / 状态（cards_ 只在 LVGL 线程被动，无需锁；id 计数跨线程原子）----
std::map<std::string, std::unique_ptr<UiCard>> s_cards;
std::string s_last_id;
std::atomic<int> s_next_id{1};
int s_overlay_count = 0;
FeedHooks s_feed{};

// overlay 护栏：浮层会阻止息屏（见 pi_screen 的 sleep 门控），无 ttl 的浮层若被 LLM
// 忘关会让屏幕永不熄灭、持续耗电，故每个 overlay 都有一个保底最大存活期；同时封顶
// 同屏叠加的浮层数，防失控刷屏把 scrim 层层堆满。
constexpr int kOverlayMaxTtlMs = 5 * 60 * 1000;  // 5min，与最长息屏档呼应
constexpr int kMaxOverlays = 3;

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
    if (xQueueSend(pi_ui_queue(), &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pi_ui_queue full, dropping card evt kind=%d", static_cast<int>(kind));
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
    if (card && card->root) lv_obj_delete_async(card->root);
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
    screen_swipe_back_ignore(scrim, true);          // 别触发 Chat→Idle 右滑

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
    screen_swipe_back_ignore(btn, true);
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
    if (card->display == Display::Overlay && s_overlay_count > 0) s_overlay_count--;
    std::string id = card->id;
    if (s_last_id == id) s_last_id.clear();
    s_cards.erase(id);  // 释放 UiCard（lv_obj 由 LVGL 释放）
}

}  // namespace

// ---------------------------------------------------------------------------
void Init() { DataHub::Instance().RegisterBuiltins(); }

void SetFeedHooks(const FeedHooks& hooks) { s_feed = hooks; }

bool HasOpenOverlay() { return s_overlay_count > 0; }

// LVGL 线程：真正建控件。spec_json 是 root 子树；display/ttl/id 由事件带入。
void OnRenderEvent(const char* spec_json, const char* card_id, int display_mode, int ttl_ms) {
    cJSON* root = cJSON_Parse(spec_json ? spec_json : "");
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "render: bad root json");
        cJSON_Delete(root);
        return;
    }
    const bool overlay = display_mode != 0;
    if (overlay && s_overlay_count >= kMaxOverlays) {
        ESP_LOGW(TAG, "overlay cap reached (%d), dropping card render", s_overlay_count);
        ReportAsyncError("同屏浮层已达上限，本次渲染被丢弃；请先 ui_close 旧卡再重试");
        cJSON_Delete(root);
        return;
    }
    std::string id = (card_id && card_id[0]) ? card_id : AllocId();

    // 显式 id 撞车：先同步删旧卡（触发其 OnRootDeleted 清理），避免覆盖注册表
    // 时把旧 UiCard 释放而其 root 仍挂着指向它的 DELETE 回调。
    if (auto it = s_cards.find(id); it != s_cards.end()) {
        if (it->second->root) lv_obj_delete(it->second->root);
    }

    auto card = std::make_unique<UiCard>();
    card->id = id;
    card->display = overlay ? Display::Overlay : Display::Chat;

    lv_obj_t* delete_root = nullptr;
    lv_obj_t* render_parent = nullptr;
    lv_obj_t* wrapper = nullptr;
    if (overlay) {
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

    if (overlay) {
        // 浮层卡片加柔和投影，从遮罩上"浮起"（聊天流内的卡片保持扁平，不加）。
        lv_obj_set_style_shadow_width(tree, 40, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(tree, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(tree, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(tree, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(tree, LV_OPA_40, LV_PART_MAIN);
        // 稳定性护栏：LLM 给出的卡片再高也不溢出屏幕——封顶 86% 屏高、内部竖向滚动。
        int32_t vres = lv_display_get_vertical_resolution(nullptr);
        lv_obj_set_style_max_height(tree, vres * 86 / 100, LV_PART_MAIN);
        lv_obj_add_flag(tree, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(tree, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(tree, LV_SCROLLBAR_MODE_AUTO);
        screen_swipe_back_ignore(tree, true);  // 内部竖滑归卡片，不误触屏级手势
        AddOverlayCloseButton(wrapper, card.get());
        // 保底 TTL：无 ttl（0）或 LLM 给的超长 ttl 一律封顶到 kOverlayMaxTtlMs，避免浮层
        // 永久阻止息屏。显式 ttl 在封顶内则照用。
        int ttl = (ttl_ms > 0 && ttl_ms < kOverlayMaxTtlMs) ? ttl_ms : kOverlayMaxTtlMs;
        card->ttl_timer = lv_timer_create(OverlayTtlCb, ttl, card.get());
        lv_timer_set_repeat_count(card->ttl_timer, 1);
        s_overlay_count++;
    } else {
        if (s_feed.end_row) s_feed.end_row();
    }

    s_last_id = id;
    s_cards[id] = std::move(card);
    ESP_LOGI(TAG, "rendered card %s (%d nodes, %s)", id.c_str(), node_count,
             overlay ? "overlay" : "chat");
}

void OnUpdateEvent(const char* card_id, const char* node_id, const char* props_json) {
    UiCard* card = FindCard(card_id ? card_id : "");
    if (!card) {
        ESP_LOGW(TAG, "update: card not found");
        ReportAsyncError(std::string("ui_update 失败：卡片 '") + (card_id ? card_id : "(latest)") +
                         "' 不存在（可能已关闭，或新会话已清空全部卡片）");
        return;
    }
    auto it = card->nodes.find(node_id ? node_id : "");
    if (it == card->nodes.end()) {
        ESP_LOGW(TAG, "update: node '%s' not found", node_id ? node_id : "");
        ReportAsyncError(std::string("ui_update 失败：节点 '") + (node_id ? node_id : "") +
                         "' 不存在（渲染时须给该节点一个 id 才能后续 update）");
        return;
    }
    cJSON* props = cJSON_Parse(props_json ? props_json : "");
    if (!cJSON_IsObject(props)) {
        cJSON_Delete(props);
        return;
    }
    std::string err;
    ApplyProps(it->second, props, err);
    cJSON_Delete(props);
    if (card->display == Display::Chat && s_feed.end_row) s_feed.end_row();
}

void OnCloseEvent(const char* card_id) {
    UiCard* card = FindCard(card_id ? card_id : "");
    if (card && card->root) lv_obj_delete_async(card->root);
}

// ---------------------------------------------------------------------------
// extern "C" 工具桥（agent worker 线程）：校验 + 分配 id + 入队。不碰 LVGL。
// ---------------------------------------------------------------------------
extern "C" char* pi_card_tool_render(const cJSON* args, bool* is_error) {
    Init();  // 幂等，保证校验器可查 DataHub 路径
    *is_error = false;
    const cJSON* root = cJSON_GetObjectItem(args, "root");
    if (!cJSON_IsObject(root)) {
        *is_error = true;
        return Dup("spec missing 'root' object");
    }
    std::string err;
    if (!Validate(root, err)) {
        *is_error = true;
        return Dup(err);
    }
    const cJSON* disp = cJSON_GetObjectItem(args, "display");
    int mode = (cJSON_IsString(disp) && std::strcmp(disp->valuestring, "overlay") == 0) ? 1 : 0;
    const cJSON* ttl = cJSON_GetObjectItem(args, "ttl_ms");
    int ttl_ms = cJSON_IsNumber(ttl) ? ttl->valueint : 0;
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    std::string id = (cJSON_IsString(cardj) && cardj->valuestring[0]) ? cardj->valuestring : AllocId();

    char* root_json = cJSON_PrintUnformatted(root);  // cJSON_Malloc == malloc（drain 侧 free）
    if (!root_json) {
        *is_error = true;
        return Dup("out of memory");
    }
    if (!Enqueue(UI_CARD_RENDER, root_json, Dup(id), nullptr, mode, ttl_ms)) {
        *is_error = true;
        return Dup("UI busy (event queue full), retry shortly");
    }
    return Dup(std::string("{\"card\":\"") + id + "\"}");
}

extern "C" char* pi_card_tool_update(const cJSON* args, bool* is_error) {
    *is_error = false;
    const cJSON* idj = cJSON_GetObjectItem(args, "id");
    const cJSON* propsj = cJSON_GetObjectItem(args, "props");
    if (!cJSON_IsString(idj) || !cJSON_IsObject(propsj)) {
        *is_error = true;
        return Dup("update needs string 'id' and object 'props'");
    }
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    std::string card = cJSON_IsString(cardj) ? cardj->valuestring : "";
    char* props_json = cJSON_PrintUnformatted(propsj);
    if (!props_json) {
        *is_error = true;
        return Dup("out of memory");
    }
    if (!Enqueue(UI_CARD_UPDATE, Dup(card), Dup(idj->valuestring), props_json, 0, 0)) {
        *is_error = true;
        return Dup("UI busy (event queue full), retry shortly");
    }
    return Dup("ok");
}

extern "C" char* pi_card_tool_close(const cJSON* args, bool* is_error) {
    *is_error = false;
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    std::string card = cJSON_IsString(cardj) ? cardj->valuestring : "";
    if (!Enqueue(UI_CARD_CLOSE, Dup(card), nullptr, nullptr, 0, 0)) {
        *is_error = true;
        return Dup("UI busy (event queue full), retry shortly");
    }
    return Dup("ok");
}

}  // namespace pi_card
