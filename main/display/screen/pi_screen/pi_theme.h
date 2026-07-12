#pragma once

#include <cstdint>

#include "lvgl.h"

// ---------------------------------------------------------------------------
// pi_theme -- pi UI 的双主题令牌表（P2）。
//
// 深色 Amber Glow（现役）与浅色 Paper Ink 两套 Palette；所有静态配色统一
// 经共享 lv_style_t（按"属性 x 令牌"惰性建表）落到控件上，Set() 切换时改
// style 值 + lv_obj_report_style_change(nullptr) 即全 UI（含 chat 历史）
// 即时换装，无需销毁重建。动态配色点位（recolor 内嵌 hex、lv_color_mix
// 等）经 AddListener 注册的回调在切换时自行重涂。
//
// 平台无关：仅依赖 lvgl + settings.h（sim 侧有 shim）。持久化 NVS
// "ui"/"theme"（0=深色 1=浅色，与设置页既有键一致）；Init() 必须在构建任
// 何控件之前调用，保证开机即是持久化主题、不闪切。
// ---------------------------------------------------------------------------
namespace pi_theme {

struct Palette {
    lv_color_t bg, card, line, line2, tx, dim, faint, accent, accent_dim, ok, err;
};

// 共享样式令牌。Card2 是派生令牌（轨道底/按压底，深色沿用现役 0x181510，
// 浅色取 card 与 line 的中值），不在 Palette 结构里。
enum class Tok : uint8_t {
    Bg,
    Card,
    Card2,
    Line,
    Line2,
    Tx,
    Dim,
    Faint,
    Accent,
    AccentDim,
    Ok,
    Err,
    kCount
};

// 读 NVS "ui"/"theme" 定初始主题。在 PiScreen::Create 构建控件前调用。
void Init();

const Palette& Get();                  // 当前主题的 Palette
const Palette& PaletteOf(bool light);  // 指定主题的 Palette（主题预览卡用）
bool IsLight();

// 切换主题：写 NVS + 刷新全部共享样式（lv_obj_report_style_change）+
// 依次通知监听者。LVGL 线程调用。
void Set(bool light);

// 动态配色点位的"重涂自己"回调。返回 id 供 RemoveListener。
int AddListener(void (*cb)());
void RemoveListener(int id);

lv_color_t Color(Tok t);  // 当前主题下令牌的即时颜色（lv_color_mix 等一次性用途）
uint32_t Hex(Tok t);      // 0xRRGGBB（label recolor 内嵌 "#RRGGBB " 标记用）

// 共享样式挂载器：先摘掉同属性下其它 pi_theme 样式再挂新令牌，因此也可
// 用于运行期角色切换（如工具卡状态点 Accent -> Ok），历史控件随主题联动。
void ApplyBg(lv_obj_t* obj, Tok t, lv_style_selector_t sel = LV_PART_MAIN);
void ApplyText(lv_obj_t* obj, Tok t, lv_style_selector_t sel = LV_PART_MAIN);
void ApplyBorder(lv_obj_t* obj, Tok t, lv_style_selector_t sel = LV_PART_MAIN);

// 遮罩样式（bg 色 + 透明度一体）：深色 = 纯黑 60%，浅色 = 深墨 45%。
void ApplyScrim(lv_obj_t* obj);

}  // namespace pi_theme
