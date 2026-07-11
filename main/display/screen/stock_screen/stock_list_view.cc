// stock_list_view.cc — 见头文件说明。

#include "stock_list_view.h"

#include "screen_util.h"
#include "stock_store.h"
#include "stock_ui_theme.h"

#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace stock_list_view {
namespace {

constexpr char TAG[] = "stock_ui";

constexpr int32_t kPanelW = 720;
constexpr int32_t kTopBarH = 88;
constexpr int32_t kRowW = 688;
constexpr int32_t kRowH = 100;
constexpr int32_t kSparkX = 300;
constexpr int32_t kSparkY = 18;
constexpr int32_t kSparkW = 200;
constexpr int32_t kSparkH = 64;
constexpr size_t kSparkMaxPts = 60;

struct Row {
    lv_obj_t* card = nullptr;
    lv_obj_t* overlay = nullptr;  // 闪烁层
    lv_obj_t* name = nullptr;
    lv_obj_t* code = nullptr;
    lv_obj_t* spark = nullptr;    // lv_line
    lv_obj_t* price = nullptr;
    lv_obj_t* percent = nullptr;
    std::string symbol;
    float prev_price = 0;
    lv_point_precise_t spark_pts[kSparkMaxPts];
    uint8_t spark_n = 0;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_list = nullptr;
lv_obj_t* s_banner = nullptr;
lv_obj_t* s_market_chip[4] = {};  // SH SZ HK US
Row s_rows[stock_store::kMaxStocks];
size_t s_row_count = 0;
Callbacks s_cb;

void FlashExecCb(void* var, int32_t v) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), 0);
}

void Flash(Row& row) {
    if (row.overlay == nullptr) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, row.overlay);
    lv_anim_set_exec_cb(&a, FlashExecCb);
    lv_anim_set_values(&a, 90, 0);
    lv_anim_set_duration(&a, 400);
    lv_anim_start(&a);
}

void RowClickedCb(lv_event_t* e) {
    lv_obj_t* card = lv_event_get_current_target_obj(e);
    size_t idx = reinterpret_cast<size_t>(lv_obj_get_user_data(card));
    if (s_cb.on_row_click) s_cb.on_row_click(idx);
}

void BackCb(lv_event_t*) {
    if (s_cb.on_back) s_cb.on_back();
}
void GearCb(lv_event_t*) {
    if (s_cb.on_gear) s_cb.on_gear();
}

lv_obj_t* MakeChip(lv_obj_t* parent, const char* txt) {
    lv_obj_t* chip = lv_label_create(parent);
    lv_label_set_text(chip, txt);
    lv_obj_set_style_text_font(chip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(chip, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(chip, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, 4, LV_PART_MAIN);
    return chip;
}

void BuildTopBar(lv_obj_t* root) {
    lv_obj_t* bar = lv_obj_create(root);
    screen_strip_obj_chrome(bar);
    lv_obj_set_size(bar, kPanelW, kTopBarH);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 返回按钮
    lv_obj_t* back = lv_button_create(bar);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 56, 56);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_event_cb(back, BackCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);

    // 标题
    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, "股票");
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 84, 0);

    // 市场状态 chips（居中）
    static const char* kNames[4] = {"沪", "深", "港", "美"};
    lv_obj_t* group = lv_obj_create(bar);
    screen_strip_obj_chrome(group);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(group, 200, 40);
    lv_obj_align(group, LV_ALIGN_CENTER, 40, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(group, 6, LV_PART_MAIN);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) s_market_chip[i] = MakeChip(group, kNames[i]);

    // 设置齿轮（文字按钮）
    lv_obj_t* gear = lv_button_create(bar);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 64, 56);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(gear, GearCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(gear, true);
    lv_obj_t* gear_lbl = lv_label_create(gear);
    lv_label_set_text(gear_lbl, "设置");
    lv_obj_set_style_text_font(gear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(gear_lbl, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_center(gear_lbl);
}

void BuildRow(size_t idx) {
    Row& r = s_rows[idx];
    r = Row{};
    r.symbol = stock_store::SymbolAt(idx);

    lv_obj_t* card = lv_obj_create(s_list);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kRowW, kRowH);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, stock_ui::Card(), LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, reinterpret_cast<void*>(idx));
    lv_obj_add_event_cb(card, RowClickedCb, LV_EVENT_CLICKED, nullptr);
    r.card = card;

    // 闪烁层
    lv_obj_t* overlay = lv_obj_create(card);
    screen_strip_obj_chrome(overlay);
    lv_obj_set_size(overlay, kRowW, kRowH);
    lv_obj_center(overlay);
    lv_obj_set_style_radius(overlay, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    r.overlay = overlay;

    // 名称 + 代码
    lv_obj_t* name = lv_label_create(card);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 250);
    lv_label_set_text(name, stock_store::NameAt(idx).empty()
                                ? stock_store::SymbolAt(idx).c_str()
                                : stock_store::NameAt(idx).c_str());
    lv_obj_set_style_text_font(name, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 20, 16);
    r.name = name;

    lv_obj_t* code = lv_label_create(card);
    lv_label_set_text(code, r.symbol.c_str());
    lv_obj_set_style_text_font(code, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(code, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_align(code, LV_ALIGN_TOP_LEFT, 20, 58);
    r.code = code;

    // 迷你分时
    lv_obj_t* spark = lv_line_create(card);
    lv_obj_set_pos(spark, kSparkX, kSparkY);
    lv_obj_set_size(spark, kSparkW, kSparkH);
    lv_obj_set_style_line_width(spark, 2, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(spark, true, LV_PART_MAIN);
    lv_obj_set_style_line_color(spark, stock_ui::Dim(), LV_PART_MAIN);
    r.spark = spark;

    // 价格 + 涨跌幅
    lv_obj_t* price = lv_label_create(card);
    lv_label_set_text(price, "--");
    lv_obj_set_style_text_font(price, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(price, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_align(price, LV_ALIGN_TOP_RIGHT, -20, 18);
    r.price = price;

    lv_obj_t* percent = lv_label_create(card);
    lv_label_set_text(percent, "");
    lv_obj_set_style_text_font(percent, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(percent, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_align(percent, LV_ALIGN_TOP_RIGHT, -20, 58);
    r.percent = percent;
}

int FindRow(const char* symbol) {
    if (symbol == nullptr) return -1;
    for (size_t i = 0; i < s_row_count; i++) {
        if (s_rows[i].symbol == symbol) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb) {
    s_cb = cb;
    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_set_size(s_root, kPanelW, kPanelW);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    BuildTopBar(s_root);

    s_list = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_list);
    lv_obj_set_size(s_list, kPanelW, kPanelW - kTopBarH);
    lv_obj_set_pos(s_list, 0, kTopBarH);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_style_pad_top(s_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);

    // 中央横幅（连接网络中…）
    s_banner = lv_label_create(s_root);
    lv_label_set_text(s_banner, "");
    lv_obj_set_style_text_font(s_banner, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_banner, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x1E2530), LV_PART_MAIN);
    lv_obj_set_style_radius(s_banner, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_banner, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_banner, 10, LV_PART_MAIN);
    lv_obj_align(s_banner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    Rebuild();
    return s_root;
}

void SetBanner(const char* text) {
    if (s_banner == nullptr) return;
    if (text == nullptr || text[0] == '\0') {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_banner, text);
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_banner);
    }
}

void Rebuild() {
    if (s_list == nullptr) return;
    lv_obj_clean(s_list);
    s_row_count = stock_store::Count();
    if (s_row_count > stock_store::kMaxStocks) s_row_count = stock_store::kMaxStocks;
    for (size_t i = 0; i < s_row_count; i++) BuildRow(i);
    ESP_LOGI(TAG, "list rebuilt: %u rows", (unsigned)s_row_count);
}

void ApplyQuote(size_t idx, const StockQuote& q) {
    if (idx >= s_row_count) return;
    Row& r = s_rows[idx];
    if (!q.valid) return;

    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f", q.current);
    lv_label_set_text(r.price, buf);
    std::snprintf(buf, sizeof(buf), "%+.2f%%", q.percent);
    lv_label_set_text(r.percent, buf);

    lv_color_t c = stock_ui::PctColor(q.percent);
    lv_obj_set_style_text_color(r.price, c, LV_PART_MAIN);
    lv_obj_set_style_text_color(r.percent, c, LV_PART_MAIN);

    if (r.prev_price > 0 && q.current != r.prev_price) Flash(r);
    r.prev_price = q.current;
}

void ApplySpark(const char* symbol, const ChartSeries& chart) {
    int idx = FindRow(symbol);
    if (idx < 0 || chart.count < 2) return;
    Row& r = s_rows[idx];

    // 下采样到 kSparkMaxPts
    size_t n = chart.count;
    size_t step = (n + kSparkMaxPts - 1) / kSparkMaxPts;
    if (step < 1) step = 1;

    float vmin = chart.points[0], vmax = chart.points[0];
    for (size_t i = 0; i < n; i += step) {
        if (chart.points[i] < vmin) vmin = chart.points[i];
        if (chart.points[i] > vmax) vmax = chart.points[i];
    }
    float range = vmax - vmin;
    if (!(range > 0)) range = 1;

    uint8_t m = 0;
    for (size_t i = 0; i < n && m < kSparkMaxPts; i += step) {
        float t = (chart.points[i] - vmin) / range;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        r.spark_pts[m].y = static_cast<lv_value_precise_t>((1.0f - t) * (kSparkH - 1));
        m++;
    }
    // 按真实点数 m 均匀铺开 x
    for (uint8_t i = 0; i < m; i++) {
        r.spark_pts[i].x = static_cast<lv_value_precise_t>(
            (m <= 1) ? 0 : static_cast<int>((float)i / (float)(m - 1) * (kSparkW - 1)));
    }
    r.spark_n = m;
    lv_line_set_points(r.spark, r.spark_pts, m);

    // 颜色：末点 vs 昨收
    float last = chart.points[n - 1];
    lv_color_t c = (chart.has_ref && chart.last_close > 0)
                       ? stock_ui::TrendColor(last, chart.last_close)
                       : stock_ui::TrendColor(last, chart.points[0]);
    lv_obj_set_style_line_color(r.spark, c, LV_PART_MAIN);
}

void ApplyMarketStat(const MarketStatus& stat) {
    const bool open[4] = {stat.sh_open, stat.sz_open, stat.hk_open, stat.us_open};
    for (int i = 0; i < 4; i++) {
        if (s_market_chip[i] == nullptr) continue;
        lv_color_t bg = open[i] ? lv_color_hex(0x1F6F52) : lv_color_hex(0x394150);
        lv_color_t fg = open[i] ? lv_color_hex(0xE6E6E6) : stock_ui::Dim();
        lv_obj_set_style_bg_color(s_market_chip[i], bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_market_chip[i], fg, LV_PART_MAIN);
    }
}

void Reset() {
    s_root = nullptr;
    s_list = nullptr;
    s_banner = nullptr;
    for (int i = 0; i < 4; i++) s_market_chip[i] = nullptr;
    for (size_t i = 0; i < stock_store::kMaxStocks; i++) s_rows[i] = Row{};
    s_row_count = 0;
}

}  // namespace stock_list_view
