// stock_chart_renderer.cc — 见头文件。LVGL 9 全 canvas layer draw（折线 + 蜡烛）。

#include "stock_chart_renderer.h"

#include "chart_math.h"
#include "chart_window.h"
#include "market_session.h"
#include "stock_ui_theme.h"

#include <cmath>
#include <cstdio>

namespace stock_chart_renderer {
namespace {

lv_obj_t* s_canvas = nullptr;
lv_obj_t* s_max_price = nullptr;
lv_obj_t* s_min_price = nullptr;
lv_obj_t* s_max_pct = nullptr;
lv_obj_t* s_min_pct = nullptr;

bool IsKline(ChartMode mode) { return mode == CHART_KLINE_D || mode == CHART_KLINE_W; }

// X 轴总 slot：分时当日按市场完整交易时段分钟数留白；5日/K 线按实际点数。
int TotalSlots(const ChartSeries& s) {
    if (s.mode == CHART_MIN_1D && s.has_ref) {
        int session = marketSessionMinutes(s.symbol.c_str());
        return static_cast<int>(s.count) > session ? static_cast<int>(s.count) : session;
    }
    return static_cast<int>(s.count);
}

int XOverSlots(int i, int total) {
    if (total <= 1) return 0;
    return static_cast<int>(static_cast<float>(i) / (total - 1) * (kChartW - 1));
}

void DrawSeg(lv_layer_t* layer, int x0, int y0, int x1, int y1, lv_color_t c, int w) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.opa = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x0;
    d.p1.y = y0;
    d.p2.x = x1;
    d.p2.y = y1;
    lv_draw_line(layer, &d);
}

// 跨昨收桥接：段两端在昨收异侧时按 ref 拆成两段分别着色。
void DrawBridged(lv_layer_t* layer, int x0, int y0, int x1, int y1, float a, float b,
                 float ref, int y_ref) {
    bool a_up = a >= ref;
    bool b_up = b >= ref;
    lv_color_t up = lv_color_hex(stock_ui::kColorUp);
    lv_color_t down = lv_color_hex(stock_ui::kColorDown);
    if (a_up == b_up) {
        DrawSeg(layer, x0, y0, x1, y1, a_up ? up : down, 2);
        return;
    }
    float f = (ref - a) / (b - a);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int xc = x0 + static_cast<int>((x1 - x0) * f);
    DrawSeg(layer, x0, y0, xc, y_ref, a_up ? up : down, 2);
    DrawSeg(layer, xc, y_ref, x1, y1, b_up ? up : down, 2);
}

void DrawCandle(lv_layer_t* layer, const CandleGeom& g) {
    lv_color_t color = lv_color_hex(g.up ? stock_ui::kColorUp : stock_ui::kColorDown);
    // 影线
    DrawSeg(layer, g.wickX, g.wickYTop, g.wickX, g.wickYBottom, color, 1);
    // 实体：阳线空心（bg 填内部擦掉穿过的影线，边框 up 色）/ 阴线实心
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 0;
    rd.border_color = color;
    rd.border_width = 1;
    rd.border_opa = LV_OPA_COVER;
    rd.bg_opa = LV_OPA_COVER;
    rd.bg_color = g.up ? lv_color_hex(stock_ui::kColorBg) : color;
    lv_area_t a = {g.bodyX, g.bodyTop, g.bodyX + g.bodyW - 1, g.bodyTop + g.bodyH - 1};
    lv_draw_rect(layer, &rd, &a);
}

void SetLabel(lv_obj_t* lbl, const char* txt) {
    if (lbl) lv_label_set_text(lbl, txt);
}

void ClearLabels() {
    SetLabel(s_max_price, "");
    SetLabel(s_min_price, "");
    SetLabel(s_max_pct, "");
    SetLabel(s_min_pct, "");
}

}  // namespace

void Attach(lv_obj_t* canvas, lv_obj_t* max_price, lv_obj_t* min_price, lv_obj_t* max_pct,
            lv_obj_t* min_pct) {
    s_canvas = canvas;
    s_max_price = max_price;
    s_min_price = min_price;
    s_max_pct = max_pct;
    s_min_pct = min_pct;
}

void Clear() {
    if (!s_canvas) return;
    lv_canvas_fill_bg(s_canvas, lv_color_hex(stock_ui::kColorBg), LV_OPA_COVER);
    lv_obj_invalidate(s_canvas);
    ClearLabels();
}

void Render(const ChartSeries& s) {
    if (!s_canvas) return;
    lv_canvas_fill_bg(s_canvas, lv_color_hex(stock_ui::kColorBg), LV_OPA_COVER);
    if (!s.valid || s.count == 0) {
        ClearLabels();
        lv_obj_invalidate(s_canvas);
        return;
    }

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    char buf[16];

    if (s.has_ref && s.last_close > 0) {
        // 分时 / 5 日
        MinuteYRange R = computeMinuteYRange(s.points, s.count, s.last_close);
        int y_ref = priceToChartY(s.last_close, R.yMin, R.yMax, kChartH);
        // 昨收虚线
        lv_draw_line_dsc_t dref;
        lv_draw_line_dsc_init(&dref);
        dref.color = lv_color_hex(stock_ui::kColorDim);
        dref.width = 1;
        dref.opa = LV_OPA_60;
        dref.dash_width = 6;
        dref.dash_gap = 5;
        dref.p1.x = 0;
        dref.p1.y = y_ref;
        dref.p2.x = kChartW - 1;
        dref.p2.y = y_ref;
        lv_draw_line(&layer, &dref);

        int total = TotalSlots(s);
        for (size_t i = 1; i < s.count; i++) {
            int x0 = XOverSlots(static_cast<int>(i - 1), total);
            int x1 = XOverSlots(static_cast<int>(i), total);
            int y0 = priceToChartY(s.points[i - 1], R.yMin, R.yMax, kChartH);
            int y1 = priceToChartY(s.points[i], R.yMin, R.yMax, kChartH);
            DrawBridged(&layer, x0, y0, x1, y1, s.points[i - 1], s.points[i], s.last_close,
                        y_ref);
        }

        std::snprintf(buf, sizeof(buf), "%.2f", R.yMax);
        SetLabel(s_max_price, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", R.yMin);
        SetLabel(s_min_price, buf);
        float pct_hi = (R.yMax - s.last_close) / s.last_close * 100.0f;
        float pct_lo = (R.yMin - s.last_close) / s.last_close * 100.0f;
        std::snprintf(buf, sizeof(buf), "%+.2f%%", pct_hi);
        SetLabel(s_max_pct, buf);
        if (s_max_pct)
            lv_obj_set_style_text_color(s_max_pct, stock_ui::PctColor(pct_hi), LV_PART_MAIN);
        std::snprintf(buf, sizeof(buf), "%+.2f%%", pct_lo);
        SetLabel(s_min_pct, buf);
        if (s_min_pct)
            lv_obj_set_style_text_color(s_min_pct, stock_ui::PctColor(pct_lo), LV_PART_MAIN);
    } else {
        // 日 K / 周 K
        KlineYRange R = computeKlineYRange(s.lows, s.highs, s.count);
        if (shouldUseBarsForWindow(s.count)) {
            KlineLayout L = computeKlineLayout(s.count);
            for (size_t i = 0; i < s.count; i++) {
                CandleGeom g = computeCandleGeom(s.opens[i], s.points[i], s.highs[i], s.lows[i],
                                                 R.yMin, R.yMax, L, i);
                if (!g.valid || !g.inCanvas) continue;
                DrawCandle(&layer, g);
            }
        } else {
            bool up = s.points[s.count - 1] >= s.points[0];
            lv_color_t c = lv_color_hex(up ? stock_ui::kColorUp : stock_ui::kColorDown);
            int total = static_cast<int>(s.count);
            for (size_t i = 1; i < s.count; i++) {
                int x0 = XOverSlots(static_cast<int>(i - 1), total);
                int x1 = XOverSlots(static_cast<int>(i), total);
                int y0 = priceToChartY(s.points[i - 1], R.yMin, R.yMax, kChartH);
                int y1 = priceToChartY(s.points[i], R.yMin, R.yMax, kChartH);
                DrawSeg(&layer, x0, y0, x1, y1, c, 2);
            }
        }
        std::snprintf(buf, sizeof(buf), "%.2f", R.yMax);
        SetLabel(s_max_price, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", R.yMin);
        SetLabel(s_min_price, buf);
        SetLabel(s_max_pct, "");
        SetLabel(s_min_pct, "");
    }

    lv_canvas_finish_layer(s_canvas, &layer);
    lv_obj_invalidate(s_canvas);
}

int XForIndex(size_t i, const ChartSeries& s) {
    if (IsKline(s.mode) && shouldUseBarsForWindow(s.count)) {
        KlineLayout L = computeKlineLayout(s.count);
        return L.startWickX + static_cast<int>(i) * L.slotW;
    }
    return XOverSlots(static_cast<int>(i), TotalSlots(s));
}

int HoverIndex(int rel_x, const ChartSeries& s) {
    if (s.count == 0) return 0;
    if (IsKline(s.mode) && shouldUseBarsForWindow(s.count)) {
        KlineLayout L = computeKlineLayout(s.count);
        return klineHoverIdx(rel_x, L, s.count);
    }
    int total = TotalSlots(s);
    int idx = (total <= 1) ? 0
                           : static_cast<int>(static_cast<float>(rel_x) / (kChartW - 1) *
                                                  (total - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(s.count)) idx = static_cast<int>(s.count) - 1;
    return idx;
}

}  // namespace stock_chart_renderer
