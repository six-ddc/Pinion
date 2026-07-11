// stock_kline_window.h
// K 线缩放档位状态机。只用 chart_window.h 的档位状态机部分（nextZoomIdx/
// maxUsableZoomIdx）；增量合并逻辑已实现但保持不接线，
// 缩放 = 按新档 bars 整窗重拉、整段替换。

#ifndef STOCK_KLINE_WINDOW_H
#define STOCK_KLINE_WINDOW_H

#include "lvgl.h"
#include "stock_models.h"

#include <cstddef>
#include <cstdint>

namespace stock_kline_window {

// 绑定 UI 控件（zoomPill + 左右日期标签）。
void Attach(lv_obj_t* zoom_pill, lv_obj_t* x_left, lv_obj_t* x_right);

// 进入/离开 K 线模式（切 zoomPill 显隐）。
void EnterMode(ChartMode mode);
void LeaveMode();

// 切股/切模式时重置档位状态。
void ResetState();

// 数据到达后推进 maxKnownBars + 触底判定 + 回压档位。
void OnChartArrived(size_t arrived_count);

// 刷新 zoomPill 文本 + 窗口日期标签。
void Refresh(const ChartSeries* chart, ChartMode mode);

// 点击 zoomPill 循环下一档，向 worker 发 RANGE 请求。返回是否已发起。
bool OnZoomCycle(const char* symbol, ChartMode mode);

// 双指缩放：zoom_in=true 减少根数（放大细节），false 增加根数（缩小看更长）。
// 到边界或无变化返回 false。
bool ZoomStep(const char* symbol, ChartMode mode, bool zoom_in);

uint16_t CurrentZoomBars();

}  // namespace stock_kline_window

#endif  // STOCK_KLINE_WINDOW_H
