// stock_detail_view.cc — 见头文件。

#include "stock_detail_view.h"

#include "beijing_clock.h"
#include "quote_parse.h"
#include "screen_util.h"
#include "stock_api.h"
#include "stock_chart_renderer.h"
#include "stock_fetch_scheduler.h"
#include "stock_fetch_worker.h"
#include "stock_kline_window.h"
#include "stock_store.h"
#include "stock_ui_theme.h"

#include "touch_feed.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace stock_detail_view {
namespace {

constexpr char TAG[] = "stock_ui";

// 布局（720x720）：顶栏96（含模式 tab）+ 画布500 + 时间轴/hover 带32 + 状态条92。
// tab 移入标题行中间，取消独立 tab 行，画布放大到 500。
constexpr int32_t kPanelW = 720;
constexpr int32_t kTopH = 96;
constexpr int32_t kCanvasX = 16;
constexpr int32_t kCanvasY = kTopH;  // 96
constexpr int32_t kCanvasW = 688;
constexpr int32_t kCanvasH = 500;
constexpr int32_t kAxisY = kCanvasY + kCanvasH;  // 596：时间轴 + hover 气泡带
constexpr int32_t kStatusY = 628;                // 状态栏 628..720

constexpr uint32_t kCrosshairThrottleMs = 50;    // ~20Hz
constexpr uint32_t kHoverLabelThrottleMs = 200;  // ~5Hz
constexpr uint32_t kDoubleTapMs = 400;

const char* const kModeLabels[CHART_MODE_COUNT] = {"分时", "五日", "日K", "周K"};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_name = nullptr;
lv_obj_t* s_code = nullptr;
lv_obj_t* s_price = nullptr;
lv_obj_t* s_chg = nullptr;
lv_obj_t* s_canvas = nullptr;
lv_obj_t* s_hover_layer = nullptr;
lv_obj_t* s_crosshair = nullptr;
lv_obj_t* s_hover_label = nullptr;
lv_obj_t* s_chart_status = nullptr;
bool s_net_ready = true;
lv_obj_t* s_max_price = nullptr;
lv_obj_t* s_min_price = nullptr;
lv_obj_t* s_max_pct = nullptr;
lv_obj_t* s_min_pct = nullptr;
lv_obj_t* s_x_left = nullptr;
lv_obj_t* s_x_right = nullptr;
lv_obj_t* s_zoom_pill = nullptr;
lv_obj_t* s_stat_val[8] = {};  // 开/高/低/昨 · 量/额/换手/振幅
lv_obj_t* s_tab_btn[CHART_MODE_COUNT] = {};

uint8_t* s_canvas_buf = nullptr;

Callbacks s_cb;
ChartMode s_mode = CHART_MIN_1D;
ChartSeries s_chart;  // 当前渲染中的数据（值拷贝，供 hover 复用）
bool s_chart_valid = false;
StockQuote s_quote;
bool s_quote_valid = false;
MarketStatus s_market;

uint32_t s_quote_last_ms = 0;
uint32_t s_chart_last_ms = 0;

bool s_hovering = false;
uint32_t s_last_cross_ms = 0;
uint32_t s_last_hlabel_ms = 0;
uint32_t s_last_release_ms = 0;

// 双指缩放状态
bool s_pinch_active = false;
float s_pinch_start_dist = 0;
uint32_t s_last_pinch_ms = 0;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
bool IsKline() { return s_mode == CHART_KLINE_D || s_mode == CHART_KLINE_W; }

bool CurrentInSession() {
    using quote_parse::Market;
    Market m = quote_parse::marketOf(stock_store::CurrentSymbol().c_str());
    switch (m) {
        case Market::A_SH: return s_market.sh_open;
        case Market::A_SZ: return s_market.sz_open;
        case Market::HK: return s_market.hk_open;
        case Market::US: return s_market.us_open;
        default: return false;
    }
}

void FormatCompact(double v, char* buf, size_t n) {
    double a = v < 0 ? -v : v;
    if (a >= 1e8) std::snprintf(buf, n, "%.2f亿", v / 1e8);
    else if (a >= 1e4) std::snprintf(buf, n, "%.2f万", v / 1e4);
    else std::snprintf(buf, n, "%.0f", v);
}

// recolor 用的十六进制色（相对参考价的红涨绿跌）。
const char* TrendHex(float v, float ref) {
    if (!(ref > 0)) return "e6e6e6";
    if (v > ref) return "ff3b30";
    if (v < ref) return "26c281";
    return "e6e6e6";
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(l, "");
    return l;
}

// ---- 事件回调（前向声明）----
void OnBackCb(lv_event_t* e);
void OnSymbolCb(lv_event_t* e);
void OnTabCb(lv_event_t* e);
void OnZoomCb(lv_event_t* e);
void OnHoverEvent(lv_event_t* e);
void RequestChartNow();
void ClearChartUi();
void ApplyLiveQuoteToTop();
void UpdateChartStatus();
void UpdateMinuteAxis();

// ---- 构建各区 ----
void BuildTopBar(lv_obj_t* root) {
    lv_obj_t* bar = lv_obj_create(root);
    screen_strip_obj_chrome(bar);
    lv_obj_set_size(bar, kPanelW, kTopH);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(bar);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 56, 56);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_event_cb(back, OnBackCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);

    // 名称 + 代码（点击切下一支），紧凑放左侧
    lv_obj_t* namebox = lv_obj_create(bar);
    screen_strip_obj_chrome(namebox);
    lv_obj_set_style_bg_opa(namebox, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(namebox, 172, kTopH);
    lv_obj_align(namebox, LV_ALIGN_LEFT_MID, 76, 0);
    lv_obj_remove_flag(namebox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(namebox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(namebox, OnSymbolCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(namebox, true);
    s_name = MakeLabel(namebox, &font_puhui_30_4, stock_ui::kColorText);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_name, 168);
    lv_obj_align(s_name, LV_ALIGN_LEFT_MID, 0, -14);
    s_code = MakeLabel(namebox, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_code, LV_ALIGN_LEFT_MID, 0, 18);

    // 4 个模式 tab，居中挨在一起
    lv_obj_t* tabs = lv_obj_create(bar);
    screen_strip_obj_chrome(tabs);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(tabs, 264, 52);
    lv_obj_align(tabs, LV_ALIGN_CENTER, 30, 0);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 4, LV_PART_MAIN);
    for (int i = 0; i < CHART_MODE_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(tabs);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 60, 44);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(btn, OnTabCb, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(btn, true);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, kModeLabels[i]);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        s_tab_btn[i] = btn;
    }

    // 现价 + 涨跌
    s_price = MakeLabel(bar, &font_puhui_number_50_4, stock_ui::kColorText);
    lv_obj_align(s_price, LV_ALIGN_RIGHT_MID, -16, -12);
    s_chg = MakeLabel(bar, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_chg, LV_ALIGN_RIGHT_MID, -16, 22);
}

void BuildCanvasArea(lv_obj_t* root) {
    s_canvas = lv_canvas_create(root);
    lv_obj_set_pos(s_canvas, kCanvasX, kCanvasY);
    // 缓冲在 Build 时一次性分配（RGB565 省内存）。
    s_canvas_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, static_cast<size_t>(kCanvasW) * kCanvasH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_canvas_buf == nullptr) {
        s_canvas_buf = static_cast<uint8_t*>(
            heap_caps_malloc(static_cast<size_t>(kCanvasW) * kCanvasH * 2, MALLOC_CAP_8BIT));
    }
    if (s_canvas_buf != nullptr) {
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, kCanvasW, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(s_canvas, lv_color_hex(stock_ui::kColorBg), LV_OPA_COVER);
    }

    // 浮动坐标 label
    s_max_price = MakeLabel(root, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_max_price, LV_ALIGN_TOP_LEFT, kCanvasX + 4, kCanvasY + 2);
    s_min_price = MakeLabel(root, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_min_price, LV_ALIGN_TOP_LEFT, kCanvasX + 4, kCanvasY + kCanvasH - 26);
    s_max_pct = MakeLabel(root, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_max_pct, LV_ALIGN_TOP_RIGHT, -(kCanvasX + 4), kCanvasY + 2);
    s_min_pct = MakeLabel(root, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_min_pct, LV_ALIGN_TOP_RIGHT, -(kCanvasX + 4), kCanvasY + kCanvasH - 26);
    // X 轴日期/时间：画布正下方左右角
    s_x_left = MakeLabel(root, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_x_left, LV_ALIGN_TOP_LEFT, kCanvasX + 4, kAxisY + 4);
    s_x_right = MakeLabel(root, &font_puhui_20_4, stock_ui::kColorDim);
    lv_obj_align(s_x_right, LV_ALIGN_TOP_RIGHT, -(kCanvasX + 4), kAxisY + 4);

    // zoomPill：浮在画布右上角的可点击圆角标签（仅 K 线显示）
    s_zoom_pill = lv_label_create(root);
    lv_label_set_text(s_zoom_pill, "");
    lv_obj_set_style_text_font(s_zoom_pill, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_zoom_pill, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_zoom_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_zoom_pill, lv_color_hex(0x263041), LV_PART_MAIN);
    lv_obj_set_style_radius(s_zoom_pill, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_zoom_pill, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_zoom_pill, 8, LV_PART_MAIN);
    lv_obj_align(s_zoom_pill, LV_ALIGN_TOP_RIGHT, -(kCanvasX + 6), kCanvasY + 8);
    lv_obj_add_flag(s_zoom_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_zoom_pill, OnZoomCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_zoom_pill, true);
    lv_obj_add_flag(s_zoom_pill, LV_OBJ_FLAG_HIDDEN);

    // 图表区状态（加载中 / 连接网络中），居中于画布
    s_chart_status = lv_label_create(root);
    lv_label_set_text(s_chart_status, "加载中…");
    lv_obj_set_style_text_font(s_chart_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_chart_status, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_chart_status, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chart_status, lv_color_hex(0x1E2530), LV_PART_MAIN);
    lv_obj_set_style_radius(s_chart_status, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_chart_status, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_chart_status, 10, LV_PART_MAIN);
    lv_obj_align(s_chart_status, LV_ALIGN_TOP_MID, 0, kCanvasY + kCanvasH / 2 - 20);

    // 十字光标（竖线）
    s_crosshair = lv_obj_create(root);
    screen_strip_obj_chrome(s_crosshair);
    lv_obj_set_size(s_crosshair, 2, kCanvasH);
    lv_obj_set_style_bg_color(s_crosshair, lv_color_hex(0xC8D0DC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_crosshair, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(s_crosshair, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_crosshair, LV_OBJ_FLAG_HIDDEN);

    // hover 信息条：做成浮动 tooltip（圆角深底 + recolor 上色）
    s_hover_label = lv_label_create(root);
    lv_label_set_recolor(s_hover_label, true);
    lv_label_set_text(s_hover_label, "");
    lv_obj_set_style_text_font(s_hover_label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hover_label, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_hover_label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hover_label, lv_color_hex(0x1E2530), LV_PART_MAIN);
    lv_obj_set_style_radius(s_hover_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_hover_label, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_hover_label, 6, LV_PART_MAIN);
    lv_obj_align(s_hover_label, LV_ALIGN_TOP_MID, 0, kAxisY + 2);

    // hover 捕获层（透明，覆盖画布，接收拖动；禁止右滑返回抢占）
    s_hover_layer = lv_obj_create(root);
    screen_strip_obj_chrome(s_hover_layer);
    lv_obj_set_size(s_hover_layer, kCanvasW, kCanvasH);
    lv_obj_set_pos(s_hover_layer, kCanvasX, kCanvasY);
    lv_obj_set_style_bg_opa(s_hover_layer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_hover_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_hover_layer, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(s_hover_layer, true);
    lv_obj_add_event_cb(s_hover_layer, OnHoverEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_hover_layer, OnHoverEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_hover_layer, OnHoverEvent, LV_EVENT_PRESS_LOST, nullptr);

    // zoomPill 需在 hover 捕获层之上才能点击；crosshair/tooltip 也提到前面。
    lv_obj_move_foreground(s_zoom_pill);
    lv_obj_move_foreground(s_crosshair);
    lv_obj_move_foreground(s_hover_label);
    lv_obj_move_foreground(s_chart_status);
}

// 底部状态栏：8 个 stat cell（caption 灰 + value）排成 4 列 × 2 行网格。
lv_obj_t* MakeStatCell(lv_obj_t* parent, const char* caption) {
    lv_obj_t* cell = lv_obj_create(parent);
    screen_strip_obj_chrome(cell);
    lv_obj_set_size(cell, 168, 34);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, 8, LV_PART_MAIN);

    lv_obj_t* cap = lv_label_create(cell);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap, stock_ui::Dim(), LV_PART_MAIN);

    lv_obj_t* val = lv_label_create(cell);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, stock_ui::Text(), LV_PART_MAIN);
    return val;
}

void BuildStatusBar(lv_obj_t* root) {
    lv_obj_t* bar = lv_obj_create(root);
    screen_strip_obj_chrome(bar);
    lv_obj_set_size(bar, kPanelW, 720 - kStatusY);
    lv_obj_set_pos(bar, 0, kStatusY);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x141922), LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(bar, 2, LV_PART_MAIN);

    static const char* kCaps[8] = {"开", "高", "低", "昨", "量", "额", "换手", "振幅"};
    for (int i = 0; i < 8; i++) s_stat_val[i] = MakeStatCell(bar, kCaps[i]);
}

// ---- 图表请求 / 清理 ----
void RequestChartNow() {
    const std::string& sym = stock_store::CurrentSymbol();
    if (sym.empty()) return;
    if (IsKline()) {
        stock_fetch::EnqueueChartRange(sym.c_str(), s_mode, "",
                                       stock_kline_window::CurrentZoomBars());
    } else {
        stock_fetch::EnqueueChart(sym.c_str(), s_mode);
    }
    s_chart_last_ms = NowMs();
}

void ClearChartUi() {
    s_chart_valid = false;
    s_chart.count = 0;
    s_chart.valid = false;
    stock_chart_renderer::Clear();
    lv_label_set_text(s_x_left, "");
    lv_label_set_text(s_x_right, "");
    UpdateChartStatus();
}

void UpdateTabHighlight() {
    for (int i = 0; i < CHART_MODE_COUNT; i++) {
        if (!s_tab_btn[i]) continue;
        lv_obj_t* lbl = lv_obj_get_child(s_tab_btn[i], 0);
        bool sel = (i == s_mode);
        lv_obj_set_style_text_color(lbl, sel ? stock_ui::Text() : stock_ui::Dim(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_tab_btn[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_tab_btn[i], lv_color_hex(0x263041), LV_PART_MAIN);
        lv_obj_set_style_radius(s_tab_btn[i], 8, LV_PART_MAIN);
    }
}

void EnterModeUi() {
    if (IsKline()) {
        stock_kline_window::EnterMode(s_mode);
    } else {
        stock_kline_window::LeaveMode();
    }
    UpdateTabHighlight();
}

// 把实时报价写回顶部（hover 结束时恢复）。
void ApplyLiveQuoteToTop() {
    if (!s_quote_valid) return;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f", s_quote.current);
    lv_label_set_text(s_price, buf);
    std::snprintf(buf, sizeof(buf), "%+.2f  %+.2f%%", s_quote.chg, s_quote.percent);
    lv_label_set_text(s_chg, buf);
    lv_color_t c = stock_ui::PctColor(s_quote.percent);
    lv_obj_set_style_text_color(s_price, c, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_chg, c, LV_PART_MAIN);
}

// ---- hover ----
void HideCrosshair() {
    if (s_crosshair) lv_obj_add_flag(s_crosshair, LV_OBJ_FLAG_HIDDEN);
    if (s_hover_label) {
        lv_label_set_text(s_hover_label, "");
        lv_obj_set_style_bg_opa(s_hover_label, LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

// 图表区状态提示：未就绪「连接网络中…」/ 无数据「加载中…」/ 有数据隐藏。
void UpdateChartStatus() {
    if (s_chart_status == nullptr) return;
    if (s_chart_valid) {
        lv_obj_add_flag(s_chart_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_chart_status, s_net_ready ? "加载中…" : "连接网络中…");
    lv_obj_remove_flag(s_chart_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_chart_status);
}

// 分时/5日 时间轴：左=首点、右=末点（分时 HH:MM，5日 MM-DD）。
void UpdateMinuteAxis() {
    if (!s_x_left || !s_x_right) return;
    if (!s_chart_valid || s_chart.count == 0) {
        lv_label_set_text(s_x_left, "");
        lv_label_set_text(s_x_right, "");
        return;
    }
    time_t t0 = static_cast<time_t>(s_chart.timestamps_s[0]);
    time_t t1 = static_cast<time_t>(s_chart.timestamps_s[s_chart.count - 1]);
    struct tm a = *localtime(&t0);
    struct tm b = *localtime(&t1);
    char l[16], r[16];
    if (s_mode == CHART_MIN_5D) {
        std::snprintf(l, sizeof(l), "%02d-%02d", a.tm_mon + 1, a.tm_mday);
        std::snprintf(r, sizeof(r), "%02d-%02d", b.tm_mon + 1, b.tm_mday);
    } else {
        std::snprintf(l, sizeof(l), "%02d:%02d", a.tm_hour, a.tm_min);
        std::snprintf(r, sizeof(r), "%02d:%02d", b.tm_hour, b.tm_min);
    }
    lv_label_set_text(s_x_left, l);
    lv_label_set_text(s_x_right, r);
}

void ShowHoverAt(int idx) {
    if (!s_chart_valid || idx < 0 || static_cast<size_t>(idx) >= s_chart.count) return;
    uint32_t now = NowMs();

    if (s_crosshair && (now - s_last_cross_ms) >= kCrosshairThrottleMs) {
        int cx = kCanvasX + stock_chart_renderer::XForIndex(idx, s_chart);
        if (cx < kCanvasX) cx = kCanvasX;
        if (cx > kCanvasX + kCanvasW - 1) cx = kCanvasX + kCanvasW - 1;
        lv_obj_set_pos(s_crosshair, cx, kCanvasY);
        lv_obj_remove_flag(s_crosshair, LV_OBJ_FLAG_HIDDEN);
        s_last_cross_ms = now;
    }

    if (s_hover_label && (now - s_last_hlabel_ms) >= kHoverLabelThrottleMs) {
        char buf[140];
        time_t ts = static_cast<time_t>(s_chart.timestamps_s[idx]);
        struct tm tm_v = *localtime(&ts);
        if (IsKline()) {
            float o = s_chart.opens[idx], h = s_chart.highs[idx];
            float l = s_chart.lows[idx], c = s_chart.points[idx];
            std::snprintf(buf, sizeof(buf),
                          "#8b93a1 %02d-%02d#   开#%s %.2f#   高#ff3b30 %.2f#   "
                          "低#26c281 %.2f#   收#%s %.2f#",
                          tm_v.tm_mon + 1, tm_v.tm_mday, TrendHex(o, s_chart.points[0]), o, h, l,
                          TrendHex(c, o), c);
        } else {
            float px = s_chart.points[idx];
            float ref = s_chart.last_close;
            float pct = (ref > 0) ? (px - ref) / ref * 100.0f : 0;
            const char* tc = TrendHex(px, ref);
            std::snprintf(buf, sizeof(buf), "#8b93a1 %02d:%02d#    #%s %.2f#    #%s %+.2f%%#",
                          tm_v.tm_hour, tm_v.tm_min, tc, px, tc, pct);
        }
        lv_label_set_text(s_hover_label, buf);
        lv_obj_set_style_bg_opa(s_hover_label, LV_OPA_90, LV_PART_MAIN);
        s_last_hlabel_ms = now;
    }
}

// 双指缩放：K 线模式下两指张合切换缩放档位。返回是否处于 pinch 中。
bool HandlePinch() {
    if (!IsKline()) return false;
    int16_t x0, y0, x1, y1;
    if (touch_feed_finger_count(&x0, &y0, &x1, &y1) < 2) {
        s_pinch_active = false;
        return false;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (!s_pinch_active) {
        s_pinch_active = true;
        s_pinch_start_dist = dist > 1 ? dist : 1;
        HideCrosshair();
        return true;
    }
    uint32_t now = NowMs();
    if (now - s_last_pinch_ms < 350) return true;  // 每档间隔节流
    float ratio = dist / s_pinch_start_dist;
    const char* sym = stock_store::CurrentSymbol().c_str();
    if (ratio > 1.35f) {  // 张开 → 放大（减少根数）
        if (stock_kline_window::ZoomStep(sym, s_mode, true)) {
            s_pinch_start_dist = dist;
            s_last_pinch_ms = now;
        }
    } else if (ratio < 0.75f) {  // 收拢 → 缩小（增加根数）
        if (stock_kline_window::ZoomStep(sym, s_mode, false)) {
            s_pinch_start_dist = dist;
            s_last_pinch_ms = now;
        }
    }
    return true;
}

void OnHoverEvent(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) {
        if (HandlePinch()) return;  // 双指缩放优先，不走十字光标
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int rel_x = p.x - kCanvasX;
        if (rel_x < 0) rel_x = 0;
        if (rel_x > kCanvasW - 1) rel_x = kCanvasW - 1;
        s_hovering = true;
        int idx = stock_chart_renderer::HoverIndex(rel_x, s_chart);
        ShowHoverAt(idx);
    } else {  // RELEASED / PRESS_LOST
        uint32_t now = NowMs();
        bool was_pinch = s_pinch_active;
        s_pinch_active = false;
        // 双击（<400ms 两次释放）→ 强制刷新（pinch 抬指不算双击）
        if (!was_pinch && code == LV_EVENT_RELEASED && (now - s_last_release_ms) < kDoubleTapMs) {
            s_quote_last_ms = 0;
            s_chart_last_ms = 0;
        }
        s_last_release_ms = now;
        s_hovering = false;
        HideCrosshair();
        ApplyLiveQuoteToTop();
    }
}

// ---- 事件回调实现 ----
void OnBackCb(lv_event_t*) {
    if (s_cb.on_back) s_cb.on_back();
}
void OnSymbolCb(lv_event_t*) { SwitchNextStock(); }
void OnTabCb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_current_target_obj(e);
    int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
    if (i < 0 || i >= CHART_MODE_COUNT || i == s_mode) return;
    s_mode = static_cast<ChartMode>(i);
    stock_fetch::BumpSession();  // 作废旧模式在途结果
    ClearChartUi();
    EnterModeUi();
    s_chart_last_ms = 0;  // 立即重拉
    RequestChartNow();
}
void OnZoomCb(lv_event_t*) {
    if (!IsKline()) return;
    stock_kline_window::OnZoomCycle(stock_store::CurrentSymbol().c_str(), s_mode);
}

}  // namespace

// ================= 公开 API =================

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb) {
    s_cb = cb;
    lv_obj_t* root = lv_obj_create(parent);
    screen_strip_obj_chrome(root);
    lv_obj_set_size(root, kPanelW, kPanelW);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    s_root = root;

    BuildTopBar(root);
    BuildCanvasArea(root);
    BuildStatusBar(root);

    stock_chart_renderer::Attach(s_canvas, s_max_price, s_min_price, s_max_pct, s_min_pct);
    stock_kline_window::Attach(s_zoom_pill, s_x_left, s_x_right);
    return root;
}

void Show() {
    if (s_root) lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_hovering = false;
    HideCrosshair();
    LoadCurrentStock();
}

void Hide() {
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_hovering = false;
    HideCrosshair();
}

void LoadCurrentStock() {
    s_mode = CHART_MIN_1D;
    stock_kline_window::ResetState();
    s_quote_valid = false;
    s_chart_valid = false;
    s_quote_last_ms = 0;
    s_chart_last_ms = 0;

    const std::string& name = stock_store::CurrentName();
    const std::string& sym = stock_store::CurrentSymbol();
    lv_label_set_text(s_name, name.empty() ? sym.c_str() : name.c_str());
    lv_label_set_text(s_code, stock_api::DisplayCode(sym.c_str()).c_str());
    lv_label_set_text(s_price, "--");
    lv_label_set_text(s_chg, "");
    lv_obj_set_style_text_color(s_price, stock_ui::Text(), LV_PART_MAIN);

    ClearChartUi();
    EnterModeUi();

    // 立即拉一轮
    stock_fetch::BumpSession();
    std::string one[1] = {sym};
    stock_fetch::EnqueueQuoteBatch(one, 1);
    s_quote_last_ms = NowMs();
    RequestChartNow();
}

void SwitchNextStock() {
    stock_store::Next();
    LoadCurrentStock();
}

void Tick(uint32_t now) {
    const std::string& sym = stock_store::CurrentSymbol();
    if (sym.empty()) return;
    bool clock_ready = beijing_clock::Valid();
    std::time_t bj = beijing_clock::NowEpochS();
    // 报价 pacing
    {
        StockFetchScheduler::State st;
        st.now_ms = now;
        st.last_fetch_at_ms = s_quote_last_ms;
        st.clock_ready = clock_ready;
        st.valid = s_quote_valid;
        st.in_session = CurrentInSession();
        st.block_fetch = s_hovering;
        st.in_flight = stock_fetch::InFlight(stock_fetch::FETCH_QUOTE_BATCH);
        st.bj_epoch = bj;
        StockFetchScheduler::Policy p(1000, 60000, 1000, 1000);
        if (StockFetchScheduler::shouldFetch(st, p)) {
            std::string one[1] = {sym};
            if (stock_fetch::EnqueueQuoteBatch(one, 1)) s_quote_last_ms = now;
        }
    }
    // 图表 pacing
    {
        bool kline = IsKline();
        StockFetchScheduler::State st;
        st.now_ms = now;
        st.last_fetch_at_ms = s_chart_last_ms;
        st.clock_ready = clock_ready;
        st.valid = s_chart_valid;
        st.in_session = CurrentInSession();
        st.block_fetch = s_hovering;
        st.in_flight = stock_fetch::InFlight(stock_fetch::FETCH_CHART) ||
                       stock_fetch::InFlight(stock_fetch::FETCH_CHART_RANGE);
        st.bj_epoch = bj;
        StockFetchScheduler::Policy p(kline ? 30000 : 10000, 60000, 3000, 3000);
        if (StockFetchScheduler::shouldFetch(st, p)) {
            RequestChartNow();
        }
    }
}

void ApplyQuote(const StockQuote& q) {
    if (!q.valid) return;
    if (q.symbol != stock_store::CurrentSymbol()) return;
    s_quote = q;
    s_quote_valid = true;

    // 底部状态栏：8 个 cell，价格类相对昨收着色。
    auto set_cell = [](int i, const char* txt, lv_color_t c) {
        if (s_stat_val[i]) {
            lv_label_set_text(s_stat_val[i], txt);
            lv_obj_set_style_text_color(s_stat_val[i], c, LV_PART_MAIN);
        }
    };
    char b[24];
    float ref = q.last_close;
    std::snprintf(b, sizeof(b), "%.2f", q.open);
    set_cell(0, b, stock_ui::TrendColor(q.open, ref));
    std::snprintf(b, sizeof(b), "%.2f", q.high);
    set_cell(1, b, stock_ui::TrendColor(q.high, ref));
    std::snprintf(b, sizeof(b), "%.2f", q.low);
    set_cell(2, b, stock_ui::TrendColor(q.low, ref));
    std::snprintf(b, sizeof(b), "%.2f", q.last_close);
    set_cell(3, b, stock_ui::Text());
    FormatCompact(q.volume, b, sizeof(b));
    set_cell(4, b, stock_ui::Text());
    FormatCompact(q.amount, b, sizeof(b));
    set_cell(5, b, stock_ui::Text());
    std::snprintf(b, sizeof(b), "%.2f%%", q.turnover_rate);
    set_cell(6, b, stock_ui::Text());
    std::snprintf(b, sizeof(b), "%.2f%%", q.amplitude);
    set_cell(7, b, stock_ui::Text());

    if (!s_hovering) ApplyLiveQuoteToTop();
}

void ApplyChart(const ChartSeries& chart, bool /*is_range*/) {
    if (chart.symbol != stock_store::CurrentSymbol() || chart.mode != s_mode) return;
    s_chart = chart;  // 值拷贝（供 hover 复用）
    s_chart_valid = chart.valid && chart.count > 0;
    stock_chart_renderer::Render(s_chart);
    if (IsKline()) {
        stock_kline_window::OnChartArrived(s_chart.count);
        stock_kline_window::Refresh(&s_chart, s_mode);
    } else {
        UpdateMinuteAxis();
    }
    UpdateChartStatus();
}

void SetNetworkReady(bool ready) {
    if (s_net_ready == ready) return;
    s_net_ready = ready;
    UpdateChartStatus();
}

void ApplyMarketStat(const MarketStatus& stat) { s_market = stat; }

void Reset() {
    if (s_canvas_buf != nullptr) {
        heap_caps_free(s_canvas_buf);
        s_canvas_buf = nullptr;
    }
    s_root = nullptr;
    s_canvas = nullptr;
    s_chart_valid = false;
    s_quote_valid = false;
    s_hovering = false;
    s_pinch_active = false;
    s_chart_status = nullptr;
    s_net_ready = true;
    for (int i = 0; i < CHART_MODE_COUNT; i++) s_tab_btn[i] = nullptr;
    for (int i = 0; i < 8; i++) s_stat_val[i] = nullptr;
}

}  // namespace stock_detail_view
