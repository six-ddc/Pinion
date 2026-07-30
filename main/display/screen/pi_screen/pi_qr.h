// pi_qr.h — 二维码控件的薄封装（lv_qrcode，双端通用，header-only）。
//
// 两个用处：设置页「设备后台」显示后台地址、开机未配置引导页让用户扫码进配置页。
// 颜色**固定用浅色调色板**（深码点 + 浅底），不跟随主题——深色主题下反色的二维码
// 很多手机相机识不出来。

#pragma once

#include <cstring>
#include <string>

#include "lvgl.h"
#include "pi_theme.h"

namespace pi_qr {

// 内容上限：URL 类内容远低于此，超了 lv_qrcode 会拒绝编码。
inline constexpr size_t kMaxTextBytes = 256;

inline lv_obj_t* Make(lv_obj_t* parent, int32_t size) {
    lv_obj_t* qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, pi_theme::PaletteOf(true).tx);
    lv_qrcode_set_light_color(qr, pi_theme::PaletteOf(true).bg);
    lv_obj_set_style_border_width(qr, 0, LV_PART_MAIN);
    // 留白：紧贴容器边缘的二维码识别率明显下降。
    lv_obj_set_style_pad_all(qr, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(qr, pi_theme::PaletteOf(true).bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, LV_PART_MAIN);
    return qr;
}

// 更新内容；空串 / 超长 → 隐藏（调用方不必自己判空）。
inline void Update(lv_obj_t* qr, const std::string& text) {
    if (qr == nullptr) return;
    if (text.empty() || text.size() > kMaxTextBytes) {
        lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_qrcode_update(qr, text.c_str(), static_cast<uint32_t>(text.size()));
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace pi_qr
