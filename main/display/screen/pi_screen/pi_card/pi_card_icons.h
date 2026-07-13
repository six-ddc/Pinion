#pragma once

// ---------------------------------------------------------------------------
// pi_card::MakeIcon —— 形状拼合图标库
//
// Claw6 没有图标字体（mono 子集仅 ASCII+·°，无 font_awesome/LV_SYMBOL），沿用
// pi_screen 既有做法用小 lv_obj 形状（矩形/圆/环/旋转条/lv_arc）拼出图标。全部
// 走 pi_theme 共享样式（Tok），主题切换自动跟随。返回一个 size×size 的透明、
// 不可滚动、不吃点击的容器，可直接塞进 row/column。未知名字回落成一个圆点。
//
// 支持名字（含别名）：
//   volume / volume_high / volume_low / mute
//   sun / brightness · battery / charging / bolt · wifi / signal / cellular
//   check / ok · close / x · plus · minus · gear / settings
//   chevron / arrow · info · warning · dot · mic · music · clock
// ---------------------------------------------------------------------------

#include "lvgl.h"

#include "pi_theme.h"

namespace pi_card {

// size 建议 20~32。tone 决定图标主色（默认令牌由调用方给，通常 Tok::Tx/Accent）。
lv_obj_t* MakeIcon(lv_obj_t* parent, const char* name, int32_t size, pi_theme::Tok tone);

}  // namespace pi_card
