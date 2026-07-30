#include "pi_quick_panel.h"

#include <cstdio>
#include <string>

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/network.h"
#include "metalio_hal/power.h"
#include "pi_card_icons.h"
#include "pi_fonts.h"
#include "pi_media.h"  // 「音乐」入口：继续上次播放
#include "pi_theme.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// Quick Panel 实现。视觉沿用 pi_screen 的设计语言：纯色平面 + 1px 细线 +
// 小圆角，唯一强调色（主题 accent）；字体子集同 pi_screen（mono 无中文/装饰
// glyph，"✚/⚙/◐/⏻/⚡" 等图标一律用小 lv_obj 形状代替，中文用 puhui）。
// 配色 P2 起统一走 pi_theme 双主题令牌表（共享样式，切主题即时翻转）。
// ---------------------------------------------------------------------------
namespace {

using pi_theme::Tok;

constexpr int32_t kW = 720;
constexpr int32_t kH = 720;

constexpr int32_t kPanelRadius = 24;
constexpr int32_t kPanelTopHide = 24;  // 顶部圆角藏到屏幕外，只留"圆角24底边"
constexpr int32_t kSliderRowH = 56;
constexpr int32_t kGridBtnH = 104;  // 触点 >= 96px
constexpr int32_t kSwipeCloseThreshold = 60;

constexpr uint32_t kOffHoldMs = 2000;     // 关机需长按 2 秒
constexpr uint32_t kSliderApplyGapMs = 150;  // 拖动中节流写入（NVS 写保护，VOL/BRT 共用）
constexpr uint32_t kToastMs = 1500;
constexpr uint32_t kStatusRefreshMs = 2000;

pi_quick_panel::Hooks s_hooks;

lv_obj_t* s_root = nullptr;  // 全屏容器（scrim + 面板），隐藏即收起
lv_obj_t* s_scrim = nullptr;
lv_obj_t* s_panel = nullptr;
lv_obj_t* s_net_lbl = nullptr;
lv_obj_t* s_chg_dot = nullptr;  // 充电中的琥珀点（⚡ 不在字体子集里）
lv_obj_t* s_batt_lbl = nullptr;
lv_obj_t* s_vol_slider = nullptr;
lv_obj_t* s_vol_val = nullptr;
lv_obj_t* s_brt_slider = nullptr;
lv_obj_t* s_brt_val = nullptr;
lv_obj_t* s_off_btn = nullptr;
lv_obj_t* s_toast_lbl = nullptr;

lv_timer_t* s_status_timer = nullptr;  // 打开期间 2s 刷一次电量/网络
lv_timer_t* s_toast_timer = nullptr;
lv_timer_t* s_off_timer = nullptr;  // 关机长按进度
uint32_t s_off_press_ms = 0;
uint32_t s_vol_last_apply_ms = 0;
uint32_t s_brt_last_apply_ms = 0;

bool s_open = false;
bool s_swipe_tracking = false;
int32_t s_swipe_start_y = 0;

// ----- 小工具（与 pi_screen.cc 同款，匿名空间各自持有） --------------------
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

// ----- toast：面板底部一行短暂提示 -----------------------------------------
void ToastHideCb(lv_timer_t*) {
    if (s_toast_lbl != nullptr)
        lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = nullptr;  // repeat_count=1，LVGL 跑完自删
}

void ShowToast(const char* text, const lv_font_t* font, Tok color) {
    if (s_toast_lbl == nullptr)
        return;
    SetLabelFont(s_toast_lbl, font, color);
    lv_label_set_text(s_toast_lbl, text);
    lv_obj_remove_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer != nullptr)
        lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(ToastHideCb, kToastMs, nullptr);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

// ----- 状态行数据 -----------------------------------------------------------
void RefreshStatus() {
    if (s_net_lbl == nullptr)
        return;
    // P1 接真状态：WiFi 显示 SSID，4G 显示 "4G"+CSQ 映射的格数。s_net_lbl
    // 是 mono 字体（ASCII+·° 子集），非 ASCII SSID 无法显示时退回 "WIFI"。
    if (mhal::network::GetType() == mhal::network::Type::WiFi) {
        std::string ssid =
            mhal::network::IsConnected() ? mhal::network::GetWifiSsid() : std::string();
        bool ascii = !ssid.empty();
        for (char c : ssid) {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                ascii = false;
        }
        if (ascii) {
            lv_label_set_text(s_net_lbl, ssid.c_str());
        } else {
            lv_label_set_text(s_net_lbl, mhal::network::IsConnected() ? "WIFI" : "WIFI --");
        }
    } else {
        char nbuf[16];
        if (mhal::network::IsConnected()) {
            int csq = mhal::network::GetSignalStrength();
            int lit = csq > 20 ? 3 : (csq > 12 ? 2 : (csq > 5 ? 1 : 0));
            std::snprintf(nbuf, sizeof(nbuf), "4G %d/3", lit);
        } else {
            std::snprintf(nbuf, sizeof(nbuf), "4G --");
        }
        lv_label_set_text(s_net_lbl, nbuf);
    }

    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (mhal::power::GetBatteryLevel(level, charging, discharging)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(s_batt_lbl, buf);
        if (charging)
            lv_obj_remove_flag(s_chg_dot, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_chg_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_batt_lbl, "--%");
        lv_obj_add_flag(s_chg_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusTimerTick(lv_timer_t*) { RefreshStatus(); }

// ----- VOL / BRT 滑条 --------------------------------------------------------
// 拖动即时生效；NVS 持久化收敛到松手那一下（mhal::audio::SetVolume 内部
// 永远持久化，故拖动中按 kSliderApplyGapMs 节流写入，避免每个 move 事件都刷
// NVS；backlight 的 persist 形参是真的，拖动中 false、松手 true）。
void OnVolChanged(lv_event_t*) {
    int v = static_cast<int>(lv_slider_get_value(s_vol_slider));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_vol_val, buf);
    uint32_t now = lv_tick_get();
    if (now - s_vol_last_apply_ms >= kSliderApplyGapMs) {
        s_vol_last_apply_ms = now;
        mhal::audio::SetVolume(v, true);
    }
}

void OnVolReleased(lv_event_t*) {
    mhal::audio::SetVolume(static_cast<int>(lv_slider_get_value(s_vol_slider)), true);
}

void OnBrtChanged(lv_event_t*) {
    int v = static_cast<int>(lv_slider_get_value(s_brt_slider));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_brt_val, buf);
    uint32_t now = lv_tick_get();
    if (now - s_brt_last_apply_ms >= kSliderApplyGapMs) {
        s_brt_last_apply_ms = now;
        mhal::backlight::SetBrightness(static_cast<uint8_t>(v), false);
    }
}

void OnBrtReleased(lv_event_t*) {
    mhal::backlight::SetBrightness(static_cast<uint8_t>(lv_slider_get_value(s_brt_slider)), true);
}

lv_obj_t* MakeSliderRow(lv_obj_t* parent, const char* name, int32_t min, int32_t max,
                        lv_obj_t** out_slider, lv_obj_t** out_val) {
    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);  // 空白处的按压落到面板（上滑收起）
    lv_obj_set_size(row, LV_PCT(100), kSliderRowH);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    SetLabelFont(lbl, &font_pi_mono_17, Tok::Faint);
    lv_obj_set_style_text_letter_space(lbl, 1, LV_PART_MAIN);
    lv_obj_set_width(lbl, 48);

    lv_obj_t* slider = lv_slider_create(row);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_height(slider, 6);
    lv_obj_set_flex_grow(slider, 1);
    pi_theme::ApplyBg(slider, Tok::Card2);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 3, LV_PART_MAIN);
    pi_theme::ApplyBg(slider, Tok::Accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    // 大把手：6px 轨道 + 14px pad = 34px 直径；行高 56 的触区靠 ext_click_area 补足
    pi_theme::ApplyBg(slider, Tok::Accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 14, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 22);
    *out_slider = slider;

    lv_obj_t* val = lv_label_create(row);
    lv_label_set_text(val, "--");
    SetLabelFont(val, &font_pi_mono_20, Tok::Tx);
    lv_obj_set_width(val, 56);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    *out_val = val;
    return row;
}

// ----- 关机长按（2s，边框红色渐进） -----------------------------------------
void OffBtnResetVisual() {
    if (s_off_btn == nullptr)
        return;
    // 长按渐进用的是局部 border 色（压过共享样式），复位时摘掉局部值，
    // 让共享 Line 样式重新生效（也保证之后切主题联动）。
    lv_obj_remove_local_style_prop(s_off_btn, LV_STYLE_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_off_btn, 1, LV_PART_MAIN);
}

void OffTimerStop() {
    if (s_off_timer != nullptr) {
        lv_timer_delete(s_off_timer);
        s_off_timer = nullptr;
    }
    OffBtnResetVisual();
}

void OffTimerTick(lv_timer_t*) {
    uint32_t held = lv_tick_get() - s_off_press_ms;
    if (held >= kOffHoldMs) {
        OffTimerStop();
        mhal::power::ForcePowerOff();  // 正常情况下不返回
        return;
    }
    // 按住时边框向红色渐进 + 加粗，给足"还差多久"的反馈
    uint8_t mix = static_cast<uint8_t>(held * 255 / kOffHoldMs);
    const pi_theme::Palette& pal = pi_theme::Get();
    lv_obj_set_style_border_color(s_off_btn, lv_color_mix(pal.err, pal.line, mix), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_off_btn, held > kOffHoldMs / 2 ? 3 : 2, LV_PART_MAIN);
}

void OnOffPressed(lv_event_t*) {
    s_off_press_ms = lv_tick_get();
    if (s_off_timer == nullptr)
        s_off_timer = lv_timer_create(OffTimerTick, 50, nullptr);
}

void OnOffReleased(lv_event_t* e) {
    bool was_arming = s_off_timer != nullptr;
    OffTimerStop();
    if (was_arming && lv_event_get_code(e) == LV_EVENT_RELEASED) {
        // 短点（没撑满 2s 就松开）：闪提示
        ShowToast(
            "长按 2 秒关机",
            &font_puhui_20_4, Tok::Dim);
    }
}

// ----- 2x4 -> 一行四个动作按钮 ----------------------------------------------
lv_obj_t* MakeGridBtn(lv_obj_t* parent, const char* text_utf8) {
    lv_obj_t* btn = lv_obj_create(parent);
    screen_strip_obj_chrome(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(btn, kGridBtnH);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(btn, Tok::Line);
    pi_theme::ApplyBg(btn, Tok::Card);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    pi_theme::ApplyBg(btn, Tok::Card2, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 10, LV_PART_MAIN);

    // 20x20 图标容器，具体形状由调用方往里画（字体子集没有 ✚⚙◐⏻）
    lv_obj_t* icon = lv_obj_create(btn);
    screen_strip_obj_chrome(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(icon, 24, 24);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text_utf8);
    SetLabelFont(lbl, &font_puhui_24_4, Tok::Tx);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void BuildActionGrid(lv_obj_t* parent) {
    lv_obj_t* grid = lv_obj_create(parent);
    screen_strip_obj_chrome(grid);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grid, LV_PCT(100), kGridBtnH);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, 16, LV_PART_MAIN);

    // 「✚ 新对话」
    lv_obj_t* btn_new = MakeGridBtn(grid, "新对话");
    pi_card::MakeIcon(lv_obj_get_child(btn_new, 0), "plus", 24, Tok::Accent);
    lv_obj_add_event_cb(
        btn_new,
        [](lv_event_t*) {
            pi_quick_panel::Close();
            if (s_hooks.on_new_session != nullptr)
                s_hooks.on_new_session();
        },
        LV_EVENT_CLICKED, nullptr);

    // 「⚙ 设置」（P1 已接线：收起面板 -> 推入设置 Hub）
    lv_obj_t* btn_set = MakeGridBtn(grid, "设置");
    pi_card::MakeIcon(lv_obj_get_child(btn_set, 0), "settings", 24, Tok::Dim);
    lv_obj_add_event_cb(
        btn_set,
        [](lv_event_t*) {
            pi_quick_panel::Close();
            if (s_hooks.on_settings != nullptr)
                s_hooks.on_settings();
        },
        LV_EVENT_CLICKED, nullptr);

    // 「◐ 主题」。P2 接线：一键在深/浅主题间切换（共享样式即时翻转，面板
    // 本身也当场换装），持久化 NVS "ui"/"theme"。
    lv_obj_t* btn_theme = MakeGridBtn(grid, "主题");
    pi_card::MakeIcon(lv_obj_get_child(btn_theme, 0), "sun-moon", 24, Tok::Dim);
    lv_obj_add_event_cb(
        btn_theme, [](lv_event_t*) { pi_theme::Set(!pi_theme::IsLight()); }, LV_EVENT_CLICKED,
        nullptr);

    // 「⌂ 设备后台」（配置 + 文件两页）。仅 WiFi 已连接才能打开（4G 有运营商
    // NAT，外部连不进来）；未连接点按只吐 toast，不置灰——省一份"打开时刷新
    // 按钮态"的常驻逻辑，点按时判一次足够。
    lv_obj_t* btn_files = MakeGridBtn(grid, "后台");
    pi_card::MakeIcon(lv_obj_get_child(btn_files, 0), "globe", 24, Tok::Dim);
    lv_obj_add_event_cb(
        btn_files,
        [](lv_event_t*) {
            bool wifi_up = mhal::network::GetType() == mhal::network::Type::WiFi &&
                          mhal::network::IsConnected();
            if (!wifi_up) {
                // "需连接 WiFi"
                ShowToast(
                    "需连接 WiFi", &font_puhui_20_4, Tok::Dim);
                return;
            }
            pi_quick_panel::Close();
            if (s_hooks.on_files != nullptr)
                s_hooks.on_files();
        },
        LV_EVENT_CLICKED, nullptr);

    // 「♪ 音乐」：继续上次播放。正在播 / 有持久化记录则打开播放页续播；都没有点按
    // 只吐 toast（不置灰——同「文件」的判定策略，点按时判一次足够）。
    lv_obj_t* btn_music = MakeGridBtn(grid, "音乐");
    pi_card::MakeIcon(lv_obj_get_child(btn_music, 0), "music", 24, Tok::Dim);
    lv_obj_add_event_cb(
        btn_music,
        [](lv_event_t*) {
            // "没有可继续的播放"
            const char* kNone =
                "没有可继续的播放";
            if (!pi_media::HasResumable()) {
                ShowToast(kNone, &font_puhui_20_4, Tok::Dim);
                return;
            }
            switch (pi_media::ResumeLast()) {
                case pi_media::ResumeResult::Opened:
                    pi_quick_panel::Close();
                    break;
                case pi_media::ResumeResult::NoNetwork:
                    // "无网络连接"
                    ShowToast("无网络连接",
                              &font_puhui_20_4, Tok::Dim);
                    break;
                case pi_media::ResumeResult::FilesGone:
                    // "文件已不存在"
                    ShowToast("文件已不存在",
                              &font_puhui_20_4, Tok::Dim);
                    break;
                default:
                    ShowToast(kNone, &font_puhui_20_4, Tok::Dim);
                    break;
            }
        },
        LV_EVENT_CLICKED, nullptr);

    // 「⏻ 关机」：长按 2s 才关机
    s_off_btn = MakeGridBtn(grid, "关机");
    pi_card::MakeIcon(lv_obj_get_child(s_off_btn, 0), "power", 24, Tok::Dim);
    lv_obj_add_event_cb(s_off_btn, OnOffPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_off_btn, OnOffReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_off_btn, OnOffReleased, LV_EVENT_PRESS_LOST, nullptr);
}

// ----- 面板内上滑收起（滑条/按钮各拥其事件，只有面板空白区走到这里） -------
void OnPanelPressed(lv_event_t* e) {
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_swipe_start_y = p.y;
    s_swipe_tracking = true;
}

void OnPanelPressing(lv_event_t* e) {
    if (!s_swipe_tracking)
        return;
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (s_swipe_start_y - p.y > kSwipeCloseThreshold) {
        s_swipe_tracking = false;
        lv_indev_wait_release(indev);
        pi_quick_panel::Close();
    }
}

void OnPanelReleased(lv_event_t*) { s_swipe_tracking = false; }

// ----- 开合动画 --------------------------------------------------------------
void PanelYExecCb(void* var, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(var), v); }

}  // namespace

namespace pi_quick_panel {

void Create(lv_obj_t* parent, const Hooks& hooks) {
    s_hooks = hooks;

    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_root, kW, kH);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    // scrim：半透明遮罩，点按收起
    s_scrim = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_scrim);
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_scrim, kW, kH);
    pi_theme::ApplyScrim(s_scrim);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, [](lv_event_t*) { Close(); }, LV_EVENT_CLICKED, nullptr);

    // 面板本体：顶到 y=-24（radius 24 的上半藏到屏外 -> 视觉上只有圆角底边）
    s_panel = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_panel);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_panel, kW);
    lv_obj_set_height(s_panel, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_panel, 0, -kPanelTopHide);
    lv_obj_set_style_radius(s_panel, kPanelRadius, LV_PART_MAIN);
    pi_theme::ApplyBg(s_panel, Tok::Card);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(s_panel, Tok::Line);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);  // 承接空白区按压（上滑收起）
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_panel, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_panel, kPanelTopHide + 24, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_panel, 16, LV_PART_MAIN);
    lv_obj_add_event_cb(s_panel, OnPanelPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_panel, OnPanelPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_panel, OnPanelReleased, LV_EVENT_RELEASED, nullptr);

    // 状态行：网络类型 | 充电点 + 电量
    lv_obj_t* status = lv_obj_create(s_panel);
    screen_strip_obj_chrome(status);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(status, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_net_lbl = lv_label_create(status);
    lv_label_set_text(s_net_lbl, "--");
    SetLabelFont(s_net_lbl, &font_pi_mono_20, Tok::Dim);
    lv_obj_set_style_text_letter_space(s_net_lbl, 1, LV_PART_MAIN);

    lv_obj_t* batt_box = lv_obj_create(status);
    screen_strip_obj_chrome(batt_box);
    lv_obj_remove_flag(batt_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(batt_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(batt_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(batt_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(batt_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batt_box, 8, LV_PART_MAIN);
    s_chg_dot = MakeCircle(batt_box, 8, Tok::Accent);  // ⚡ 的形状替身：充电中亮琥珀点
    lv_obj_add_flag(s_chg_dot, LV_OBJ_FLAG_HIDDEN);
    s_batt_lbl = lv_label_create(batt_box);
    lv_label_set_text(s_batt_lbl, "--%");
    SetLabelFont(s_batt_lbl, &font_pi_mono_20, Tok::Tx);

    lv_obj_t* rule = MakeRect(s_panel, kW - 64, 1, Tok::Line);
    (void)rule;

    MakeSliderRow(s_panel, "VOL", 0, 100, &s_vol_slider, &s_vol_val);
    lv_obj_add_event_cb(s_vol_slider, OnVolChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_vol_slider, OnVolReleased, LV_EVENT_RELEASED, nullptr);

    // 亮度下限 5%（backlight::Restore 同款下限，避免拖到全黑）
    MakeSliderRow(s_panel, "BRT", 5, 100, &s_brt_slider, &s_brt_val);
    lv_obj_add_event_cb(s_brt_slider, OnBrtChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_brt_slider, OnBrtReleased, LV_EVENT_RELEASED, nullptr);

    BuildActionGrid(s_panel);

    s_toast_lbl = lv_label_create(s_panel);
    lv_label_set_text(s_toast_lbl, "");
    SetLabelFont(s_toast_lbl, &font_pi_mono_17, Tok::Accent);
    lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_toast_lbl, LV_PCT(100));
    lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);

    s_status_timer = lv_timer_create(StatusTimerTick, kStatusRefreshMs, nullptr);
    lv_timer_pause(s_status_timer);
}

void Open() {
    if (s_root == nullptr || s_open)
        return;
    s_open = true;
    s_swipe_tracking = false;

    // 打开瞬间灌入当前真实值，滑条不回跳
    int vol = mhal::audio::GetVolume();
    lv_slider_set_value(s_vol_slider, vol, LV_ANIM_OFF);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", vol);
    lv_label_set_text(s_vol_val, buf);

    int brt = static_cast<int>(mhal::backlight::GetBrightness());
    if (brt < 5)
        brt = 5;
    lv_slider_set_value(s_brt_slider, brt, LV_ANIM_OFF);
    std::snprintf(buf, sizeof(buf), "%d", brt);
    lv_label_set_text(s_brt_val, buf);

    RefreshStatus();
    lv_timer_resume(s_status_timer);
    lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_panel);
    int32_t h = lv_obj_get_height(s_panel);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, PanelYExecCb);
    lv_anim_set_values(&a, -h, -kPanelTopHide);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void Close() {
    if (s_root == nullptr || !s_open)
        return;
    s_open = false;
    s_swipe_tracking = false;
    lv_anim_delete(s_panel, PanelYExecCb);
    lv_obj_set_y(s_panel, -kPanelTopHide);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_status_timer);
    OffTimerStop();
    if (s_toast_timer != nullptr) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = nullptr;
    }
    lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);
}

void Toggle() {
    if (s_open)
        Close();
    else
        Open();
}

bool IsOpen() { return s_open; }

void OnScreenUnloaded() {
    if (s_status_timer != nullptr) {
        lv_timer_delete(s_status_timer);
        s_status_timer = nullptr;
    }
    if (s_toast_timer != nullptr) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = nullptr;
    }
    if (s_off_timer != nullptr) {
        lv_timer_delete(s_off_timer);
        s_off_timer = nullptr;
    }
    s_root = s_scrim = s_panel = nullptr;
    s_net_lbl = s_chg_dot = s_batt_lbl = nullptr;
    s_vol_slider = s_vol_val = s_brt_slider = s_brt_val = nullptr;
    s_off_btn = s_toast_lbl = nullptr;
    s_open = false;
    s_swipe_tracking = false;
}

}  // namespace pi_quick_panel
