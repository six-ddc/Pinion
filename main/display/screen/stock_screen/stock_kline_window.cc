// stock_kline_window.cc — 见头文件。

#include "stock_kline_window.h"

#include "chart_window.h"
#include "stock_fetch_worker.h"

#include <cstdio>
#include <ctime>

namespace stock_kline_window {
namespace {

lv_obj_t* s_zoom_pill = nullptr;
lv_obj_t* s_x_left = nullptr;
lv_obj_t* s_x_right = nullptr;

int s_zoom_idx = 0;
size_t s_max_known_bars = 0;
bool s_zoom_at_edge = false;

bool IsKline(ChartMode mode) { return mode == CHART_KLINE_D || mode == CHART_KLINE_W; }

bool ChartInFlight() {
    return stock_fetch::InFlight(stock_fetch::FETCH_CHART) ||
           stock_fetch::InFlight(stock_fetch::FETCH_CHART_RANGE);
}

void FormatWindowDate(char* buf, size_t buf_size, time_t ts, int cur_year) {
    struct tm t = *localtime(&ts);
    int year = t.tm_year + 1900;
    if (year != cur_year) {
        std::snprintf(buf, buf_size, "%02d/%02d-%02d", year % 100, t.tm_mon + 1, t.tm_mday);
    } else {
        std::snprintf(buf, buf_size, "%02d-%02d", t.tm_mon + 1, t.tm_mday);
    }
}

void UpdateDateLabels(const ChartSeries* chart, ChartMode mode) {
    if (!s_x_left || !s_x_right || !IsKline(mode)) return;
    if (!chart || chart->count == 0) {
        lv_label_set_text(s_x_left, ChartInFlight() ? "..." : "--");
        lv_label_set_text(s_x_right, "");
        return;
    }
    // 用最新一根 K 线的时间戳所在年作为"当前年"参考（无系统时钟依赖）。
    int cur_year = 0;
    {
        time_t last = static_cast<time_t>(chart->timestamps_s[chart->count - 1]);
        struct tm tm_last = *localtime(&last);
        cur_year = tm_last.tm_year + 1900;
    }
    char l[40], r[40];
    FormatWindowDate(l, sizeof(l), static_cast<time_t>(chart->timestamps_s[0]), cur_year);
    FormatWindowDate(r, sizeof(r), static_cast<time_t>(chart->timestamps_s[chart->count - 1]),
                     cur_year);
    lv_label_set_text(s_x_left, l);
    lv_label_set_text(s_x_right, r);
}

void UpdateZoomPill(ChartMode mode) {
    if (!s_zoom_pill) return;
    // 只显示档位标签（1M/3M/…）。加载态由图表区「加载中…」提示，pill 不加省略号
    // ——回调早于 in-flight 清除，加了会永久卡住。
    lv_label_set_text(s_zoom_pill, zoomPresetLabel(s_zoom_idx, mode == CHART_KLINE_W));
}

}  // namespace

void Attach(lv_obj_t* zoom_pill, lv_obj_t* x_left, lv_obj_t* x_right) {
    s_zoom_pill = zoom_pill;
    s_x_left = x_left;
    s_x_right = x_right;
}

void EnterMode(ChartMode mode) {
    if (s_zoom_pill) lv_obj_remove_flag(s_zoom_pill, LV_OBJ_FLAG_HIDDEN);
    UpdateZoomPill(mode);
}

void LeaveMode() {
    if (s_zoom_pill) lv_obj_add_flag(s_zoom_pill, LV_OBJ_FLAG_HIDDEN);
}

void ResetState() {
    s_zoom_idx = 0;
    s_max_known_bars = 0;
    s_zoom_at_edge = false;
}

void OnChartArrived(size_t arrived_count) {
    if (arrived_count > s_max_known_bars) s_max_known_bars = arrived_count;
    uint16_t target = zoomPreset(s_zoom_idx).bars;
    if (arrived_count + 1 < target) s_zoom_at_edge = true;
    int actual_max = maxUsableZoomIdx(arrived_count, /*headAtEdge=*/true);
    if (s_zoom_idx > actual_max) s_zoom_idx = actual_max;
}

void Refresh(const ChartSeries* chart, ChartMode mode) {
    if (!IsKline(mode)) return;
    UpdateDateLabels(chart, mode);
    UpdateZoomPill(mode);
}

bool OnZoomCycle(const char* symbol, ChartMode mode) {
    if (!IsKline(mode) || ChartInFlight() || !symbol || !*symbol) return false;
    s_zoom_idx = nextZoomIdx(s_zoom_idx, s_max_known_bars, s_zoom_at_edge);
    uint16_t target = zoomPreset(s_zoom_idx).bars;
    if (stock_fetch::EnqueueChartRange(symbol, mode, "", target)) {
        UpdateZoomPill(mode);
        return true;
    }
    return false;
}

bool ZoomStep(const char* symbol, ChartMode mode, bool zoom_in) {
    if (!IsKline(mode) || ChartInFlight() || !symbol || !*symbol) return false;
    int max_idx = maxUsableZoomIdx(s_max_known_bars, s_zoom_at_edge);
    int next = s_zoom_idx + (zoom_in ? -1 : 1);
    if (next < 0) next = 0;
    if (next > max_idx) next = max_idx;
    if (next == s_zoom_idx) return false;  // 已到边界
    s_zoom_idx = next;
    uint16_t target = zoomPreset(s_zoom_idx).bars;
    if (stock_fetch::EnqueueChartRange(symbol, mode, "", target)) {
        UpdateZoomPill(mode);
        return true;
    }
    return false;
}

uint16_t CurrentZoomBars() { return zoomPreset(s_zoom_idx).bars; }

}  // namespace stock_kline_window
