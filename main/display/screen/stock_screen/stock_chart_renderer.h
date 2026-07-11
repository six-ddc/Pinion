// stock_chart_renderer.h
// 行情图 canvas 渲染（720x720 详情，全 canvas 自绘，不用 lv_chart）。
// 分时/5日：双色折线（跨昨收桥接）+ 昨收虚线；日/周 K：≤60 根蜡烛（阳线空心/
// 阴线实心）、>60 根整段单色折线。坐标全走 chart_math.h（kChartW/kChartH=688/420）。

#ifndef STOCK_CHART_RENDERER_H
#define STOCK_CHART_RENDERER_H

#include "lvgl.h"
#include "stock_models.h"

#include <cstddef>

namespace stock_chart_renderer {

// 绑定 canvas + 4 个浮动坐标 label（最高/最低价、最高/最低涨跌幅%，后者仅分时）。
void Attach(lv_obj_t* canvas, lv_obj_t* max_price, lv_obj_t* min_price,
            lv_obj_t* max_pct, lv_obj_t* min_pct);

// 整段重绘。
void Render(const ChartSeries& s);

// 清空（无数据/切换时）。
void Clear();

// hover：canvas 内相对 x → 数据索引；数据索引 → canvas 内 x（crosshair 吸附）。
int HoverIndex(int rel_x, const ChartSeries& s);
int XForIndex(size_t i, const ChartSeries& s);

}  // namespace stock_chart_renderer

#endif  // STOCK_CHART_RENDERER_H
