// stock_chart_renderer.cc — 见头文件。LVGL 9 全 canvas layer draw（折线 + 蜡烛）。

#include "stock_chart_renderer.h"

#include "pi_theme.h"
#include "stock/chart_math.h"
#include "stock/market_session.h"

#include <cstdio>

namespace stock_chart_renderer {
namespace {

using pi_theme::Tok;

// 柱/折线渲染阈值（Claw4 chart_window.h 同款）：≤60 根走蜡烛，>60 根退化折线。
constexpr size_t kBarRenderThreshold = 60;

lv_color_t BgColor() { return pi_theme::Color(Tok::Card2); }
lv_color_t DimColor() { return pi_theme::Color(Tok::Dim); }

// X 轴总 slot：分时当日按市场完整交易时段分钟数留白；5日/K 线按实际点数。
int TotalSlots(const ChartSeries& s) {
    if (s.mode == CHART_MIN_1D && s.has_ref) {
        int session = marketSessionMinutes(s.symbol.c_str());
        return static_cast<int>(s.count) > session ? static_cast<int>(s.count) : session;
    }
    return static_cast<int>(s.count);
}

int XOverSlots(int i, int total, int w) {
    if (total <= 1) return 0;
    return static_cast<int>(static_cast<float>(i) / (total - 1) * (w - 1));
}

// 时间戳日期桶（秒 epoch → 天）。同一交易日落同一桶，跨隔夜必换桶。
uint32_t DayBucket(uint32_t ts) { return ts / 86400u; }

// 5 日分时里 i 与 i-1 是否跨了交易日（此处需断线并画竖分隔）。
bool IsDayBoundary(const ChartSeries& s, size_t i) {
    return s.mode == CHART_MIN_5D && i > 0 && DayBucket(s.timestamps_s[i]) != DayBucket(s.timestamps_s[i - 1]);
}

void DrawDaySeparator(lv_layer_t* layer, int x, int h) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = DimColor();
    d.width = 1;
    d.opa = LV_OPA_30;
    d.p1.x = x;
    d.p1.y = 0;
    d.p2.x = x;
    d.p2.y = h - 1;
    lv_draw_line(layer, &d);
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
void DrawBridged(lv_layer_t* layer, int x0, int y0, int x1, int y1, float a, float b, float ref, int y_ref) {
    bool a_up = a >= ref;
    bool b_up = b >= ref;
    lv_color_t up = lv_color_hex(kColorUp);
    lv_color_t down = lv_color_hex(kColorDown);
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
    lv_color_t color = lv_color_hex(g.up ? kColorUp : kColorDown);
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
    rd.bg_color = g.up ? BgColor() : color;
    lv_area_t a = {g.bodyX, g.bodyTop, g.bodyX + g.bodyW - 1, g.bodyTop + g.bodyH - 1};
    lv_draw_rect(layer, &rd, &a);
}

void SetLabel(lv_obj_t* lbl, const char* txt) {
    if (lbl) lv_label_set_text(lbl, txt);
}

void ClearLabels(const Target& t) {
    SetLabel(t.max_price, "");
    SetLabel(t.min_price, "");
    SetLabel(t.max_pct, "");
    SetLabel(t.min_pct, "");
}

}  // namespace

lv_color_t PctColor(float pct) {
    if (pct > 0.001f) return lv_color_hex(kColorUp);
    if (pct < -0.001f) return lv_color_hex(kColorDown);
    return DimColor();
}

void Clear(const Target& t) {
    if (!t.canvas) return;
    lv_canvas_fill_bg(t.canvas, BgColor(), LV_OPA_COVER);
    lv_obj_invalidate(t.canvas);
    ClearLabels(t);
}

void Render(const Target& t, const ChartSeries& s) {
    if (!t.canvas || t.w <= 0 || t.h <= 0) return;
    lv_canvas_fill_bg(t.canvas, BgColor(), LV_OPA_COVER);
    if (!s.valid || s.count == 0) {
        ClearLabels(t);
        lv_obj_invalidate(t.canvas);
        return;
    }

    lv_layer_t layer;
    lv_canvas_init_layer(t.canvas, &layer);
    char buf[16];

    if (s.has_ref && s.last_close > 0) {
        // 分时 / 5 日
        MinuteYRange R = computeMinuteYRange(s.points, s.count, s.last_close);
        int y_ref = priceToChartY(s.last_close, R.yMin, R.yMax, t.h);
        // 昨收虚线
        lv_draw_line_dsc_t dref;
        lv_draw_line_dsc_init(&dref);
        dref.color = DimColor();
        dref.width = 1;
        dref.opa = LV_OPA_60;
        dref.dash_width = 6;
        dref.dash_gap = 5;
        dref.p1.x = 0;
        dref.p1.y = y_ref;
        dref.p2.x = t.w - 1;
        dref.p2.y = y_ref;
        lv_draw_line(&layer, &dref);

        int total = TotalSlots(s);
        for (size_t i = 1; i < s.count; i++) {
            int x0 = XOverSlots(static_cast<int>(i - 1), total, t.w);
            int x1 = XOverSlots(static_cast<int>(i), total, t.w);
            // 5 日：跨交易日不连线，改画淡竖分隔（每天独立成列）。
            if (IsDayBoundary(s, i)) {
                DrawDaySeparator(&layer, (x0 + x1) / 2, t.h);
                continue;
            }
            int y0 = priceToChartY(s.points[i - 1], R.yMin, R.yMax, t.h);
            int y1 = priceToChartY(s.points[i], R.yMin, R.yMax, t.h);
            DrawBridged(&layer, x0, y0, x1, y1, s.points[i - 1], s.points[i], s.last_close, y_ref);
        }

        std::snprintf(buf, sizeof(buf), "%.2f", R.yMax);
        SetLabel(t.max_price, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", R.yMin);
        SetLabel(t.min_price, buf);
        float pct_hi = (R.yMax - s.last_close) / s.last_close * 100.0f;
        float pct_lo = (R.yMin - s.last_close) / s.last_close * 100.0f;
        std::snprintf(buf, sizeof(buf), "%+.2f%%", pct_hi);
        SetLabel(t.max_pct, buf);
        if (t.max_pct) lv_obj_set_style_text_color(t.max_pct, PctColor(pct_hi), LV_PART_MAIN);
        std::snprintf(buf, sizeof(buf), "%+.2f%%", pct_lo);
        SetLabel(t.min_pct, buf);
        if (t.min_pct) lv_obj_set_style_text_color(t.min_pct, PctColor(pct_lo), LV_PART_MAIN);
    } else {
        // 日 K / 周 K
        KlineYRange R = computeKlineYRange(s.lows, s.highs, s.count);
        if (s.count > 0 && s.count <= kBarRenderThreshold) {
            KlineLayout L = computeKlineLayout(s.count, t.w);
            for (size_t i = 0; i < s.count; i++) {
                CandleGeom g =
                    computeCandleGeom(s.opens[i], s.points[i], s.highs[i], s.lows[i], R.yMin, R.yMax, L, i, t.w, t.h);
                if (!g.valid || !g.inCanvas) continue;
                DrawCandle(&layer, g);
            }
        } else {
            bool up = s.points[s.count - 1] >= s.points[0];
            lv_color_t c = lv_color_hex(up ? kColorUp : kColorDown);
            int total = static_cast<int>(s.count);
            for (size_t i = 1; i < s.count; i++) {
                int x0 = XOverSlots(static_cast<int>(i - 1), total, t.w);
                int x1 = XOverSlots(static_cast<int>(i), total, t.w);
                int y0 = priceToChartY(s.points[i - 1], R.yMin, R.yMax, t.h);
                int y1 = priceToChartY(s.points[i], R.yMin, R.yMax, t.h);
                DrawSeg(&layer, x0, y0, x1, y1, c, 2);
            }
        }
        std::snprintf(buf, sizeof(buf), "%.2f", R.yMax);
        SetLabel(t.max_price, buf);
        std::snprintf(buf, sizeof(buf), "%.2f", R.yMin);
        SetLabel(t.min_price, buf);
        SetLabel(t.max_pct, "");
        SetLabel(t.min_pct, "");
    }

    lv_canvas_finish_layer(t.canvas, &layer);
    lv_obj_invalidate(t.canvas);
}

Hit HitTest(const ChartSeries& s, int w, int h, int rel_x) {
    Hit r;
    if (!s.valid || s.count == 0 || w <= 1 || h <= 0) return r;
    if (rel_x < 0) rel_x = 0;
    if (rel_x > w - 1) rel_x = w - 1;
    // 折线两种（分时/5日 与 >60 根退化折线）共用 XOverSlots 的逆映射：最近邻取整。
    auto line_idx = [&](int total) {
        if (total <= 1) return 0;
        int idx = (rel_x * (total - 1) + (w - 1) / 2) / (w - 1);
        if (idx >= static_cast<int>(s.count)) idx = static_cast<int>(s.count) - 1;
        return idx < 0 ? 0 : idx;
    };
    if (s.has_ref && s.last_close > 0) {  // 分时 / 5 日
        MinuteYRange R = computeMinuteYRange(s.points, s.count, s.last_close);
        int total = TotalSlots(s);
        int idx = line_idx(total);
        r.idx = static_cast<size_t>(idx);
        r.x = XOverSlots(idx, total, w);
        r.y = priceToChartY(s.points[idx], R.yMin, R.yMax, h);
        r.valid = true;
        return r;
    }
    KlineYRange R = computeKlineYRange(s.lows, s.highs, s.count);
    if (s.count <= kBarRenderThreshold) {  // 蜡烛：与渲染共用 KlineLayout，钉在影线 x 上
        KlineLayout L = computeKlineLayout(s.count, w);
        int idx = klineHoverIdx(rel_x, L, s.count);
        r.idx = static_cast<size_t>(idx);
        r.x = L.startWickX + idx * L.slotW;
        r.y = priceToChartY(s.points[idx], R.yMin, R.yMax, h);
    } else {  // >60 根退化折线
        int idx = line_idx(static_cast<int>(s.count));
        r.idx = static_cast<size_t>(idx);
        r.x = XOverSlots(idx, static_cast<int>(s.count), w);
        r.y = priceToChartY(s.points[idx], R.yMin, R.yMax, h);
    }
    r.valid = true;
    return r;
}

}  // namespace stock_chart_renderer
