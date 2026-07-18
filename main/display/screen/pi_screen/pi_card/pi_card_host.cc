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
    pi_card_media::RegisterDataPaths();     // media.* 播放态路径（Stage B）
    pi_card_media::RegisterCommands();      // media.* invoke 命令（Stage B）
}

void SetFeedHooks(const FeedHooks& hooks) { s_feed = hooks; }

bool HasOpenOverlay() { return s_overlay_count > 0; }

void SetScreenOff(bool off) {
    DataHub::Instance().SetLivePaused(off);
    pi_card_stock::SetScreenOff(off);
}

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
    cJSON* root = cJSON_Parse(spec_json ? spec_json : "");
    if (!cJSON_IsObject(root)) {
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

namespace {

// data.set/append/remove/replace 落地到 card->data，收集被改的 key（供 RefreshDataConsumers
// 只刷真正变化的消费者）。越界/类型不符只 ReportAsyncError（异步告知 LLM），绝不崩、绝不
// 让一次坏的 data op 拖垮整卡。
void ApplyDataOps(UiCard* card, const cJSON* ops, std::set<std::string>& changed) {
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
            changed.insert(kv->string);
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
                changed.insert(keyj->valuestring);
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
                changed.insert(keyj->valuestring);
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
            changed.insert(keyj->valuestring);
        } else {
            ReportAsyncError("data.replace 失败：数组不存在，或 index 越界");
        }
    }
}

// 只刷 key∈changed 的消费者：Label 直接 set-text；List 全量重渲子树（保存/恢复 scroll_y）。
// 末尾按显示模式收尾：Overlay 重跑高度稳定器，Chat 走 end_row（贴底跟随/滚动到底）。
void RefreshDataConsumers(UiCard* card, const std::set<std::string>& changed) {
    if (!card || changed.empty()) return;
    bool touched = false;
    for (auto& dc : card->consumers) {
        if (changed.find(dc.key) == changed.end() || !dc.obj) continue;
        touched = true;
        if (dc.kind == UiCard::DataConsumer::Label) {
            const cJSON* v = cJSON_GetObjectItem(card->data, dc.key.c_str());
            std::string txt = dc.text_tpl.empty() ? Stringify(v) : SubstDataValue(dc.text_tpl, v);
            lv_label_set_text(dc.obj, txt.c_str());
        } else {  // List：全量重渲子树，保存/恢复 scroll_y 消除跳动
            int32_t sy = lv_obj_get_scroll_y(dc.obj);
            lv_obj_clean(dc.obj);
            const cJSON* arr = cJSON_GetObjectItem(card->data, dc.key.c_str());
            int len = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
            int rows = std::min(len, dc.eff_max);
            int node_count = 0;
            std::string rerr;
            for (int i = 0; i < rows; i++) {
                cJSON* clone = cJSON_Duplicate(dc.item_tpl, 1);
                SubstRecord(clone, cJSON_GetArrayItem(arr, i), i);
                RenderNode(dc.obj, clone, card, dc.limits, dc.depth + 1, node_count, rerr,
                          /*parent_flow=*/0, /*in_list_row=*/true);
                cJSON_Delete(clone);
            }
            if (rows == 0 && !dc.empty_text.empty()) {
                lv_obj_t* lbl = lv_label_create(dc.obj);
                lv_label_set_text(lbl, dc.empty_text.c_str());
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                const lv_font_t* f = &font_pi_mono_14;
                SafeFont(f, dc.empty_text.c_str());  // 中文兜底文案回退 puhui，不出豆腐块
                lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
                pi_theme::ApplyText(lbl, pi_theme::Tok::Faint);
            }
            lv_obj_scroll_to_y(dc.obj, sy, LV_ANIM_OFF);
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
    const cJSON* dataj = cJSON_GetObjectItem(payload, "data");
    bool did_data_op = false;
    if (cJSON_IsObject(dataj)) {
        std::set<std::string> changed;
        ApplyDataOps(card, dataj, changed);
        RefreshDataConsumers(card, changed);  // 内部按需 ReflowOverlay/end_row
        did_data_op = !changed.empty();
    }
    cJSON_Delete(payload);
    // node patch 改的文本/显隐可能变了内容高度：重跑收口（data op 分支已在
    // RefreshDataConsumers 里做过，这里只补 node-patch 单独生效、data op 没碰的场景，
    // 避免同一轮 update 两条分支都命中时重复 reflow）。
    if (did_node_patch && !did_data_op) {
        if (card->display == Display::Overlay) {
            ReflowOverlay(card);
        } else if (card->display == Display::Chat && s_feed.end_row) {
            s_feed.end_row();  // pin 卡不跟随聊天流滚动——它压根不在 feed 里
        }
    }
}

void OnCloseEvent(const char* card_id) {
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
    const cJSON* rootspec = cJSON_GetObjectItem(env, "root");
    if (!cJSON_IsObject(rootspec)) return discard("missing root");
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

// 收集 spec 树里所有 bind 路径。递归口径与渲染器/校验器一致：只有 column/row/grid 的
// children 会被渲染，别去收集一棵永远不会存在的子树。
void CollectBindPaths(const cJSON* node, std::set<std::string>& paths) {
    if (!cJSON_IsObject(node)) return;
    const cJSON* b = cJSON_GetObjectItem(node, "bind");
    if (cJSON_IsString(b)) paths.insert(b->valuestring);
    const cJSON* type = cJSON_GetObjectItem(node, "type");
    if (!cJSON_IsString(type)) return;
    if (std::strcmp(type->valuestring, "column") != 0 && std::strcmp(type->valuestring, "row") != 0 &&
        std::strcmp(type->valuestring, "grid") != 0) {
        return;
    }
    const cJSON* children = cJSON_GetObjectItem(node, "children");
    if (!cJSON_IsArray(children)) return;
    const cJSON* c = nullptr;
    cJSON_ArrayForEach(c, children) CollectBindPaths(c, paths);
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
// preset 展开：worker 侧把 {preset,slots} 拼成一棵普通 spec 树，再走既有 Validate+Render——
// preset 纯语法糖，不新增渲染路径（D5）。缺 slot 时返回具名错误串，同步回给 LLM 重试。
namespace {

cJSON* MkNode(const char* type) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    return o;
}
cJSON* MkLabel(const char* role, const std::string& text) {
    cJSON* o = MkNode("label");
    if (role) cJSON_AddStringToObject(o, "role", role);
    cJSON_AddStringToObject(o, "text", text.c_str());
    return o;
}
cJSON* AddChildren(cJSON* container) { return cJSON_AddArrayToObject(container, "children"); }
const char* Str(const cJSON* obj, const char* key, const char* dflt = nullptr) {
    const cJSON* v = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
    return (v && cJSON_IsString(v)) ? v->valuestring : dflt;
}
// dashboard 的 value 标签：role=value + bind + fmt，不带静态 text（显示全靠 bind 实时值）。
cJSON* MkBindValueLabel(const char* bind, const char* fmt) {
    cJSON* o = MkNode("label");
    cJSON_AddStringToObject(o, "role", "value");
    cJSON_AddStringToObject(o, "bind", bind);
    cJSON_AddStringToObject(o, "fmt", fmt);
    return o;
}

// header：eyebrow+title 两行小 column，四个 preset 共用。
cJSON* MkHeader(const char* eyebrow_dflt, const cJSON* slots, const char* title) {
    cJSON* header = MkNode("column");
    cJSON_AddNumberToObject(header, "gap", 2);
    cJSON* hc = AddChildren(header);
    cJSON_AddItemToArray(hc, MkLabel("eyebrow", Str(slots, "eyebrow", eyebrow_dflt)));
    cJSON_AddItemToArray(hc, MkLabel("title", title));
    return header;
}

cJSON* BuildConfirm(const cJSON* slots, std::string& err) {
    const char* title = Str(slots, "title");
    if (!title || !title[0]) {
        err = "preset 'confirm' needs slots.title (a short heading)";
        return nullptr;
    }
    cJSON* root = MkNode("column");
    cJSON_AddNumberToObject(root, "gap", 16);
    cJSON* children = AddChildren(root);
    cJSON_AddItemToArray(children, MkHeader("CONFIRM", slots, title));
    const char* body = Str(slots, "body");
    if (body && body[0]) cJSON_AddItemToArray(children, MkLabel("label", body));

    cJSON* row = MkNode("row");
    cJSON_AddNumberToObject(row, "gap", 12);
    cJSON* rc = AddChildren(row);

    cJSON* cancel = slots ? cJSON_GetObjectItem(slots, "cancel") : nullptr;
    cJSON* cancel_btn = MkNode("button");
    cJSON_AddStringToObject(cancel_btn, "variant", "ghost");
    cJSON_AddStringToObject(cancel_btn, "text", Str(cancel, "text", "取消"));
    cJSON* cancel_click = cJSON_AddArrayToObject(cancel_btn, "on_click");
    cJSON* cancel_close = cJSON_CreateObject();
    cJSON_AddStringToObject(cancel_close, "do", "close");
    cJSON_AddItemToArray(cancel_click, cancel_close);
    cJSON_AddItemToArray(rc, cancel_btn);

    cJSON* confirm = slots ? cJSON_GetObjectItem(slots, "confirm") : nullptr;
    cJSON* confirm_btn = MkNode("button");
    cJSON_AddStringToObject(confirm_btn, "variant", "primary");
    cJSON_AddStringToObject(confirm_btn, "text", Str(confirm, "text", "确认"));
    cJSON* confirm_click = cJSON_AddArrayToObject(confirm_btn, "on_click");
    cJSON* action = cJSON_CreateObject();
    const cJSON* reportj = confirm ? cJSON_GetObjectItem(confirm, "report") : nullptr;
    const cJSON* setj = confirm ? cJSON_GetObjectItem(confirm, "set") : nullptr;
    if (cJSON_IsString(reportj)) {
        cJSON_AddStringToObject(action, "do", "report");
        cJSON_AddStringToObject(action, "text", reportj->valuestring);
    } else if (cJSON_IsObject(setj)) {
        cJSON_AddStringToObject(action, "do", "set");
        cJSON_AddStringToObject(action, "path", Str(setj, "path", ""));
        const cJSON* valj = cJSON_GetObjectItem(setj, "value");
        if (cJSON_IsNumber(valj)) cJSON_AddNumberToObject(action, "value", valj->valuedouble);
    } else {
        cJSON_AddStringToObject(action, "do", "report");
        cJSON_AddStringToObject(action, "text", title);
    }
    cJSON_AddItemToArray(confirm_click, action);
    cJSON* close2 = cJSON_CreateObject();
    cJSON_AddStringToObject(close2, "do", "close");
    cJSON_AddItemToArray(confirm_click, close2);
    cJSON_AddItemToArray(rc, confirm_btn);

    cJSON_AddItemToArray(children, row);
    return root;
}

cJSON* BuildForm(const cJSON* slots, std::string& err) {
    const char* title = Str(slots, "title");
    const cJSON* fields = slots ? cJSON_GetObjectItem(slots, "fields") : nullptr;
    if (!title || !title[0] || !cJSON_IsArray(fields) || cJSON_GetArraySize(fields) == 0) {
        err = "preset 'form' needs a non-empty slots.fields array; each field needs "
              "type(slider|switch|choice), id, label";
        return nullptr;
    }
    const cJSON* f = nullptr;
    cJSON_ArrayForEach(f, fields) {
        const char* ftype = Str(f, "type");
        const char* fid = Str(f, "id");
        const char* flabel = Str(f, "label");
        if (!ftype || !fid || !flabel ||
            (std::strcmp(ftype, "slider") && std::strcmp(ftype, "switch") && std::strcmp(ftype, "choice"))) {
            err = "preset 'form' needs a non-empty slots.fields array; each field needs "
                  "type(slider|switch|choice), id, label";
            return nullptr;
        }
    }
    cJSON* root = MkNode("column");
    cJSON_AddNumberToObject(root, "gap", 12);
    cJSON* children = AddChildren(root);
    cJSON_AddItemToArray(children, MkHeader("FORM", slots, title));

    cJSON_ArrayForEach(f, fields) {
        const char* ftype = Str(f, "type");
        const char* fid = Str(f, "id");
        const char* flabel = Str(f, "label");
        cJSON* row = MkNode("row");
        cJSON* rc = AddChildren(row);
        cJSON* lbl = MkLabel("label", flabel);
        cJSON_AddNumberToObject(lbl, "grow", 1);
        cJSON_AddItemToArray(rc, lbl);
        cJSON* ctrl = MkNode(ftype);
        cJSON_AddStringToObject(ctrl, "id", fid);
        if (!std::strcmp(ftype, "slider")) {
            const cJSON* mn = cJSON_GetObjectItem(f, "min");
            const cJSON* mx = cJSON_GetObjectItem(f, "max");
            const cJSON* val = cJSON_GetObjectItem(f, "value");
            cJSON_AddNumberToObject(ctrl, "min", cJSON_IsNumber(mn) ? mn->valuedouble : 0);
            cJSON_AddNumberToObject(ctrl, "max", cJSON_IsNumber(mx) ? mx->valuedouble : 100);
            if (cJSON_IsNumber(val)) cJSON_AddNumberToObject(ctrl, "value", val->valuedouble);
            cJSON_AddNumberToObject(ctrl, "grow", 2);
        } else if (!std::strcmp(ftype, "switch")) {
            const cJSON* chk = cJSON_GetObjectItem(f, "checked");
            cJSON_AddBoolToObject(ctrl, "checked", cJSON_IsBool(chk) && cJSON_IsTrue(chk));
        } else {  // choice
            const cJSON* opts = cJSON_GetObjectItem(f, "options");
            cJSON_AddItemToObject(ctrl, "options", cJSON_IsArray(opts) ? cJSON_Duplicate(opts, 1)
                                                                       : cJSON_CreateArray());
            const cJSON* val = cJSON_GetObjectItem(f, "value");
            if (cJSON_IsNumber(val)) cJSON_AddNumberToObject(ctrl, "value", val->valuedouble);
        }
        cJSON_AddItemToArray(rc, ctrl);
        cJSON_AddItemToArray(children, row);
    }

    const cJSON* submit = slots ? cJSON_GetObjectItem(slots, "submit") : nullptr;
    cJSON* btn = MkNode("button");
    cJSON_AddStringToObject(btn, "variant", "primary");
    cJSON_AddStringToObject(btn, "text", Str(submit, "text", "提交"));
    cJSON* click = cJSON_AddArrayToObject(btn, "on_click");
    cJSON* rep = cJSON_CreateObject();
    cJSON_AddStringToObject(rep, "do", "report");
    cJSON_AddStringToObject(rep, "text", Str(submit, "report", "已提交"));
    cJSON_AddItemToArray(click, rep);
    cJSON_AddItemToArray(children, btn);
    return root;
}

cJSON* BuildDashboard(const cJSON* slots, std::string& err) {
    const char* title = Str(slots, "title");
    const cJSON* metrics = slots ? cJSON_GetObjectItem(slots, "metrics") : nullptr;
    if (!title || !title[0] || !cJSON_IsArray(metrics) || cJSON_GetArraySize(metrics) == 0) {
        err = "preset 'dashboard' needs slots.metrics:[{label,bind,...}]";
        return nullptr;
    }
    cJSON* root = MkNode("column");
    cJSON_AddNumberToObject(root, "gap", 16);
    cJSON* children = AddChildren(root);
    cJSON_AddItemToArray(children, MkHeader("STATUS", slots, title));

    const cJSON* m = nullptr;
    cJSON_ArrayForEach(m, metrics) {
        const char* label = Str(m, "label", "");
        const char* bind = Str(m, "bind");
        const char* kind = Str(m, "kind", "value");
        const char* fmt = Str(m, "fmt", "%d");
        const char* icon = Str(m, "icon");
        if (!bind) continue;  // 合法性交给既有 Validate（下游会报 unknown bind path）
        cJSON* row = MkNode("row");
        cJSON* rc = AddChildren(row);
        if (icon) {
            cJSON* ic = MkNode("icon");
            cJSON_AddStringToObject(ic, "icon", icon);
            cJSON_AddItemToArray(rc, ic);
        }
        if (!std::strcmp(kind, "bar")) {
            cJSON* lbl = MkLabel("label", label);
            cJSON_AddNumberToObject(lbl, "grow", 1);
            cJSON_AddItemToArray(rc, lbl);
            cJSON* bar = MkNode("bar");
            cJSON_AddStringToObject(bar, "bind", bind);
            cJSON_AddNumberToObject(bar, "grow", 2);
            cJSON_AddItemToArray(rc, bar);
            cJSON_AddItemToArray(rc, MkBindValueLabel(bind, fmt));
        } else {
            cJSON_AddItemToArray(rc, MkLabel("label", label));
            cJSON_AddItemToArray(rc, MkNode("spacer"));
            cJSON_AddItemToArray(rc, MkBindValueLabel(bind, fmt));
        }
        cJSON_AddItemToArray(children, row);
    }
    return root;
}

cJSON* BuildMenu(const cJSON* slots, std::string& err) {
    const char* title = Str(slots, "title");
    const cJSON* items = slots ? cJSON_GetObjectItem(slots, "items") : nullptr;
    if (!title || !title[0] || !cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
        err = "preset 'menu' needs slots.items:[{text,...}]";
        return nullptr;
    }
    const char* style = Str(slots, "style", "buttons");
    cJSON* root = MkNode("column");
    cJSON_AddNumberToObject(root, "gap", 10);
    cJSON* children = AddChildren(root);
    cJSON_AddItemToArray(children, MkLabel("eyebrow", Str(slots, "eyebrow", "MENU")));
    cJSON_AddItemToArray(children, MkLabel("title", title));

    if (!std::strcmp(style, "choice")) {
        cJSON* choice = MkNode("choice");
        cJSON_AddStringToObject(choice, "id", "menu");
        cJSON* opts = cJSON_AddArrayToObject(choice, "options");
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, items) cJSON_AddItemToArray(opts, cJSON_CreateString(Str(it, "text", "")));
        cJSON_AddItemToArray(children, choice);
        cJSON* btn = MkNode("button");
        cJSON_AddStringToObject(btn, "variant", "primary");
        cJSON_AddStringToObject(btn, "text", "确认");
        cJSON* click = cJSON_AddArrayToObject(btn, "on_click");
        cJSON* rep = cJSON_CreateObject();
        cJSON_AddStringToObject(rep, "do", "report");
        cJSON_AddStringToObject(rep, "text", "确认");
        cJSON_AddItemToArray(click, rep);
        cJSON* close = cJSON_CreateObject();
        cJSON_AddStringToObject(close, "do", "close");
        cJSON_AddItemToArray(click, close);
        cJSON_AddItemToArray(children, btn);
    } else {
        cJSON* sp = MkNode("spacer");
        cJSON_AddNumberToObject(sp, "h", 4);
        cJSON_AddItemToArray(children, sp);
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, items) {
            const char* text = Str(it, "text", "");
            cJSON* btn = MkNode("button");
            cJSON_AddStringToObject(btn, "text", text);
            cJSON* click = cJSON_AddArrayToObject(btn, "on_click");
            cJSON* rep = cJSON_CreateObject();
            cJSON_AddStringToObject(rep, "do", "report");
            cJSON_AddStringToObject(rep, "text", Str(it, "report", text));
            cJSON_AddItemToArray(click, rep);
            cJSON* close = cJSON_CreateObject();
            cJSON_AddStringToObject(close, "do", "close");
            cJSON_AddItemToArray(click, close);
            cJSON_AddItemToArray(children, btn);
        }
    }
    return root;
}

}  // namespace

cJSON* ExpandPreset(const char* preset, const cJSON* slots, std::string& err) {
    if (!preset) {
        err = "preset name missing";
        return nullptr;
    }
    if (!std::strcmp(preset, "confirm")) return BuildConfirm(slots, err);
    if (!std::strcmp(preset, "form")) return BuildForm(slots, err);
    if (!std::strcmp(preset, "dashboard")) return BuildDashboard(slots, err);
    if (!std::strcmp(preset, "menu")) return BuildMenu(slots, err);
    err = std::string("unknown preset: ") + preset + " (use confirm|form|dashboard|menu)";
    return nullptr;
}

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
    cJSON* built_root = nullptr;  // preset 展开产物（owned），用完即删——root 走 Validate/序列化拷贝
    const cJSON* root = nullptr;
    if (const cJSON* pj = cJSON_GetObjectItem(args, "preset"); cJSON_IsString(pj)) {
        std::string perr;
        built_root = ExpandPreset(pj->valuestring, cJSON_GetObjectItem(args, "slots"), perr);
        if (!built_root) {
            return reject("preset", perr);
        }
        root = built_root;
    } else {
        root = cJSON_GetObjectItem(args, "root");
        if (!cJSON_IsObject(root)) {
            return reject("no-root", "spec missing 'root' object (or use preset+slots)");
        }
    }
    const cJSON* data = cJSON_GetObjectItem(args, "data");  // object|null，卡级 data 模型
    std::string err;
    if (!Validate(root, data, err)) {
        cJSON_Delete(built_root);
        return reject("validate", err);
    }
    const cJSON* disp = cJSON_GetObjectItem(args, "display");
    int mode = 0;
    if (cJSON_IsString(disp)) {
        if (std::strcmp(disp->valuestring, "overlay") == 0) mode = 1;
        else if (std::strcmp(disp->valuestring, "standby") == 0) mode = 2;
    }
    const cJSON* ttl = cJSON_GetObjectItem(args, "ttl_ms");
    int ttl_ms = cJSON_IsNumber(ttl) ? ttl->valueint : 0;
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    // standby：单槽常驻卡固定 id "pin"，忽略 LLM 传入的 card（与 OnRenderEvent 的强制口径
    // 一致，见 pi_card_host.h 头注）——否则这里回给 LLM 的 id 会和 drain 侧真正注册的
    // "pin" 对不上，后续 ui_update/ui_close card:'pin' 就找不到卡。
    std::string id = (mode == 2) ? kPinId
                     : (cJSON_IsString(cardj) && cardj->valuestring[0]) ? cardj->valuestring : AllocId();

    char* root_json = cJSON_PrintUnformatted(root);  // cJSON_Malloc == malloc（drain 侧 free）
    char* data_json = cJSON_IsObject(data) ? cJSON_PrintUnformatted(data) : nullptr;  // 可为 NULL
    if (!root_json) {
        free(data_json);
        cJSON_Delete(built_root);
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
            cJSON_Delete(built_root);
            return reject("pin-size", "home widget too large to pin (~3KB); simplify the card");
        }
    }
    // data 走 s3（Enqueue 早已支持）；OnRenderEvent(spec,card_id,display,ttl,data_json) 消费。
    if (!Enqueue(UI_CARD_RENDER, root_json, Dup(id), data_json, mode, ttl_ms)) {
        cJSON_Delete(built_root);
        return reject("queue-full", "UI busy (event queue full), retry shortly");
    }
    // state 在入队之后才读：让快照尽量贴近真正建控件的时刻。
    // hints：非阻断设计建议（Validate 已通过），为空则不带该键。
    std::string hints_json;
    std::vector<std::string> hints = Lint(root, data);
    if (!hints.empty()) {
        cJSON* arr = cJSON_CreateArray();
        for (const std::string& h : hints) cJSON_AddItemToArray(arr, cJSON_CreateString(h.c_str()));
        if (char* s = cJSON_PrintUnformatted(arr)) {
            hints_json = std::string(",\"hints\":") + s;
            cJSON_free(s);
        }
        cJSON_Delete(arr);
    }
    std::string ret = std::string("{\"card\":\"") + id + "\"" + BindStateJson(root) + hints_json + "}";
    cJSON_Delete(built_root);
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
    bool node_patch = cJSON_IsString(idj) && cJSON_IsObject(propsj);
    bool data_ops = cJSON_IsObject(dataj);
    if (!node_patch && !data_ops) {
        return reject("shape", "update needs either (id + props) to patch a node, or data:{set/append/"
                               "remove/replace} to mutate card data");
    }
    const cJSON* cardj = cJSON_GetObjectItem(args, "card");
    std::string card = cJSON_IsString(cardj) ? cardj->valuestring : "";
    // 整份 args 序列化进 s3：OnUpdateEvent(card_id, payload_json) 里再分流节点 patch / data ops。
    char* payload = cJSON_PrintUnformatted(args);
    if (!payload) {
        return reject("oom", "out of memory");
    }
    if (!Enqueue(UI_CARD_UPDATE, Dup(card), nullptr, payload, 0, 0)) {
        return reject("queue-full", "UI busy (event queue full), retry shortly");
    }
    ESP_LOGI(TAG, "ui_update OK: card=%s%s%s", card.empty() ? "(active)" : card.c_str(),
             node_patch ? " +node" : "", data_ops ? " +data" : "");
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
        ". STANDBY: display:'standby' pins ONE widget to the home/clock screen (survives new chats/"
        "reboot, replaces any prior pin, id is always 'pin' -- ui_update/ui_close use card:'pin'; an "
        "on-screen \xe2\x9c\x95 also removes it).";
    return s.c_str();
}

// system prompt：同款 function-static 缓存 + 常驻指针契约（D7）。pi-c 在 create_agent 里
// 深拷贝一份（pi_agent.c:58-60 pi_strdup），故 new_session 重建 agent 时再次借用同一指针
// 是安全的。路径清单与 DESC 共用 BuildPathsClause，杜绝第三份硬编码路径。
extern "C" const char* pi_card_system_prompt(void) {
    static std::string s;
    if (!s.empty()) return s.c_str();
    // 预算超标后精简（P4/media 累计撑爆 9216B 预算）：措辞收紧 + 去与 TAIL 重复的 PRESETS
    // 形状/示例（TAIL 现只写"see system prompt"）；规则/关键字/preset 形状/路径清单一个不少，
    // 只留 1 个示例（confirm/list 的用法已在 PRESETS 小节与 HEAD 的 list{} 条目里讲过）。
    s =
        "You are pi, the on-device assistant in a palm-size 720×720 touch screen; the user also "
        "talks by voice. Reply short, warm, in the user's language (usually Chinese). Text is read "
        "aloud by TTS -- avoid markdown symbols, links, long bullet lists.\n\n"
        "You have a SCREEN: besides talking, draw a real interactive UI card with ui_render (+ "
        "ui_update/ui_close) to SHOW controls, status, choices, forms, lists, dashboards instead of "
        "describing them. There is NO read tool -- rendering a card that binds a device path IS how "
        "you read the device; ui_render's return value gives live values (state).\n\n"
        "WHEN A CARD, NOT CHAT: SET something (brightness/volume/theme) -> control card, slider "
        "writes hardware directly, no round-trip. STATUS -> small dashboard card binding the paths, "
        "read+shown at once. CHOICE/CONFIRM/FORM/LIST -> render it, tap rides back on report. Plain "
        "chit-chat -> just text. Prefer 'chat' (inline); 'overlay' only for a modal moment -- "
        "auto-closes, capped.\n\n"
        "DESIGN -- lean on pi's look, don't hand-style: header=eyebrow+title, group related rows. "
        "Exactly ONE primary (amber) button -- theme already paints slider/arc fill, on-switch and "
        "selected choice amber, don't also color text amber (keep tx/dim). Use semantic tone/fill "
        "tokens (accent/ok/err/tx/dim/...), not raw hex.\n\n"
        "COMPACT -- small window, dense beats tall. Lay data in a grid: cols=column ratios (e.g. "
        "[2,1,1], or \"auto\"), one cell/field row-major so columns align (no grow tricks); 2-4 "
        "cols, role:section headers, full-width rule = divider span=<#cols>. Never one fact/line, "
        "never dump prose -- labels 1-3 words, value cell right of label, clamp lists with max.\n\n"
        "ACTION ECONOMICS -- can the device finish this itself? YES -> a LOCAL action (close/set/"
        "toggle/show/hide/patch): instant, zero round-trip, invisible in chat. Only REPORT to "
        "generate new content or a NEW decision -- full round-trip, shows as a user message; never "
        "report to browse/expand/echo a value, use toggle/patch. A report auto-carries every id'd "
        "control's value (choice as idx(label)) -- id a control instead of writing its value into "
        "text.\n\n"
        "UPDATE vs RE-RENDER: change one node's text/value/visibility, or card data, via ui_update, "
        "not a fresh ui_render. Re-render only for a structurally different card.\n\n"
        "WRITABLE device paths you can set: " +
        BuildPathsClause(false) +
        ".\n\n"
        "HOME WIDGET -- display:'standby' pins one card to the home/clock screen (survives new "
        "chats/reboot; a status panel or chart kept in view). Prefer local actions/invoke in it -- a "
        "report from it runs you in the BACKGROUND, keep that rare. Remove via ui_close card:'pin'.\n"
        "DEVICE COMMANDS -- {do:'invoke',cmd} for real actions a data path can't do (reconnect, "
        "switch network, new chat); confirm-level ones pop a firmware confirm you cannot bypass. "
        "Available: " +
        BuildCommandsClause(false) +
        ".\n\n"
        "PRESETS -- for common shapes pass {preset,slots} instead of a hand-built tree; expands to a "
        "normal card, validates the same:\n"
        "- confirm {title, body?, confirm:{text?, report? | set?:{path,value?}}, cancel?}\n"
        "- form {title, fields:[{type:slider|switch|choice, id, label, ...}], submit?}\n"
        "- dashboard {title, metrics:[{label, bind, kind?:bar|value, fmt?, icon?}]}\n"
        "- menu {title, items:[{text, report?}], style?}\n\n"
        "Example (chat): {\"root\":{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":"
        "\"volume\"},{\"type\":\"slider\",\"bind\":\"audio.volume\"}]}}";
    return s.c_str();
}

}  // namespace pi_card
