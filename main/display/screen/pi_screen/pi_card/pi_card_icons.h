#pragma once

// ---------------------------------------------------------------------------
// pi_card::MakeIcon —— Lucide 图标字体 + 形状拼合回落
//
// 首选 Lucide 图标字体子集（font_pi_icons_{16,22,28}，bpp4 RLE，与 puhui 同
// 管线）：名字查生成表 pi_card_icon_map.h（Lucide 官方 kebab-case 名 + 历史
// 别名，约 190 个字形），命中则 lv_label 渲染字形，ApplyText 令牌颜色自动跟随
// 主题。增删图标改 gen_pi_icons.py 的 ICONS 后重跑。
//
// 回落：名字不在表内、或 size<16（字形会被裁切）时，沿用旧的小 lv_obj 形状
// 拼合（矩形/圆/环/旋转条/lv_arc）；完全未知的名字画一个圆点。逐帧动画件
// （实时信号条、音频波形）不属于这里，各自就地手绘。
//
// 返回一个 size×size 的透明、不可滚动、不吃点击的容器，可直接塞进 row/column。
// 常用名（含别名）：wifi / signal(cellular) / bluetooth /
// battery[-low|-medium|-full|-charging] / volume-2(volume) / volume-x(mute) /
// mic / music / play / pause / skip-back / skip-forward / sun(brightness) /
// moon / sun-moon(theme) / check(ok) / x(close) / plus(add) / minus /
// chevron-left(back) / chevron-right(chevron,arrow,next) / settings(gear) /
// info / triangle-alert(warning) / power / folder(files) / menu / list /
// message-circle(chat) / clock / dot ……全集见 pi_card_icon_map.h。
// ---------------------------------------------------------------------------

#include "lvgl.h"

#include "pi_theme.h"

namespace pi_card {

// size 建议 20~32。tone 决定图标主色（默认令牌由调用方给，通常 Tok::Tx/Accent）。
lv_obj_t* MakeIcon(lv_obj_t* parent, const char* name, int32_t size, pi_theme::Tok tone);

}  // namespace pi_card
