// stock_chart_renderer.h
// 行情图 canvas 渲染（移植自 Claw4，去单例化 → Target 传参，供 pi_card 的
// stock_chart 控件复用；一屏可同时存在多张卡）。
// 分时/5日：双色折线（跨昨收桥接）+ 昨收虚线；日/周 K：≤60 根蜡烛（阳线空心/
// 阴线实心）、>60 根整段单色折线。坐标全走 chart_math.h（宽高按 Target 传入）。
// 背景/网格色取 pi_theme 令牌（双主题自适应）；红涨绿跌为固定语义色。

#ifndef STOCK_CHART_RENDERER_H
#define STOCK_CHART_RENDERER_H

#include "lvgl.h"
#include "stock/stock_models.h"

namespace stock_chart_renderer {

// 红涨绿跌语义色（双主题共用；也供控件头部涨跌幅 label 复用）。
constexpr uint32_t kColorUp = 0xFF3B30;
constexpr uint32_t kColorDown = 0x26C281;
lv_color_t PctColor(float pct);  // >0 红 / <0 绿 / ≈0 主题 Dim

// 渲染目标：canvas + 4 个可选浮动坐标 label（最高/最低价、最高/最低涨跌幅%，
// 后者仅分时模式填写）。w/h 是 canvas 像素尺寸。
struct Target {
    lv_obj_t* canvas = nullptr;
    lv_obj_t* max_price = nullptr;
    lv_obj_t* min_price = nullptr;
    lv_obj_t* max_pct = nullptr;
    lv_obj_t* min_pct = nullptr;
    int w = 0;
    int h = 0;
};

// 整段重绘（含背景清底）。
void Render(const Target& t, const ChartSeries& s);

// 清空（无数据/切换模式时）。
void Clear(const Target& t);

}  // namespace stock_chart_renderer

#endif  // STOCK_CHART_RENDERER_H
