#include "pi_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/bluetooth.h"
#include "metalio_hal/network.h"
#include "metalio_hal/power.h"
#include "pi_fonts.h"
#include "pi_net_events.h"
#include "pi_sleep.h"
#include "pi_sys_info.h"
#include "pi_theme.h"
#include "screen_util.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// 设置栈实现。视觉延续 pi_screen 的设计语言：纯色平面 + 1px 细线 + 唯一
// 琥珀强调；mono 字体子集只有 ASCII+·°，"‹ › ⚙ ⌁ ↻ ◌ ▲" 等一律用 ASCII
// 近似（"<" ">" "!" "^"）或小 lv_obj 形状代替；中英混排一律 puhui。
//
// 线程模型：mhal::network::OnEvent / mhal::bt::SetCallbacks 的回调都在各自
// 的后台任务线程触发，只写 s_mu 保护的快照 + dirty 标志；LVGL 侧由打开
// 期间常驻的 500ms tick 定时器轮询落地（与 pi_screen 的 ASR 轮询同一套
// 封送模式）。30s 无操作自动整栈退出也由该定时器检查
// lv_display_get_inactive_time()。
// ---------------------------------------------------------------------------
namespace {

// ----- palette：P2 起统一走 pi_theme 双主题令牌表（共享样式即时翻转） -------
using pi_theme::Tok;

constexpr int32_t kW = 720;
constexpr int32_t kH = 720;
constexpr int32_t kHeaderH = 56;
constexpr int32_t kHintH = 84;    // Hub 底部提示条
constexpr int32_t kHubRowH = 96;  // 触点 >= 96（6 行 + 5 条细线 ~ 581px，可微滚）
constexpr int32_t kSegH = 96;     // 分段按钮行高
constexpr int32_t kSwipePopThreshold = 80;
constexpr uint32_t kAutoCloseMs = 30 * 1000;
constexpr uint32_t kTickMs = 500;
constexpr uint32_t kSliderApplyGapMs = 150;  // 拖动中节流写入（同快捷面板）

pi_settings::Hooks s_hooks;

enum class PageId { Hub, Network, Bluetooth, Sound, Display, Chat, About };

struct PageEntry {
    PageId id;
    lv_obj_t* obj;
};

lv_obj_t* s_root = nullptr;  // 全屏容器（页栈 + 确认 sheet），删除即整栈关闭
std::vector<PageEntry> s_stack;
lv_timer_t* s_tick_timer = nullptr;
uint32_t s_ticks = 0;

// 右滑返回追踪（挂在 s_root 上；页面子树递归开 EVENT_BUBBLE）
bool s_swipe_tracking = false;
lv_point_t s_swipe_start = {0, 0};

// Hub 行值摘要（六行）与右上电量
lv_obj_t* s_hub_val[6] = {};
lv_obj_t* s_hub_batt = nullptr;

// 网络页
lv_obj_t* s_net_seg[2] = {};
lv_obj_t* s_net_cap = nullptr;
lv_obj_t* s_net_k[3] = {};
lv_obj_t* s_net_v[3] = {};
lv_obj_t* s_net_portal_state = nullptr;
lv_obj_t* s_net_portal_btn = nullptr;
lv_obj_t* s_net_confirm_root = nullptr;  // 切网确认 sheet（网络页存续期间常驻）
bool s_portal_started = false;           // 本次开机内已起配网热点（不可逆）

// 蓝牙页（两档：音箱 RX / 发射 TX。BT 模组硬件始终处于某个音频模式，
// 无"关闭"AT 指令，故不设关闭档——与官方 Claw4 固件的模式语义一致。）
lv_obj_t* s_bt_seg[2] = {};
lv_obj_t* s_bt_state_lbl = nullptr;
lv_obj_t* s_bt_dev_cap = nullptr;
lv_obj_t* s_bt_dev_list = nullptr;
std::vector<mhal::bt::Device> s_bt_devices;  // LVGL 线程侧的行数据
int s_bt_connecting_idx = -1;                // 点了"点按连接"的行
int s_bt_connected_idx = -1;
// 扫描需要模组已处于 Tx（AT+INQUIRING 前置条件）：刚点了发射档时先挂起，
// 等 on_mode_changed 确认 Tx 落地再发 StartScan（直接连发有竞态）。
bool s_bt_scan_on_tx = false;
// 进页瞬间是否已连接（扫描会把 ConnState 顶成 Scanning，行匹配缓存地址时
// 要用这份采样判断"该行就是当前连着的设备"）。
bool s_bt_entered_connected = false;
std::string s_bt_cached_addr;  // 进页时读一次 NVS，供行匹配

// 声音页
lv_obj_t* s_vol_slider = nullptr;
lv_obj_t* s_vol_val = nullptr;
lv_obj_t* s_tts_toggle = nullptr;
lv_obj_t* s_tts_knob = nullptr;
uint32_t s_vol_last_apply_ms = 0;

// 显示页
lv_obj_t* s_brt_slider = nullptr;
lv_obj_t* s_brt_val = nullptr;
lv_obj_t* s_theme_card[2] = {};
lv_obj_t* s_sleep_seg[4] = {};
uint32_t s_brt_last_apply_ms = 0;

// 对话页
lv_obj_t* s_mode_seg[2] = {};

// ----- 跨线程快照（回调线程写，tick 定时器读） ------------------------------
std::mutex s_mu;
struct NetSnapshot {
    bool dirty = false;
    bool portal = false;      // WifiConfigPortal 事件已到
    std::string portal_info;  // "ssid|url"
} s_net_evt;
struct BtSnapshot {
    bool dirty = false;
    mhal::bt::Mode mode = mhal::bt::Mode::None;
    mhal::bt::ConnState conn = mhal::bt::ConnState::Idle;
    std::vector<mhal::bt::Device> found;  // 增量：tick 取走即清
} s_bt_evt;
bool s_bt_cbs_registered = false;
// P1 起网络事件不再直连 mhal::network::OnEvent（覆盖式单回调，会与
// pi_screen 状态栏互相顶掉），改走 pi_net_events 分发层。
int s_net_listener_id = -1;

// ----- 小工具（与 pi_screen/pi_quick_panel 同款，匿名空间各自持有） ---------
lv_obj_t* MakeRect(lv_obj_t* parent, int32_t w, int32_t h, Tok color) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    pi_theme::ApplyBg(o, color);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

lv_obj_t* MakeCircle(lv_obj_t* parent, int32_t d, Tok color) {
    lv_obj_t* o = MakeRect(parent, d, d, color);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    return o;
}

void SetLabelFont(lv_obj_t* label, const lv_font_t* font, Tok color) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    pi_theme::ApplyText(label, color);
}

lv_obj_t* MakeFlexRow(lv_obj_t* parent, int32_t h) {
    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN);
    return row;
}

// 卡片：Card 底 + 1px Line 边 + 圆角 12 + mono cap 行
lv_obj_t* MakeCard(lv_obj_t* parent, const char* cap_ascii, lv_obj_t** out_cap = nullptr) {
    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(card, Tok::Line);
    pi_theme::ApplyBg(card, Tok::Card);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 14, LV_PART_MAIN);

    if (cap_ascii != nullptr) {
        lv_obj_t* cap = lv_label_create(card);
        lv_label_set_text(cap, cap_ascii);
        SetLabelFont(cap, &font_pi_mono_17, Tok::Faint);
        lv_obj_set_style_text_letter_space(cap, 2, LV_PART_MAIN);
        if (out_cap != nullptr)
            *out_cap = cap;
    }
    return card;
}

// kv 行：左 key（puhui, dim, 固定宽）+ 右 value（右对齐）。返回 value 标签。
lv_obj_t* MakeKvRow(lv_obj_t* parent, const char* key_utf8, lv_obj_t** out_key = nullptr) {
    lv_obj_t* row = MakeFlexRow(parent, 40);
    lv_obj_t* k = lv_label_create(row);
    lv_label_set_text(k, key_utf8);
    SetLabelFont(k, &font_puhui_20_4, Tok::Dim);
    lv_obj_set_width(k, 150);
    if (out_key != nullptr)
        *out_key = k;
    lv_obj_t* v = lv_label_create(row);
    lv_label_set_text(v, "--");
    SetLabelFont(v, &font_puhui_20_4, Tok::Tx);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return v;
}

// 分段按钮：等宽一行，选中 accent 描边 + accent 字 + Card2 底
void SegSetSelected(lv_obj_t* const* btns, int n, int selected) {
    for (int i = 0; i < n; i++) {
        if (btns[i] == nullptr)
            continue;
        bool sel = (i == selected);
        pi_theme::ApplyBorder(btns[i], sel ? Tok::Accent : Tok::Line);
        pi_theme::ApplyBg(btns[i], sel ? Tok::Card2 : Tok::Card);
        lv_obj_t* lbl = lv_obj_get_child(btns[i], 0);
        if (lbl != nullptr)
            pi_theme::ApplyText(lbl, sel ? Tok::Accent : Tok::Dim);
    }
}

lv_obj_t* MakeSegBtn(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_event_cb_t cb,
                     intptr_t idx) {
    lv_obj_t* btn = lv_obj_create(parent);
    screen_strip_obj_chrome(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(btn, Tok::Line);
    pi_theme::ApplyBg(btn, Tok::Card);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    SetLabelFont(lbl, font, Tok::Dim);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(idx));
    return btn;
}

// 琥珀描边动作按钮（整宽，h=96）
lv_obj_t* MakeActionBtn(lv_obj_t* parent, const char* text_utf8, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    screen_strip_obj_chrome(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, LV_PCT(100), 96);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(btn, Tok::AccentDim);
    pi_theme::ApplyBg(btn, Tok::Card);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    pi_theme::ApplyBg(btn, Tok::Card2, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text_utf8);
    SetLabelFont(lbl, &font_puhui_24_4, Tok::Accent);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (cb != nullptr)
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

// 大滑条行（h=88，拖动即时生效；样式与快捷面板一致）
lv_obj_t* MakeBigSliderRow(lv_obj_t* parent, int32_t min, int32_t max, lv_obj_t** out_slider,
                           lv_obj_t** out_val) {
    lv_obj_t* row = MakeFlexRow(parent, 88);
    lv_obj_set_style_pad_column(row, 24, LV_PART_MAIN);

    lv_obj_t* slider = lv_slider_create(row);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_height(slider, 8);
    lv_obj_set_flex_grow(slider, 1);
    pi_theme::ApplyBg(slider, Tok::Card2);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
    pi_theme::ApplyBg(slider, Tok::Accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    pi_theme::ApplyBg(slider, Tok::Accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 16, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 24);
    screen_swipe_back_ignore(slider, true);  // 滑条拥有横向拖拽语义（也豁免右滑返回）
    *out_slider = slider;

    lv_obj_t* val = lv_label_create(row);
    lv_label_set_text(val, "--");
    SetLabelFont(val, &font_pi_mono_20, Tok::Tx);
    lv_obj_set_width(val, 72);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    *out_val = val;
    return row;
}

void EnableEventBubbleRecursive(lv_obj_t* obj) {
    if (obj == nullptr)
        return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i)
        EnableEventBubbleRecursive(lv_obj_get_child(obj, i));
}

// ----- 蓝牙最近连接缓存（NVS ns "bt"：last_name / last_addr） ---------------
// BT 模组协议拿不到"当前已连接设备"（CONNECT SUCCESS 不带地址，也无查询
// AT）——只能在 UI 侧记住"最近一次连接成功的设备"。展示语义：ConnState 为
// Connected 且缓存存在时显示缓存的设备名（仅名称，不声称"当前连接的就是
// 它"）；Disconnected 时只显模式。
// 12 位 hex 地址缩略成 "AB12..EF34"
std::string AbbrevAddr(const std::string& addr) {
    if (addr.size() <= 10)
        return addr;
    return addr.substr(0, 4) + ".." + addr.substr(addr.size() - 4);
}

// 截到 UTF-8 码点边界（设备名可能中英混排，硬截会产生半个多字节序列）
std::string ClampUtf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes)
        return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        n--;
    return s.substr(0, n) + "..";
}

void BtSaveLastConnected(const mhal::bt::Device& dev) {
    Settings s("bt", true);
    s.SetString("last_addr", dev.addr_hex);
    s.SetString("last_name", dev.name);
}

std::string BtCachedAddr() {
    Settings s("bt", false);
    return s.GetString("last_addr");
}

// 缓存设备的显示名（name 为空退化为地址缩略；无缓存返回 ""）
std::string BtCachedDisplayName() {
    Settings s("bt", false);
    std::string name = s.GetString("last_name");
    if (!name.empty())
        return ClampUtf8(name, 24);
    std::string addr = s.GetString("last_addr");
    return addr.empty() ? "" : AbbrevAddr(addr);
}

// ----- 前置声明 --------------------------------------------------------------
void Push(PageId id);
void Pop();
void CloseAll();
void RefreshHub();
void RefreshNetworkPage();
void PageWillClose(PageId id);

// ---------------------------------------------------------------------------
// 页面骨架：56px 状态栏（< + 标题 + 右侧电量%）+ 可滚内容列
// ---------------------------------------------------------------------------
// Pop 会 lv_obj_delete 当前页对象；直接在事件回调里删会让 LVGL 后续对原
// target 的事件派发（RELEASED 之后还有 CLICKED、以及沿子树的冒泡）踩到已
// 释放对象 —— sim 交互测试实测 segfault。统一经 lv_async_call 延迟到本轮
// 事件处理结束后出栈。
bool s_pop_pending = false;

void PopAsync(void*) {
    s_pop_pending = false;
    Pop();
}

void RequestPop() {
    if (s_pop_pending || s_root == nullptr)
        return;
    s_pop_pending = true;
    lv_async_call(PopAsync, nullptr);
}

void OnBackClicked(lv_event_t*) { RequestPop(); }

void UpdateBattLabel(lv_obj_t* lbl) {
    if (lbl == nullptr)
        return;
    int level = 0;
    bool charging = false, discharging = false;
    char buf[16];
    if (mhal::power::GetBatteryLevel(level, charging, discharging)) {
        std::snprintf(buf, sizeof(buf), "%d%%", level);
    } else {
        std::snprintf(buf, sizeof(buf), "--%%");
    }
    lv_label_set_text(lbl, buf);
}

// 返回内容列容器（header 之下、整页余下区域，纵向可滚）。
lv_obj_t* MakePage(PageId id, const char* title_utf8, lv_obj_t** out_page,
                   int32_t bottom_reserved = 0) {
    lv_obj_t* page = lv_obj_create(s_root);
    screen_strip_obj_chrome(page);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);  // 承接空白区按压（右滑返回追踪）
    lv_obj_set_size(page, kW, kH);
    lv_obj_set_pos(page, 0, 0);
    pi_theme::ApplyBg(page, Tok::Bg);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    (void)id;

    // header
    lv_obj_t* hdr = lv_obj_create(page);
    screen_strip_obj_chrome(hdr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hdr, kW, kHeaderH);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_right(hdr, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_column(hdr, 8, LV_PART_MAIN);

    // 「<」返回：96 宽全高触区（"‹" 不在 mono 子集）
    lv_obj_t* back = lv_obj_create(hdr);
    screen_strip_obj_chrome(back);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(back, 96, kHeaderH);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "<");
    SetLabelFont(back_lbl, &font_pi_mono_20, Tok::Dim);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, title_utf8);
    SetLabelFont(title, &font_puhui_24_4, Tok::Tx);

    lv_obj_t* sp = lv_obj_create(hdr);
    screen_strip_obj_chrome(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t* batt = lv_label_create(hdr);
    SetLabelFont(batt, &font_pi_mono_17, Tok::Dim);
    UpdateBattLabel(batt);
    if (id == PageId::Hub)
        s_hub_batt = batt;

    lv_obj_t* rule = MakeRect(page, kW, 1, Tok::Line);
    lv_obj_set_pos(rule, 0, kHeaderH - 1);

    // content column
    lv_obj_t* content = lv_obj_create(page);
    screen_strip_obj_chrome(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(content, kW, kH - kHeaderH - bottom_reserved);
    lv_obj_set_pos(content, 0, kHeaderH);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(content, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 20, LV_PART_MAIN);

    *out_page = page;
    return content;
}

// ---------------------------------------------------------------------------
// Hub 页
// ---------------------------------------------------------------------------
void OnHubRowClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    Push(static_cast<PageId>(static_cast<int>(PageId::Network) + static_cast<int>(idx)));
}

// 行：52px 方框 mono 图标 + 中文标题 + 右侧 mono 值摘要 + ">"
void MakeHubRow(lv_obj_t* parent, int idx, const char* icon_ascii, const char* title_utf8) {
    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), kHubRowH);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    pi_theme::ApplyBg(row, Tok::Card, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 20, LV_PART_MAIN);
    lv_obj_add_event_cb(row, OnHubRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

    lv_obj_t* icon_box = lv_obj_create(row);
    screen_strip_obj_chrome(icon_box);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(icon_box, 52, 52);
    lv_obj_set_style_radius(icon_box, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_box, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(icon_box, Tok::Line2);
    pi_theme::ApplyBg(icon_box, Tok::Card);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(icon_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* icon = lv_label_create(icon_box);
    lv_label_set_text(icon, icon_ascii);
    SetLabelFont(icon, &font_pi_mono_17, Tok::Accent);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, title_utf8);
    SetLabelFont(title, &font_puhui_24_4, Tok::Tx);

    lv_obj_t* sp = lv_obj_create(row);
    screen_strip_obj_chrome(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t* val = lv_label_create(row);
    lv_label_set_text(val, "--");
    // 值摘要可能中英混排（"已连接"/"深色"），mono 子集放不下 -> puhui
    SetLabelFont(val, &font_puhui_20_4, Tok::Dim);
    s_hub_val[idx] = val;

    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, ">");
    SetLabelFont(arrow, &font_pi_mono_17, Tok::Faint);
}

void BuildHubPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::Hub, "\xe8\xae\xbe\xe7\xbd\xae", out_page, kHintH);
    // Hub 行区不要左右 pad（行自带 28），行间 1px 细线
    lv_obj_set_style_pad_hor(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 0, LV_PART_MAIN);

    static const char* kIcons[6] = {"N", "B", "S", "D", "C", "i"};
    static const char* kTitles[6] = {
        "\xe7\xbd\x91\xe7\xbb\x9c",  // 网络
        "\xe8\x93\x9d\xe7\x89\x99",  // 蓝牙
        "\xe5\xa3\xb0\xe9\x9f\xb3",  // 声音
        "\xe6\x98\xbe\xe7\xa4\xba",  // 显示
        "\xe5\xaf\xb9\xe8\xaf\x9d",  // 对话
        "\xe5\x85\xb3\xe4\xba\x8e",  // 关于
    };
    for (int i = 0; i < 6; i++) {
        MakeHubRow(content, i, kIcons[i], kTitles[i]);
        if (i < 5)
            MakeRect(content, kW, 1, Tok::Line);
    }

    // 底部 84px 提示条
    lv_obj_t* page = *out_page;
    lv_obj_t* hrule = MakeRect(page, kW, 1, Tok::Line);
    lv_obj_set_pos(hrule, 0, kH - kHintH);
    lv_obj_t* hint = lv_obj_create(page);
    screen_strip_obj_chrome(hint);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hint, kW, kHintH);
    lv_obj_set_pos(hint, 0, kH - kHintH);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint, 16, LV_PART_MAIN);
    lv_obj_t* h1 = lv_label_create(hint);
    lv_label_set_text(h1, "\xe5\x8f\xb3\xe6\xbb\x91\xe8\xbf\x94\xe5\x9b\x9e");  // "右滑返回"
    SetLabelFont(h1, &font_puhui_20_4, Tok::Faint);
    lv_obj_t* h2 = lv_label_create(hint);
    lv_label_set_text(h2, "SWIPE > BACK \xc2\xb7 30S AUTO");
    SetLabelFont(h2, &font_pi_mono_14, Tok::Faint);
    lv_obj_set_style_text_letter_space(h2, 2, LV_PART_MAIN);

    RefreshHub();
}

// Hub 六行值摘要（也刷新右上电量）
void RefreshHub() {
    if (s_hub_val[0] == nullptr)
        return;
    char buf[96];

    // 网络
    bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
    bool up = mhal::network::IsConnected();
    if (wifi) {
        std::snprintf(
            buf, sizeof(buf), "WiFi \xc2\xb7 %s",
            up ? "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5" : "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5");
    } else {
        int csq = mhal::network::GetSignalStrength();
        if (up && csq >= 0 && csq <= 31) {
            std::snprintf(buf, sizeof(buf), "4G \xc2\xb7 %ddBm", -113 + 2 * csq);
        } else {
            std::snprintf(buf, sizeof(buf), "4G \xc2\xb7 %s",
                          up ? "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5"
                             : "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5");
        }
    }
    lv_label_set_text(s_hub_val[0], buf);

    // 蓝牙
    {
        mhal::bt::Mode m = mhal::bt::GetMode();
        const char* mode_txt =
            m == mhal::bt::Mode::Tx
                ? "TX"
                : (m == mhal::bt::Mode::None ? "\xe5\x85\xb3\xe9\x97\xad" : "RX");  // "关闭"
        if (mhal::bt::GetConnState() == mhal::bt::ConnState::Connected) {
            // 已连接：优先显示缓存的最近连接设备名（拿不到地址查询，名称
            // 即最佳近似）；无缓存退化为"已连接"。
            std::string name = BtCachedDisplayName();
            if (!name.empty()) {
                std::snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s", mode_txt, name.c_str());
            } else {
                std::snprintf(buf, sizeof(buf), "%s \xc2\xb7 \xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5",
                              mode_txt);  // "· 已连接"
            }
        } else {
            std::snprintf(buf, sizeof(buf), "%s", mode_txt);
        }
        lv_label_set_text(s_hub_val[1], buf);
    }

    // 声音
    bool tts_on = s_hooks.get_tts != nullptr && s_hooks.get_tts();
    std::snprintf(buf, sizeof(buf),
                  "\xe9\x9f\xb3\xe9\x87\x8f %d \xc2\xb7 TTS %s",  // "音量 N · TTS"
                  mhal::audio::GetVolume(),
                  tts_on ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  // 开/关
    lv_label_set_text(s_hub_val[2], buf);

    // 显示（P1 起息屏档位接真：NVS "ui"/"sleep_s"）
    {
        Settings ui("ui", false);
        int32_t sleep_s = ui.GetInt("sleep_s", 0);
        char sleep_txt[16];
        if (sleep_s <= 0) {
            std::snprintf(sleep_txt, sizeof(sleep_txt), "\xe6\xb0\xb8\xe4\xb8\x8d");  // "永不"
        } else if (sleep_s < 60) {
            std::snprintf(sleep_txt, sizeof(sleep_txt), "%ds", static_cast<int>(sleep_s));
        } else {
            std::snprintf(sleep_txt, sizeof(sleep_txt), "%dmin", static_cast<int>(sleep_s / 60));
        }
        std::snprintf(buf, sizeof(buf),
                      "\xe4\xba\xae\xe5\xba\xa6 %d \xc2\xb7 %s \xc2\xb7 "
                      "\xe6\x81\xaf\xe5\xb1\x8f %s",  // "亮度 N · 深色/浅色 · 息屏 X"
                      static_cast<int>(mhal::backlight::GetBrightness()),
                      pi_theme::IsLight() ? "\xe6\xb5\x85\xe8\x89\xb2"   // "浅色"
                                          : "\xe6\xb7\xb1\xe8\x89\xb2",  // "深色"
                      sleep_txt);
    }
    lv_label_set_text(s_hub_val[3], buf);

    // 对话
    bool zen = s_hooks.get_zen != nullptr && s_hooks.get_zen();
    std::snprintf(buf, sizeof(buf), "%s%s", zen ? "ZEN" : "FLOW", tts_on ? " \xc2\xb7 TTS" : "");
    lv_label_set_text(s_hub_val[4], buf);

    // 关于（"▲" 不在字体子集 -> "^"）
    {
        int level = 0;
        bool chg = false, dis = false;
        bool have = mhal::power::GetBatteryLevel(level, chg, dis);
        if (have) {
            std::snprintf(buf, sizeof(buf), "claw6 v%s \xc2\xb7 ^%d%%", pi_sys_fw_version(), level);
        } else {
            std::snprintf(buf, sizeof(buf), "claw6 v%s", pi_sys_fw_version());
        }
        lv_label_set_text(s_hub_val[5], buf);
    }

    UpdateBattLabel(s_hub_batt);
}

// ---------------------------------------------------------------------------
// 网络页
// ---------------------------------------------------------------------------
void CloseNetConfirm() {
    if (s_net_confirm_root != nullptr)
        lv_obj_add_flag(s_net_confirm_root, LV_OBJ_FLAG_HIDDEN);
}

void OnNetConfirmSwitch(lv_event_t*) {
    CloseNetConfirm();
    // 持久化另一类型并 esp_restart（不返回）。sim shim 打日志后退出进程。
    mhal::network::SwitchType();
}

// 切网确认 sheet（底部弹层，样式同 pi_screen 新对话 sheet）
void BuildNetConfirmSheet() {
    s_net_confirm_root = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_net_confirm_root);
    lv_obj_remove_flag(s_net_confirm_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_net_confirm_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_net_confirm_root, kW, kH);
    lv_obj_set_pos(s_net_confirm_root, 0, 0);
    lv_obj_set_style_bg_opa(s_net_confirm_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_net_confirm_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scrim = lv_obj_create(s_net_confirm_root);
    screen_strip_obj_chrome(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scrim, kW, kH);
    pi_theme::ApplyScrim(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, [](lv_event_t*) { CloseNetConfirm(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sheet = lv_obj_create(s_net_confirm_root);
    screen_strip_obj_chrome(sheet);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sheet, kW);
    lv_obj_set_height(sheet, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(sheet, 24, LV_PART_MAIN);
    pi_theme::ApplyBg(sheet, Tok::Card);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sheet, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(sheet, Tok::Line);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);  // 挡住透传到 scrim 的点击
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(sheet, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_top(sheet, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(sheet, 28 + 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sheet, 14, LV_PART_MAIN);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 24);  // 底部圆角藏出屏外

    lv_obj_t* title = lv_label_create(sheet);
    // "切换网络通道？"
    lv_label_set_text(title,
                      "\xe5\x88\x87\xe6\x8d\xa2\xe7\xbd\x91\xe7\xbb\x9c\xe9\x80\x9a\xe9\x81\x93"
                      "\xef\xbc\x9f");
    SetLabelFont(title, &font_puhui_30_4, Tok::Tx);

    lv_obj_t* note = lv_label_create(sheet);
    // "切换网络通道将重启设备"
    lv_label_set_text(note,
                      "\xe5\x88\x87\xe6\x8d\xa2\xe7\xbd\x91\xe7\xbb\x9c\xe9\x80\x9a\xe9\x81\x93"
                      "\xe5\xb0\x86\xe9\x87\x8d\xe5\x90\xaf\xe8\xae\xbe\xe5\xa4\x87");
    SetLabelFont(note, &font_puhui_20_4, Tok::Faint);

    lv_obj_t* btn_row = lv_obj_create(sheet);
    screen_strip_obj_chrome(btn_row);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn_row, LV_PCT(100), 96 + 10);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 20, LV_PART_MAIN);

    auto make_btn = [](lv_obj_t* parent, Tok border, const char* text, Tok color) {
        lv_obj_t* b = lv_obj_create(parent);
        screen_strip_obj_chrome(b);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(b, 96);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        pi_theme::ApplyBorder(b, border);
        pi_theme::ApplyBg(b, Tok::Card);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        pi_theme::ApplyBg(b, Tok::Card2, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, text);
        SetLabelFont(lbl, &font_puhui_24_4, color);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return b;
    };
    lv_obj_t* cancel =
        make_btn(btn_row, Tok::Line2, "\xe5\x8f\x96\xe6\xb6\x88", Tok::Dim);  // "取消"
    lv_obj_add_event_cb(cancel, [](lv_event_t*) { CloseNetConfirm(); }, LV_EVENT_CLICKED, nullptr);
    // "切换并重启"
    lv_obj_t* confirm =
        make_btn(btn_row, Tok::Accent,
                 "\xe5\x88\x87\xe6\x8d\xa2\xe5\xb9\xb6\xe9\x87\x8d\xe5\x90\xaf", Tok::Accent);
    lv_obj_add_event_cb(confirm, OnNetConfirmSwitch, LV_EVENT_CLICKED, nullptr);
}

void OnNetSegClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    bool wifi_now = mhal::network::GetType() == mhal::network::Type::WiFi;
    int cur = wifi_now ? 0 : 1;
    if (static_cast<int>(idx) == cur)
        return;  // 点当前侧无动作
    if (s_net_confirm_root != nullptr)
        lv_obj_remove_flag(s_net_confirm_root, LV_OBJ_FLAG_HIDDEN);
}

void SetPortalActiveVisual(const char* info) {
    if (s_net_portal_state == nullptr)
        return;
    char buf[128];
    if (info != nullptr && info[0] != '\0') {
        // "配网中 · <ssid|url>"
        std::snprintf(buf, sizeof(buf), "\xe9\x85\x8d\xe7\xbd\x91\xe4\xb8\xad \xc2\xb7 %s", info);
    } else {
        // "配网中 (热点已开启)"
        std::snprintf(buf, sizeof(buf),
                      "\xe9\x85\x8d\xe7\xbd\x91\xe4\xb8\xad (\xe7\x83\xad\xe7\x82\xb9\xe5\xb7\xb2"
                      "\xe5\xbc\x80\xe5\x90\xaf)");
    }
    lv_label_set_text(s_net_portal_state, buf);
    lv_obj_remove_flag(s_net_portal_state, LV_OBJ_FLAG_HIDDEN);
    if (s_net_portal_btn != nullptr) {
        lv_obj_remove_flag(s_net_portal_btn, LV_OBJ_FLAG_CLICKABLE);
        pi_theme::ApplyBorder(s_net_portal_btn, Tok::Line);
        lv_obj_t* lbl = lv_obj_get_child(s_net_portal_btn, 0);
        if (lbl != nullptr)
            pi_theme::ApplyText(lbl, Tok::Faint);
    }
}

void OnPortalClicked(lv_event_t*) {
    if (s_portal_started)
        return;
    s_portal_started = true;
    // StartConfigPortal 阻塞不返回（softAP + 网页配网，提交后设备重启）——
    // 丢进专用任务，UI 立即切"配网中"态；热点名经 WifiConfigPortal 事件回填。
    xTaskCreate(
        [](void*) {
            mhal::network::StartConfigPortal();
            vTaskDelete(nullptr);  // 正常不会到达
        },
        "cfg_portal", 6144, nullptr, 4, nullptr);
    SetPortalActiveVisual(nullptr);
}

void BuildNetworkPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::Network, "\xe7\xbd\x91\xe7\xbb\x9c", out_page);  // "网络"

    // WiFi / 4G 分段
    lv_obj_t* seg_row = MakeFlexRow(content, kSegH);
    s_net_seg[0] = MakeSegBtn(seg_row, "WiFi", &font_pi_mono_20, OnNetSegClicked, 0);
    s_net_seg[1] = MakeSegBtn(seg_row, "4G", &font_pi_mono_20, OnNetSegClicked, 1);

    // 警示行「! 切换网络通道将重启设备」
    lv_obj_t* warn = MakeFlexRow(content, 32);
    lv_obj_set_style_pad_column(warn, 12, LV_PART_MAIN);
    lv_obj_t* bang = lv_label_create(warn);
    lv_label_set_text(bang, "!");
    SetLabelFont(bang, &font_pi_mono_17, Tok::Accent);
    lv_obj_t* warn_lbl = lv_label_create(warn);
    lv_label_set_text(warn_lbl,
                      "\xe5\x88\x87\xe6\x8d\xa2\xe7\xbd\x91\xe7\xbb\x9c\xe9\x80\x9a\xe9\x81\x93"
                      "\xe5\xb0\x86\xe9\x87\x8d\xe5\x90\xaf\xe8\xae\xbe\xe5\xa4\x87");
    SetLabelFont(warn_lbl, &font_puhui_20_4, Tok::Faint);

    // 状态卡片（cap 与 kv 行随类型在 RefreshNetworkPage 填充）
    lv_obj_t* card = MakeCard(content, "STATUS", &s_net_cap);
    s_net_v[0] = MakeKvRow(card, "--", &s_net_k[0]);
    s_net_v[1] = MakeKvRow(card, "--", &s_net_k[1]);
    s_net_v[2] = MakeKvRow(card, "IP", &s_net_k[2]);

    // 配网卡片
    lv_obj_t* portal = MakeCard(content, "PORTAL");
    lv_obj_t* hint = lv_label_create(portal);
    // "手机连接配网热点完成配网"
    lv_label_set_text(hint,
                      "\xe6\x89\x8b\xe6\x9c\xba\xe8\xbf\x9e\xe6\x8e\xa5\xe9\x85\x8d\xe7\xbd\x91"
                      "\xe7\x83\xad\xe7\x82\xb9\xe5\xae\x8c\xe6\x88\x90\xe9\x85\x8d\xe7\xbd\x91");
    SetLabelFont(hint, &font_puhui_20_4, Tok::Dim);
    s_net_portal_state = lv_label_create(portal);
    lv_label_set_text(s_net_portal_state, "");
    SetLabelFont(s_net_portal_state, &font_puhui_20_4, Tok::Accent);
    lv_obj_add_flag(s_net_portal_state, LV_OBJ_FLAG_HIDDEN);
    // "开始配网"（"⌁" 不在字体子集，省略图标）
    s_net_portal_btn =
        MakeActionBtn(portal, "\xe5\xbc\x80\xe5\xa7\x8b\xe9\x85\x8d\xe7\xbd\x91", OnPortalClicked);

    BuildNetConfirmSheet();
    if (s_portal_started)
        SetPortalActiveVisual(nullptr);
    RefreshNetworkPage();
}

void RefreshNetworkPage() {
    if (s_net_cap == nullptr)
        return;
    bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
    bool up = mhal::network::IsConnected();
    SegSetSelected(s_net_seg, 2, wifi ? 0 : 1);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s", wifi ? "WIFI" : "4G",
                  up ? "CONNECTED" : "OFFLINE");
    lv_label_set_text(s_net_cap, buf);
    pi_theme::ApplyText(s_net_cap, up ? Tok::Ok : Tok::Faint);

    if (wifi) {
        lv_label_set_text(s_net_k[0], "SSID");
        std::string ssid = mhal::network::GetWifiSsid();
        if (!ssid.empty()) {
            lv_label_set_text(s_net_v[0], ssid.c_str());
        } else {
            // 拿不到 SSID 时退化为连接状态文案（"已连接/未连接"）
            lv_label_set_text(s_net_v[0], up ? "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5"
                                             : "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5");
        }
        lv_label_set_text(s_net_k[1], "\xe4\xbf\xa1\xe5\x8f\xb7");  // "信号"
        int rssi = mhal::network::GetWifiRssi();
        if (rssi != 0) {
            std::snprintf(buf, sizeof(buf), "%ddBm", rssi);
            lv_label_set_text(s_net_v[1], buf);
        } else {
            lv_label_set_text(s_net_v[1], "--");
        }
    } else {
        lv_label_set_text(s_net_k[0], "\xe4\xbf\xa1\xe5\x8f\xb7");  // "信号"
        int csq = mhal::network::GetSignalStrength();
        if (csq >= 0 && csq <= 31) {
            std::snprintf(buf, sizeof(buf), "%ddBm (CSQ %d)", -113 + 2 * csq, csq);
            lv_label_set_text(s_net_v[0], buf);
        } else {
            lv_label_set_text(s_net_v[0], "--");
        }
        lv_label_set_text(s_net_k[1], "\xe6\xb3\xa8\xe5\x86\x8c");  // "注册"
        // GetRegistrationStateJson: {"stat":1,...}，stat 1/5 = 已注册
        std::string reg = mhal::network::GetRegistrationStateJson();
        const char* p = std::strstr(reg.c_str(), "\"stat\":");
        int stat = (p != nullptr) ? std::atoi(p + 7) : -1;
        lv_label_set_text(s_net_v[1], (stat == 1 || stat == 5)
                                          ? "\xe5\xb7\xb2\xe6\xb3\xa8\xe5\x86\x8c"    // "已注册"
                                          : "\xe6\x9c\xaa\xe6\xb3\xa8\xe5\x86\x8c");  // "未注册"
    }

    std::string ip = mhal::network::GetIpAddress();
    lv_label_set_text(s_net_v[2], ip.empty() ? "--" : ip.c_str());
}

// ---------------------------------------------------------------------------
// 蓝牙页
// ---------------------------------------------------------------------------
void SetBtStateLabel(mhal::bt::ConnState st) {
    if (s_bt_state_lbl == nullptr)
        return;
    switch (st) {
        case mhal::bt::ConnState::Connected: {
            // 优先显示缓存的最近连接设备名（可能中英混排 -> puhui）
            std::string name = BtCachedDisplayName();
            if (!name.empty()) {
                lv_label_set_text(s_bt_state_lbl, name.c_str());
                SetLabelFont(s_bt_state_lbl, &font_puhui_20_4, Tok::Ok);
            } else {
                lv_label_set_text(s_bt_state_lbl, "CONNECTED");
                SetLabelFont(s_bt_state_lbl, &font_pi_mono_17, Tok::Ok);
            }
            break;
        }
        case mhal::bt::ConnState::Connecting:
            lv_label_set_text(s_bt_state_lbl, "CONNECTING");
            SetLabelFont(s_bt_state_lbl, &font_pi_mono_17, Tok::Accent);
            break;
        case mhal::bt::ConnState::Scanning:
            lv_label_set_text(s_bt_state_lbl, "SCANNING");
            SetLabelFont(s_bt_state_lbl, &font_pi_mono_17, Tok::Accent);
            break;
        default:
            lv_label_set_text(s_bt_state_lbl, "IDLE");
            SetLabelFont(s_bt_state_lbl, &font_pi_mono_17, Tok::Faint);
            break;
    }
    if (s_bt_dev_cap != nullptr) {
        lv_label_set_text(s_bt_dev_cap, st == mhal::bt::ConnState::Scanning
                                            ? "DEVICES \xc2\xb7 SCANNING"
                                            : "DEVICES");
    }
}

int BtSegIndexOf(mhal::bt::Mode m) {
    switch (m) {
        case mhal::bt::Mode::Tx:
            return 1;
        default:
            return 0;  // Rx / MusicRx / None 都归"音箱"档
    }
}

// 设备行右侧动作标签的三种状态
void SetBtRowAction(lv_obj_t* row, int state /*0=可连 1=连接中 2=已连接*/) {
    lv_obj_t* action = lv_obj_get_child(row, static_cast<int32_t>(lv_obj_get_child_count(row)) - 1);
    if (action == nullptr)
        return;
    if (state == 2) {
        lv_label_set_text(action, "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5");  // "已连接"
        pi_theme::ApplyText(action, Tok::Ok);
    } else if (state == 1) {
        lv_label_set_text(action, "...");
        pi_theme::ApplyText(action, Tok::Accent);
    } else {
        lv_label_set_text(action,
                          "\xe7\x82\xb9\xe6\x8c\x89\xe8\xbf\x9e\xe6\x8e\xa5");  // "点按连接"
        pi_theme::ApplyText(action, Tok::Accent);
    }
}

void OnBtDeviceRowClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (idx < 0 || idx >= static_cast<intptr_t>(s_bt_devices.size()))
        return;
    if (idx == s_bt_connected_idx)
        return;  // 已连接的行不重复连
    s_bt_connecting_idx = static_cast<int>(idx);
    mhal::bt::Connect(s_bt_devices[idx].addr_hex);
    lv_obj_t* row = lv_obj_get_child(s_bt_dev_list, static_cast<int32_t>(idx));
    if (row != nullptr)
        SetBtRowAction(row, 1);
    SetBtStateLabel(mhal::bt::ConnState::Connecting);
}

void AppendBtDeviceRow(const mhal::bt::Device& dev) {
    // 去重（同地址只保留首次上报）
    for (const auto& d : s_bt_devices) {
        if (d.addr_hex == dev.addr_hex)
            return;
    }
    s_bt_devices.push_back(dev);
    int idx = static_cast<int>(s_bt_devices.size()) - 1;

    lv_obj_t* row = lv_obj_create(s_bt_dev_list);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    pi_theme::ApplyBg(row, Tok::Card2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN);
    lv_obj_add_event_cb(row, OnBtDeviceRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
    EnableEventBubbleRecursive(row);  // 新行也要参与右滑返回追踪

    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, dev.name.empty() ? "(unknown)" : dev.name.c_str());
    SetLabelFont(name, &font_puhui_20_4, Tok::Tx);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 300);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* addr = lv_label_create(row);
    lv_label_set_text(addr, AbbrevAddr(dev.addr_hex).c_str());
    SetLabelFont(addr, &font_pi_mono_14, Tok::Faint);
    lv_obj_set_flex_grow(addr, 1);
    lv_obj_remove_flag(addr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* action = lv_label_create(row);  // 必须是最后一个子件（SetBtRowAction 约定）
    SetLabelFont(action, &font_puhui_20_4, Tok::Accent);
    lv_obj_remove_flag(action, LV_OBJ_FLAG_CLICKABLE);
    // 进页时已连接且该行地址命中缓存 -> 直接标"已连接"（无需本页点过）
    if (s_bt_entered_connected && s_bt_connected_idx < 0 && !s_bt_cached_addr.empty() &&
        dev.addr_hex == s_bt_cached_addr) {
        s_bt_connected_idx = idx;
        SetBtRowAction(row, 2);
    } else {
        SetBtRowAction(row, 0);
    }
}

void BtClearDeviceRows() {
    if (s_bt_dev_list != nullptr)
        lv_obj_clean(s_bt_dev_list);
    s_bt_devices.clear();
    s_bt_connecting_idx = -1;
    s_bt_connected_idx = -1;
}

void OnBtSegClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    static const mhal::bt::Mode kModes[2] = {mhal::bt::Mode::Rx, mhal::bt::Mode::Tx};
    mhal::bt::Mode target = kModes[idx];
    if (BtSegIndexOf(mhal::bt::GetMode()) == static_cast<int>(idx))
        return;
    SegSetSelected(s_bt_seg, 2, static_cast<int>(idx));  // 乐观切换；事件回填纠偏
    BtClearDeviceRows();
    s_bt_entered_connected = false;  // 切模式必断连，缓存匹配标绿作废
    SetBtStateLabel(mhal::bt::ConnState::Idle);
    s_bt_scan_on_tx = (target == mhal::bt::Mode::Tx);  // Tx 落地后自动扫描
    mhal::bt::SetMode(target);
}

void OnBtRescanClicked(lv_event_t*) {
    BtClearDeviceRows();
    if (mhal::bt::GetMode() != mhal::bt::Mode::Tx) {
        // 未在发射档：先切档，Tx 落地（on_mode_changed）后自动扫描
        SegSetSelected(s_bt_seg, 2, 1);
        s_bt_scan_on_tx = true;
        mhal::bt::SetMode(mhal::bt::Mode::Tx);
        return;
    }
    mhal::bt::StartScan();
    SetBtStateLabel(mhal::bt::ConnState::Scanning);
}

void BuildBluetoothPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::Bluetooth, "\xe8\x93\x9d\xe7\x89\x99", out_page);  // 蓝牙

    // 进页采样：扫描会把 ConnState 顶成 Scanning，"已连接"判定要用这份快照；
    // 缓存地址读一次备行匹配（见 AppendBtDeviceRow）。
    s_bt_entered_connected = mhal::bt::GetConnState() == mhal::bt::ConnState::Connected;
    s_bt_cached_addr = BtCachedAddr();

    // 两档分段：音箱 RX / 发射 TX（模组无"关闭"AT，不设关闭档）
    lv_obj_t* seg_row = MakeFlexRow(content, kSegH);
    s_bt_seg[0] =
        MakeSegBtn(seg_row, "\xe9\x9f\xb3\xe7\xae\xb1 RX", &font_puhui_24_4, OnBtSegClicked, 0);
    s_bt_seg[1] =
        MakeSegBtn(seg_row, "\xe5\x8f\x91\xe5\xb0\x84 TX", &font_puhui_24_4, OnBtSegClicked, 1);
    SegSetSelected(s_bt_seg, 2, BtSegIndexOf(mhal::bt::GetMode()));

    // 状态行：左中文"状态"，右 mono 状态
    lv_obj_t* st_row = MakeFlexRow(content, 40);
    lv_obj_t* st_k = lv_label_create(st_row);
    lv_label_set_text(st_k, "\xe7\x8a\xb6\xe6\x80\x81");  // "状态"
    SetLabelFont(st_k, &font_puhui_20_4, Tok::Dim);
    lv_obj_t* sp = lv_obj_create(st_row);
    screen_strip_obj_chrome(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_grow(sp, 1);
    s_bt_state_lbl = lv_label_create(st_row);
    SetLabelFont(s_bt_state_lbl, &font_pi_mono_17, Tok::Faint);
    lv_obj_set_style_text_letter_space(s_bt_state_lbl, 1, LV_PART_MAIN);

    // 设备卡片
    lv_obj_t* card = MakeCard(content, "DEVICES", &s_bt_dev_cap);
    s_bt_dev_list = lv_obj_create(card);
    screen_strip_obj_chrome(s_bt_dev_list);
    lv_obj_remove_flag(s_bt_dev_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_bt_dev_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(s_bt_dev_list, LV_PCT(100));
    lv_obj_set_height(s_bt_dev_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_bt_dev_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_bt_dev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bt_dev_list, 4, LV_PART_MAIN);

    // 「重新扫描」（"↻" 不在字体子集，省略图标）
    MakeActionBtn(content, "\xe9\x87\x8d\xe6\x96\xb0\xe6\x89\xab\xe6\x8f\x8f", OnBtRescanClicked);

    SetBtStateLabel(mhal::bt::GetConnState());

    // 回调注册（UART RX / AT 任务线程 -> 快照 -> tick 落地）。页面关闭时
    // PageWillClose 里取消订阅。
    mhal::bt::Callbacks cbs;
    cbs.on_mode_changed = [](mhal::bt::Mode m) {
        std::lock_guard<std::mutex> lk(s_mu);
        s_bt_evt.mode = m;
        s_bt_evt.dirty = true;
    };
    cbs.on_conn_state = [](mhal::bt::ConnState st) {
        std::lock_guard<std::mutex> lk(s_mu);
        s_bt_evt.conn = st;
        s_bt_evt.dirty = true;
    };
    cbs.on_device_found = [](const mhal::bt::Device& d) {
        std::lock_guard<std::mutex> lk(s_mu);
        s_bt_evt.found.push_back(d);
        s_bt_evt.dirty = true;
    };
    mhal::bt::SetCallbacks(std::move(cbs));
    s_bt_cbs_registered = true;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        s_bt_evt = BtSnapshot{};
        s_bt_evt.mode = mhal::bt::GetMode();
        s_bt_evt.conn = mhal::bt::GetConnState();
    }

    // 已处于发射档则进场即扫描
    if (mhal::bt::GetMode() == mhal::bt::Mode::Tx) {
        mhal::bt::StartScan();
        SetBtStateLabel(mhal::bt::ConnState::Scanning);
    }
}

// tick 落地蓝牙快照（LVGL 线程）
void DrainBtSnapshot() {
    if (s_bt_state_lbl == nullptr)
        return;
    mhal::bt::Mode mode;
    mhal::bt::ConnState conn;
    std::vector<mhal::bt::Device> found;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (!s_bt_evt.dirty)
            return;
        s_bt_evt.dirty = false;
        mode = s_bt_evt.mode;
        conn = s_bt_evt.conn;
        found.swap(s_bt_evt.found);
    }
    SegSetSelected(s_bt_seg, 2, BtSegIndexOf(mode));
    SetBtStateLabel(conn);
    if (s_bt_scan_on_tx && mode == mhal::bt::Mode::Tx) {
        s_bt_scan_on_tx = false;
        mhal::bt::StartScan();
        SetBtStateLabel(mhal::bt::ConnState::Scanning);
    }
    for (const auto& d : found)
        AppendBtDeviceRow(d);
    if (conn == mhal::bt::ConnState::Connected && s_bt_connecting_idx >= 0) {
        // 换连成功：旧"已连接"行复位（进页缓存匹配标绿的行也在内）
        if (s_bt_connected_idx >= 0 && s_bt_connected_idx != s_bt_connecting_idx) {
            lv_obj_t* old_row = lv_obj_get_child(s_bt_dev_list, s_bt_connected_idx);
            if (old_row != nullptr)
                SetBtRowAction(old_row, 0);
        }
        s_bt_connected_idx = s_bt_connecting_idx;
        s_bt_connecting_idx = -1;
        lv_obj_t* row = lv_obj_get_child(s_bt_dev_list, s_bt_connected_idx);
        if (row != nullptr)
            SetBtRowAction(row, 2);
        // 连接成功即缓存 name+addr（NVS "bt"/last_*），Hub 摘要与状态行
        // 此后优先显示该设备名。
        if (s_bt_connected_idx < static_cast<int>(s_bt_devices.size())) {
            BtSaveLastConnected(s_bt_devices[s_bt_connected_idx]);
            SetBtStateLabel(conn);  // 缓存落地后重刷状态行（显示设备名）
        }
    }
    RefreshHub();
}

// ---------------------------------------------------------------------------
// 声音页
// ---------------------------------------------------------------------------
void ApplyTtsToggleVisual() {
    if (s_tts_toggle == nullptr)
        return;
    bool on = s_hooks.get_tts != nullptr && s_hooks.get_tts();
    pi_theme::ApplyBorder(s_tts_toggle, on ? Tok::Accent : Tok::Line2);
    pi_theme::ApplyBg(s_tts_knob, on ? Tok::Accent : Tok::Faint);
    lv_obj_align(s_tts_knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -5 : 5, 0);
}

void OnTtsToggleClicked(lv_event_t*) {
    if (s_hooks.get_tts == nullptr || s_hooks.set_tts == nullptr)
        return;
    s_hooks.set_tts(!s_hooks.get_tts());  // 内部持久化 NVS "pi_screen"/"tts_on" 并刷状态栏点
    ApplyTtsToggleVisual();
    RefreshHub();
}

void OnVolChanged(lv_event_t*) {
    int v = static_cast<int>(lv_slider_get_value(s_vol_slider));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_vol_val, buf);
    uint32_t now = lv_tick_get();
    if (now - s_vol_last_apply_ms >= kSliderApplyGapMs) {  // 节流（同快捷面板）
        s_vol_last_apply_ms = now;
        mhal::audio::SetVolume(v, true);
    }
}

void OnVolReleased(lv_event_t*) {
    mhal::audio::SetVolume(static_cast<int>(lv_slider_get_value(s_vol_slider)), true);
    RefreshHub();
}

void BuildSoundPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::Sound, "\xe5\xa3\xb0\xe9\x9f\xb3", out_page);  // "声音"

    // 音量卡片（测试音按钮省略：无现成提示音资源，TTS 播放依赖在线会话，
    // 成本与耦合都超出本期 —— 见工作包报告）
    lv_obj_t* vol_card = MakeCard(content, "VOL");
    MakeBigSliderRow(vol_card, 0, 100, &s_vol_slider, &s_vol_val);
    lv_obj_add_event_cb(s_vol_slider, OnVolChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_vol_slider, OnVolReleased, LV_EVENT_RELEASED, nullptr);
    int vol = mhal::audio::GetVolume();
    lv_slider_set_value(s_vol_slider, vol, LV_ANIM_OFF);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", vol);
    lv_label_set_text(s_vol_val, buf);

    // 语音卡片：「回复语音播报 TTS」开关（与状态栏 TTS 钮同源）
    lv_obj_t* tts_card = MakeCard(content, "TTS");
    lv_obj_t* row = MakeFlexRow(tts_card, 64);
    lv_obj_t* lbl = lv_label_create(row);
    // "回复语音播报"
    lv_label_set_text(lbl,
                      "\xe5\x9b\x9e\xe5\xa4\x8d\xe8\xaf\xad\xe9\x9f\xb3\xe6\x92\xad\xe6\x8a\xa5");
    SetLabelFont(lbl, &font_puhui_24_4, Tok::Tx);
    lv_obj_t* sp = lv_obj_create(row);
    screen_strip_obj_chrome(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_grow(sp, 1);
    // 自绘开关（lv_switch 依赖面不引入）：72x40 圆角轨 + 28px 圆钮
    s_tts_toggle = lv_obj_create(row);
    screen_strip_obj_chrome(s_tts_toggle);
    lv_obj_remove_flag(s_tts_toggle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_tts_toggle, 72, 40);
    lv_obj_set_style_radius(s_tts_toggle, 20, LV_PART_MAIN);
    pi_theme::ApplyBg(s_tts_toggle, Tok::Card2);
    lv_obj_set_style_bg_opa(s_tts_toggle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tts_toggle, 1, LV_PART_MAIN);
    lv_obj_add_flag(s_tts_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_tts_toggle, 28);  // 触区补足 96
    s_tts_knob = MakeCircle(s_tts_toggle, 28, Tok::Faint);
    lv_obj_add_event_cb(s_tts_toggle, OnTtsToggleClicked, LV_EVENT_CLICKED, nullptr);
    ApplyTtsToggleVisual();
}

// ---------------------------------------------------------------------------
// 显示页
// ---------------------------------------------------------------------------
void OnBrtChanged(lv_event_t*) {
    int v = static_cast<int>(lv_slider_get_value(s_brt_slider));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_brt_val, buf);
    uint32_t now = lv_tick_get();
    if (now - s_brt_last_apply_ms >= kSliderApplyGapMs) {
        s_brt_last_apply_ms = now;
        mhal::backlight::SetBrightness(static_cast<uint8_t>(v), false);  // 即时、不持久化
    }
}

void OnBrtReleased(lv_event_t*) {
    mhal::backlight::SetBrightness(static_cast<uint8_t>(lv_slider_get_value(s_brt_slider)), true);
    RefreshHub();
}

void ApplyThemeCardVisual() {
    int theme = pi_theme::IsLight() ? 1 : 0;
    for (int i = 0; i < 2; i++) {
        if (s_theme_card[i] == nullptr)
            continue;
        bool sel = (theme == i);
        // 描边随当前主题的 accent/line 令牌（共享样式，切主题自动联动）；
        // 卡面/字色是"各自主题的示意色"，固定不随当前主题——见 Build 处。
        pi_theme::ApplyBorder(s_theme_card[i], sel ? Tok::Accent : Tok::Line);
        lv_obj_t* lbl = lv_obj_get_child(s_theme_card[i], 0);
        if (lbl != nullptr) {
            const pi_theme::Palette& pv = pi_theme::PaletteOf(i == 1);
            lv_obj_set_style_text_color(lbl, sel ? pv.accent : pv.dim, LV_PART_MAIN);
        }
    }
}

void OnThemeCardClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    pi_theme::Set(idx == 1);  // P2：立即生效（共享样式全 UI 翻转）+ NVS 持久化
    ApplyThemeCardVisual();
    RefreshHub();  // Hub「显示」行摘要里的 深色/浅色
}

constexpr int32_t kSleepSecs[4] = {30, 60, 300, 0};  // 0 = 永不

int SleepIndexOf(int32_t secs) {
    for (int i = 0; i < 4; i++) {
        if (kSleepSecs[i] == secs)
            return i;
    }
    return 3;
}

void OnSleepSegClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    {
        Settings ui("ui", true);
        ui.SetInt("sleep_s", kSleepSecs[idx]);
    }
    pi_sleep::ReloadConfig();  // 息屏状态机即时吃新档位（否则要等 ~5s 自动重读）
    SegSetSelected(s_sleep_seg, 4, static_cast<int>(idx));
}

void BuildDisplayPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::Display, "\xe6\x98\xbe\xe7\xa4\xba", out_page);  // "显示"

    // 亮度卡片（下限 5%，同快捷面板/backlight::Restore）
    lv_obj_t* brt_card = MakeCard(content, "BRT");
    MakeBigSliderRow(brt_card, 5, 100, &s_brt_slider, &s_brt_val);
    lv_obj_add_event_cb(s_brt_slider, OnBrtChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_brt_slider, OnBrtReleased, LV_EVENT_RELEASED, nullptr);
    int brt = static_cast<int>(mhal::backlight::GetBrightness());
    if (brt < 5)
        brt = 5;
    lv_slider_set_value(s_brt_slider, brt, LV_ANIM_OFF);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", brt);
    lv_label_set_text(s_brt_val, buf);

    // 主题卡片：深色 Amber Glow / 浅色 Paper Ink 双选，点选立即生效（P2）
    lv_obj_t* theme_card = MakeCard(content, "THEME");
    lv_obj_t* theme_row = MakeFlexRow(theme_card, kSegH);
    for (int i = 0; i < 2; i++) {
        lv_obj_t* c = lv_obj_create(theme_row);
        screen_strip_obj_chrome(c);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(c, LV_PCT(100));
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_style_radius(c, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
        // 卡面用各自主题的真实底/字色示意（固定局部色，不随当前主题翻转），
        // 描边/选中态在 ApplyThemeCardVisual 里按当前主题令牌刷。
        const pi_theme::Palette& pv = pi_theme::PaletteOf(i == 1);
        lv_obj_set_style_bg_color(c, i == 0 ? pv.card : pv.bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, 10, LV_PART_MAIN);
        lv_obj_t* lbl = lv_label_create(c);
        lv_label_set_text(lbl, i == 0 ? "\xe6\xb7\xb1\xe8\x89\xb2"    // "深色"
                                      : "\xe6\xb5\x85\xe8\x89\xb2");  // "浅色"
        lv_obj_set_style_text_font(lbl, &font_puhui_24_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, pv.dim, LV_PART_MAIN);  // 真值在 ApplyThemeCardVisual
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, OnThemeCardClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        s_theme_card[i] = c;
    }
    ApplyThemeCardVisual();

    // 息屏卡片：30s / 1min / 5min / 永不 -> NVS "ui"/"sleep_s"
    lv_obj_t* sleep_card = MakeCard(content, "SLEEP");
    lv_obj_t* sleep_row = MakeFlexRow(sleep_card, kSegH);
    static const char* kSleepTexts[4] = {"30s", "1min", "5min",
                                         "\xe6\xb0\xb8\xe4\xb8\x8d"};  // "永不"
    for (int i = 0; i < 4; i++) {
        s_sleep_seg[i] = MakeSegBtn(sleep_row, kSleepTexts[i], &font_puhui_24_4, OnSleepSegClicked,
                                    static_cast<intptr_t>(i));
    }
    Settings ui("ui", false);
    SegSetSelected(s_sleep_seg, 4, SleepIndexOf(ui.GetInt("sleep_s", 0)));
}

// ---------------------------------------------------------------------------
// 对话页
// ---------------------------------------------------------------------------
void OnModeSegClicked(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (s_hooks.set_zen != nullptr)
        s_hooks.set_zen(idx == 1);  // 与状态栏 mode 钮同源
    SegSetSelected(s_mode_seg, 2, static_cast<int>(idx));
    RefreshHub();
}

void BuildChatPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::Chat, "\xe5\xaf\xb9\xe8\xaf\x9d", out_page);  // "对话"

    lv_obj_t* mode_card = MakeCard(content, "MODE");
    lv_obj_t* seg_row = MakeFlexRow(mode_card, kSegH);
    s_mode_seg[0] = MakeSegBtn(seg_row, "FLOW", &font_pi_mono_20, OnModeSegClicked, 0);
    s_mode_seg[1] = MakeSegBtn(seg_row, "ZEN", &font_pi_mono_20, OnModeSegClicked, 1);
    bool zen = s_hooks.get_zen != nullptr && s_hooks.get_zen();
    SegSetSelected(s_mode_seg, 2, zen ? 1 : 0);
    lv_obj_t* desc = lv_label_create(mode_card);
    // "FLOW 过程全显 · ZEN 只看结果"
    lv_label_set_text(desc,
                      "FLOW \xe8\xbf\x87\xe7\xa8\x8b\xe5\x85\xa8\xe6\x98\xbe \xc2\xb7 ZEN "
                      "\xe5\x8f\xaa\xe7\x9c\x8b\xe7\xbb\x93\xe6\x9e\x9c");
    SetLabelFont(desc, &font_puhui_20_4, Tok::Faint);

    // 历史会话入口（展示性占位：会话历史规划走云端服务，本地不做——置灰
    // 不可点，右侧 mono 小字 "CLOUD" 标注去向）。
    lv_obj_t* hist_card = MakeCard(content, nullptr);
    lv_obj_t* row = MakeFlexRow(hist_card, 56);
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, "\xe5\x8e\x86\xe5\x8f\xb2\xe4\xbc\x9a\xe8\xaf\x9d");  // "历史会话"
    SetLabelFont(lbl, &font_puhui_24_4, Tok::Faint);
    lv_obj_t* sp = lv_obj_create(row);
    screen_strip_obj_chrome(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_t* tag = lv_label_create(row);
    lv_label_set_text(tag, "CLOUD");
    SetLabelFont(tag, &font_pi_mono_14, Tok::Faint);
    lv_obj_set_style_text_letter_space(tag, 2, LV_PART_MAIN);
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, ">");
    SetLabelFont(arrow, &font_pi_mono_17, Tok::Faint);
}

// ---------------------------------------------------------------------------
// 关于页（只读 kv，打开时刷新一次）
// ---------------------------------------------------------------------------
void BuildAboutPage(lv_obj_t** out_page) {
    lv_obj_t* content = MakePage(PageId::About, "\xe5\x85\xb3\xe4\xba\x8e", out_page);  // "关于"

    lv_obj_t* card = MakeCard(content, "DEVICE");
    char buf[96];

    // 电量（充电状态）
    lv_obj_t* v_batt = MakeKvRow(card, "\xe7\x94\xb5\xe9\x87\x8f");  // "电量"
    int level = 0;
    bool chg = false, dis = false;
    if (mhal::power::GetBatteryLevel(level, chg, dis)) {
        if (chg) {
            std::snprintf(buf, sizeof(buf), "%d%% \xc2\xb7 \xe5\x85\x85\xe7\x94\xb5\xe4\xb8\xad",
                          level);  // "· 充电中"
        } else {
            std::snprintf(buf, sizeof(buf), "%d%%", level);
        }
        lv_label_set_text(v_batt, buf);
    }

    // 电压 mV
    lv_obj_t* v_volt = MakeKvRow(card, "\xe7\x94\xb5\xe5\x8e\x8b");  // "电压"
    uint16_t mv = 0;
    if (mhal::power::GetVoltageMv(mv)) {
        std::snprintf(buf, sizeof(buf), "%u mV", static_cast<unsigned>(mv));
        lv_label_set_text(v_volt, buf);
    }

    // 固件版本（pi_sys_info.h 平台接口）
    lv_obj_t* v_fw = MakeKvRow(card, "\xe5\x9b\xba\xe4\xbb\xb6");  // "固件"
    std::snprintf(buf, sizeof(buf), "claw6 v%s", pi_sys_fw_version());
    lv_label_set_text(v_fw, buf);

    // 网络类型 / IP
    lv_obj_t* v_net = MakeKvRow(card, "\xe7\xbd\x91\xe7\xbb\x9c");  // "网络"
    {
        bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
        std::string ip = mhal::network::GetIpAddress();
        std::snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s", wifi ? "WiFi" : "4G",
                      ip.empty() ? "--" : ip.c_str());
        lv_label_set_text(v_net, buf);
    }

    // 屏幕
    lv_obj_t* v_scr = MakeKvRow(card, "\xe5\xb1\x8f\xe5\xb9\x95");  // "屏幕"
    lv_label_set_text(v_scr, "720x720 MIPI-DSI");
}

// ---------------------------------------------------------------------------
// 页栈管理
// ---------------------------------------------------------------------------
void PageWillClose(PageId id) {
    switch (id) {
        case PageId::Network:
            s_net_cap = nullptr;
            s_net_portal_state = nullptr;
            s_net_portal_btn = nullptr;
            // 确认 sheet 挂在 s_root 下而非页对象下，需手动删
            if (s_net_confirm_root != nullptr) {
                lv_obj_delete(s_net_confirm_root);
                s_net_confirm_root = nullptr;
            }
            for (auto*& o : s_net_seg)
                o = nullptr;
            for (auto*& o : s_net_k)
                o = nullptr;
            for (auto*& o : s_net_v)
                o = nullptr;
            break;
        case PageId::Bluetooth:
            if (s_bt_cbs_registered) {
                mhal::bt::SetCallbacks(mhal::bt::Callbacks{});  // 取消订阅
                s_bt_cbs_registered = false;
            }
            s_bt_state_lbl = nullptr;
            s_bt_dev_cap = nullptr;
            s_bt_dev_list = nullptr;
            s_bt_devices.clear();
            s_bt_connecting_idx = -1;
            s_bt_connected_idx = -1;
            s_bt_scan_on_tx = false;
            s_bt_entered_connected = false;
            s_bt_cached_addr.clear();
            for (auto*& o : s_bt_seg)
                o = nullptr;
            break;
        case PageId::Sound:
            s_vol_slider = s_vol_val = s_tts_toggle = s_tts_knob = nullptr;
            break;
        case PageId::Display:
            s_brt_slider = s_brt_val = nullptr;
            for (auto*& o : s_theme_card)
                o = nullptr;
            for (auto*& o : s_sleep_seg)
                o = nullptr;
            break;
        case PageId::Chat:
            for (auto*& o : s_mode_seg)
                o = nullptr;
            break;
        case PageId::Hub:
            for (auto*& o : s_hub_val)
                o = nullptr;
            s_hub_batt = nullptr;
            break;
        default:
            break;
    }
}

void Push(PageId id) {
    if (s_root == nullptr)
        return;
    lv_obj_t* page = nullptr;
    switch (id) {
        case PageId::Hub:
            BuildHubPage(&page);
            break;
        case PageId::Network:
            BuildNetworkPage(&page);
            break;
        case PageId::Bluetooth:
            BuildBluetoothPage(&page);
            break;
        case PageId::Sound:
            BuildSoundPage(&page);
            break;
        case PageId::Display:
            BuildDisplayPage(&page);
            break;
        case PageId::Chat:
            BuildChatPage(&page);
            break;
        case PageId::About:
            BuildAboutPage(&page);
            break;
    }
    if (page == nullptr)
        return;
    if (!s_stack.empty())
        lv_obj_add_flag(s_stack.back().obj, LV_OBJ_FLAG_HIDDEN);
    s_stack.push_back({id, page});
    // 页面子树全开 EVENT_BUBBLE：让任意位置起手的按压到达 s_root 的右滑
    // 返回追踪器（s_root 自身不再向 screen 冒泡，pi_screen 的手势不受扰）。
    EnableEventBubbleRecursive(page);
}

void Pop() {
    if (s_stack.empty()) {
        CloseAll();
        return;
    }
    PageEntry top = s_stack.back();
    s_stack.pop_back();
    PageWillClose(top.id);
    lv_obj_delete(top.obj);  // 确认 sheet 等挂在 s_root 下的浮层由 PageWillClose 置空前删除
    if (s_stack.empty()) {
        CloseAll();
        return;
    }
    lv_obj_remove_flag(s_stack.back().obj, LV_OBJ_FLAG_HIDDEN);
    if (s_stack.back().id == PageId::Hub)
        RefreshHub();
}

// ----- 右滑返回（s_root 上的手动追踪；滑条等横向拖拽件豁免） -----------------
constexpr lv_obj_flag_t kSwipeIgnoreFlag = LV_OBJ_FLAG_USER_1;  // 与 screen_util 同一位

bool OriginatesInDragOwner(lv_obj_t* from) {
    for (lv_obj_t* obj = from; obj != nullptr && obj != s_root; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kSwipeIgnoreFlag))
            return true;
        if (lv_obj_check_type(obj, &lv_slider_class))
            return true;
    }
    return false;
}

void OnRootPressed(lv_event_t* e) {
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    if (OriginatesInDragOwner(lv_event_get_target_obj(e))) {
        s_swipe_tracking = false;
        return;
    }
    lv_indev_get_point(indev, &s_swipe_start);
    s_swipe_tracking = true;
}

// 阈值判定放在 PRESSING（同 pi_screen 的状态栏下拉/PTT 上滑取消模式）：
// 命中即 lv_indev_wait_release 抑制原按压目标的 CLICKED —— 若等到 RELEASED
// 再判，扫过可点行（Hub 行/设备行）的右滑会同时把那一行"点"了。
void OnRootPressing(lv_event_t* e) {
    if (!s_swipe_tracking)
        return;
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_swipe_start.x;
    int32_t dy = p.y - s_swipe_start.y;
    if (dx > kSwipePopThreshold && std::abs(dy) < dx) {
        s_swipe_tracking = false;
        lv_indev_wait_release(indev);
        // 确认 sheet 打开时右滑先收 sheet，不动页栈
        if (s_net_confirm_root != nullptr &&
            !lv_obj_has_flag(s_net_confirm_root, LV_OBJ_FLAG_HIDDEN)) {
            CloseNetConfirm();
            return;
        }
        RequestPop();
    }
}

void OnRootReleased(lv_event_t*) { s_swipe_tracking = false; }

// ----- tick：30s 无操作自动退、跨线程快照落地、周期刷新 ---------------------
void TickCb(lv_timer_t*) {
    s_ticks++;

    if (lv_display_get_inactive_time(nullptr) > kAutoCloseMs) {
        CloseAll();
        return;
    }

    // 网络事件快照落地（配网热点起、连接状态变化）
    {
        bool dirty = false, portal = false;
        std::string info;
        {
            std::lock_guard<std::mutex> lk(s_mu);
            if (s_net_evt.dirty) {
                dirty = true;
                s_net_evt.dirty = false;
                portal = s_net_evt.portal;
                info = s_net_evt.portal_info;
            }
        }
        if (dirty) {
            if (portal)
                SetPortalActiveVisual(info.c_str());
            RefreshNetworkPage();
            RefreshHub();
        }
    }

    DrainBtSnapshot();

    PageId top = s_stack.empty() ? PageId::Hub : s_stack.back().id;
    // Hub 摘要 + 页头电量：2s 一刷
    if (s_ticks % 4 == 0 && top == PageId::Hub)
        RefreshHub();
    // 网络状态卡片 5s 一刷（4G 信号走 AT 通道，别刷太勤）
    if (s_ticks % 10 == 0 && top == PageId::Network)
        RefreshNetworkPage();
}

void CloseAll() {
    if (s_root == nullptr)
        return;
    for (auto& entry : s_stack)
        PageWillClose(entry.id);
    s_stack.clear();
    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    if (s_net_listener_id >= 0) {
        pi_net_events::RemoveListener(s_net_listener_id);
        s_net_listener_id = -1;
    }
    lv_obj_delete(s_root);  // 整棵删除；栈全空即露出进入前的 ViewState
    s_root = nullptr;
    s_swipe_tracking = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
namespace pi_settings {

void SetHooks(const Hooks& hooks) { s_hooks = hooks; }

void Open(lv_obj_t* parent) {
    if (s_root != nullptr)
        return;

    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_root, kW, kH);
    lv_obj_set_pos(s_root, 0, 0);
    pi_theme::ApplyBg(s_root, Tok::Bg);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // 承接按压（右滑返回追踪）
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_root, OnRootPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_root, OnRootPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_root, OnRootReleased, LV_EVENT_RELEASED, nullptr);

    // 订阅网络事件（经 pi_net_events 分发层，与 pi_screen 状态栏共存；回调
    // 仍在网络栈任务线程：只写快照）。配网热点起来时把 "ssid|url" 回填到
    // 配网卡片；其余事件统一触发一次轮询刷新。
    s_net_listener_id =
        pi_net_events::AddListener([](mhal::network::Event e, const std::string& data) {
            std::lock_guard<std::mutex> lk(s_mu);
            s_net_evt.dirty = true;
            if (e == mhal::network::Event::WifiConfigPortal) {
                s_net_evt.portal = true;
                s_net_evt.portal_info = data;
            }
        });

    Push(PageId::Hub);
    s_ticks = 0;
    s_tick_timer = lv_timer_create(TickCb, kTickMs, nullptr);
}

void Close() { CloseAll(); }

bool IsOpen() { return s_root != nullptr; }

void OnScreenUnloaded() {
    // widget 树由 LVGL 随 screen 删除，这里只清理定时器/订阅与静态指针
    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    if (s_bt_cbs_registered) {
        mhal::bt::SetCallbacks(mhal::bt::Callbacks{});
        s_bt_cbs_registered = false;
    }
    if (s_net_listener_id >= 0) {
        pi_net_events::RemoveListener(s_net_listener_id);
        s_net_listener_id = -1;
    }
    for (auto& entry : s_stack)
        PageWillClose(entry.id);
    s_stack.clear();
    s_root = nullptr;
    s_swipe_tracking = false;
}

}  // namespace pi_settings
