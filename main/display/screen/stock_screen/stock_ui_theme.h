// stock_ui_theme.h
// 股票 app 各视图共用的配色与字体（720x720 深色主题）。红涨绿跌（A 股惯例）。

#ifndef STOCK_UI_THEME_H
#define STOCK_UI_THEME_H

#include "lvgl.h"

#include <cstdint>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace stock_ui {

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorCard = 0x161B22;
constexpr uint32_t kColorText = 0xE6E6E6;
constexpr uint32_t kColorDim = 0x8B93A1;
constexpr uint32_t kColorUp = 0xFF3B30;    // 红涨
constexpr uint32_t kColorDown = 0x26C281;  // 绿跌
constexpr uint32_t kColorFlat = 0x8B8B8B;

inline lv_color_t Bg() { return lv_color_hex(kColorBg); }
inline lv_color_t Card() { return lv_color_hex(kColorCard); }
inline lv_color_t Text() { return lv_color_hex(kColorText); }
inline lv_color_t Dim() { return lv_color_hex(kColorDim); }

// 按涨跌幅（%）取色：>0 红，<0 绿，≈0 灰。
inline lv_color_t PctColor(float pct) {
    if (pct > 0.001f) return lv_color_hex(kColorUp);
    if (pct < -0.001f) return lv_color_hex(kColorDown);
    return lv_color_hex(kColorFlat);
}

// 按价格相对昨收取色（分时/spark 用）。
inline lv_color_t TrendColor(float value, float ref) {
    if (!(ref > 0)) return lv_color_hex(kColorFlat);
    if (value > ref) return lv_color_hex(kColorUp);
    if (value < ref) return lv_color_hex(kColorDown);
    return lv_color_hex(kColorFlat);
}

}  // namespace stock_ui

#endif  // STOCK_UI_THEME_H
