#include "pi_card_icons.h"

#include <cstring>

#include "screen_util.h"

namespace pi_card {

using pi_theme::Tok;

namespace {

// 图标容器：透明、不滚动、不吃点击、内部绝对定位（子件用 align + IGNORE_LAYOUT）。
lv_obj_t* Box(lv_obj_t* parent, int32_t size) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    return o;
}

// 绝对定位的实心条：中心置于 box 中心 + (dx,dy)，绕自身中心旋转 deg 度，圆角端。
lv_obj_t* Bar(lv_obj_t* box, int32_t w, int32_t h, Tok tone, int32_t dx, int32_t dy, int32_t deg) {
    lv_obj_t* o = lv_obj_create(box);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, (w < h ? w : h) / 2, LV_PART_MAIN);
    pi_theme::ApplyBg(o, tone);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    if (deg != 0) {
        lv_obj_set_style_transform_pivot_x(o, w / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(o, h / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(o, deg * 10, LV_PART_MAIN);  // 单位 0.1°
    }
    lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
    return o;
}

lv_obj_t* Circle(lv_obj_t* box, int32_t d, Tok tone, int32_t dx, int32_t dy) {
    lv_obj_t* o = Bar(box, d, d, tone, dx, dy, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    return o;
}

lv_obj_t* Ring(lv_obj_t* box, int32_t d, Tok tone, int32_t bw, int32_t dx, int32_t dy) {
    lv_obj_t* o = lv_obj_create(box);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, bw, LV_PART_MAIN);
    pi_theme::ApplyBorder(o, tone);
    lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
    return o;
}

// 空心圆角矩形描边（电池外壳等）。
lv_obj_t* RoundRectOutline(lv_obj_t* box, int32_t w, int32_t h, Tok tone, int32_t bw, int32_t radius,
                           int32_t dx, int32_t dy) {
    lv_obj_t* o = lv_obj_create(box);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, bw, LV_PART_MAIN);
    pi_theme::ApplyBorder(o, tone);
    lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
    return o;
}

// lv_arc 画的一段弧（wifi 信号波用）：只留背景弧、value=0 无 indicator、knob 隐形、
// 不可交互。arc_color 是一次性取色（tone 令牌），主题切换由卡片整体重建覆盖即可。
lv_obj_t* ArcSeg(lv_obj_t* box, int32_t d, Tok tone, int32_t bw, int32_t start_deg, int32_t end_deg,
                 int32_t dx, int32_t dy) {
    lv_obj_t* a = lv_arc_create(box);
    lv_obj_add_flag(a, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(a, d, d);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, start_deg, end_deg);
    lv_arc_set_value(a, 0);  // 无 indicator 弧
    lv_obj_set_style_arc_width(a, bw, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, pi_theme::Color(tone), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);  // knob 隐形
    lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
    lv_obj_align(a, LV_ALIGN_CENTER, dx, dy);
    return a;
}

bool Eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

}  // namespace

lv_obj_t* MakeIcon(lv_obj_t* parent, const char* name, int32_t size, Tok tone) {
    lv_obj_t* box = Box(parent, size);
    if (name == nullptr) name = "dot";
    const int32_t s = size;
    const int32_t stroke = s >= 28 ? 3 : 2;  // 线条粗细随尺寸

    if (Eq(name, "check") || Eq(name, "ok")) {
        // 对勾：短臂 + 长臂，绕各自中心旋转拼成 ✓，整体略微左移下沉更居中。
        Bar(box, stroke, s * 4 / 10, tone, -s * 3 / 20, s * 3 / 20, -45);
        Bar(box, stroke, s * 7 / 10, tone, s * 3 / 20, 0, 45);
    } else if (Eq(name, "close") || Eq(name, "x")) {
        Bar(box, stroke, s * 7 / 10, tone, 0, 0, 45);
        Bar(box, stroke, s * 7 / 10, tone, 0, 0, -45);
    } else if (Eq(name, "plus") || Eq(name, "add")) {
        Bar(box, s * 6 / 10, stroke, tone, 0, 0, 0);
        Bar(box, stroke, s * 6 / 10, tone, 0, 0, 0);
    } else if (Eq(name, "minus")) {
        Bar(box, s * 6 / 10, stroke, tone, 0, 0, 0);
    } else if (Eq(name, "chevron") || Eq(name, "arrow") || Eq(name, "next")) {
        // ">"：两条短臂夹角。
        Bar(box, stroke, s * 45 / 100, tone, -s / 20, -s * 3 / 20, 45);
        Bar(box, stroke, s * 45 / 100, tone, -s / 20, s * 3 / 20, -45);
    } else if (Eq(name, "dot")) {
        Circle(box, s * 45 / 100, tone, 0, 0);
    } else if (Eq(name, "gear") || Eq(name, "settings")) {
        Ring(box, s * 62 / 100, tone, stroke, 0, 0);
        Circle(box, s * 22 / 100, tone, 0, 0);
    } else if (Eq(name, "info")) {
        Ring(box, s * 8 / 10, tone, stroke, 0, 0);
        Circle(box, stroke + 1, tone, 0, -s * 2 / 10);
        Bar(box, stroke, s * 3 / 10, tone, 0, s / 20, 0);
    } else if (Eq(name, "warning") || Eq(name, "alert")) {
        Ring(box, s * 8 / 10, tone, stroke, 0, 0);
        Bar(box, stroke, s * 3 / 10, tone, 0, -s / 20, 0);
        Circle(box, stroke + 1, tone, 0, s * 2 / 10);
    } else if (Eq(name, "battery") || Eq(name, "charging") || Eq(name, "bolt")) {
        // 电池外壳 + 正极凸点 + 内部电量条；charging/bolt 额外一根强调竖条。
        int32_t bw = s * 68 / 100, bh = s * 42 / 100;
        RoundRectOutline(box, bw, bh, tone, stroke, s / 10, -s / 20, 0);
        Bar(box, stroke, bh / 2, tone, bw / 2 - s / 20 + stroke, 0, 0);  // 正极
        bool charging = !Eq(name, "battery");
        Bar(box, bw * 5 / 10, bh - 2 * stroke - 2, charging ? Tok::Accent : tone,
            -bw * 12 / 100, 0, 0);
        if (charging) Bar(box, stroke, bh + 2, Tok::Accent, 0, 0, 20);  // 闪电近似
    } else if (Eq(name, "wifi") || Eq(name, "signal")) {
        // 三段同心弧 + 底部原点。
        ArcSeg(box, s * 42 / 100, tone, stroke, 220, 320, 0, s * 12 / 100);
        ArcSeg(box, s * 68 / 100, tone, stroke, 220, 320, 0, s * 12 / 100);
        ArcSeg(box, s * 94 / 100, tone, stroke, 220, 320, 0, s * 12 / 100);
        Circle(box, stroke + 2, tone, 0, s * 22 / 100);
    } else if (Eq(name, "cellular")) {
        // 4G 信号：三格递增柱。
        Bar(box, s * 14 / 100, s * 25 / 100, tone, -s * 22 / 100, s * 15 / 100, 0);
        Bar(box, s * 14 / 100, s * 40 / 100, tone, 0, s * 8 / 100, 0);
        Bar(box, s * 14 / 100, s * 55 / 100, tone, s * 22 / 100, 0, 0);
    } else if (Eq(name, "sun") || Eq(name, "brightness")) {
        Circle(box, s * 34 / 100, tone, 0, 0);
        Bar(box, stroke, s * 18 / 100, tone, 0, -s * 38 / 100, 0);   // 上
        Bar(box, stroke, s * 18 / 100, tone, 0, s * 38 / 100, 0);    // 下
        Bar(box, s * 18 / 100, stroke, tone, -s * 38 / 100, 0, 0);   // 左
        Bar(box, s * 18 / 100, stroke, tone, s * 38 / 100, 0, 0);    // 右
        Bar(box, stroke, s * 16 / 100, tone, -s * 27 / 100, -s * 27 / 100, 45);
        Bar(box, stroke, s * 16 / 100, tone, s * 27 / 100, -s * 27 / 100, -45);
        Bar(box, stroke, s * 16 / 100, tone, -s * 27 / 100, s * 27 / 100, -45);
        Bar(box, stroke, s * 16 / 100, tone, s * 27 / 100, s * 27 / 100, 45);
    } else if (Eq(name, "volume") || Eq(name, "volume_high") || Eq(name, "volume_low") ||
               Eq(name, "mute") || Eq(name, "music") || Eq(name, "mic")) {
        // 喇叭：方形箱体 + 两段音波弧（mute 用一根斜杠替代波）。
        Bar(box, s * 26 / 100, s * 40 / 100, tone, -s * 22 / 100, 0, 0);
        bool low = Eq(name, "volume_low");
        bool mute = Eq(name, "mute");
        if (!mute) {
            ArcSeg(box, s * 42 / 100, tone, stroke, 300, 60, s * 2 / 100, 0);
            if (!low) ArcSeg(box, s * 72 / 100, tone, stroke, 300, 60, s * 6 / 100, 0);
        } else {
            Bar(box, stroke, s * 5 / 10, Tok::Err, s * 12 / 100, 0, 45);
        }
    } else if (Eq(name, "clock")) {
        Ring(box, s * 8 / 10, tone, stroke, 0, 0);
        Bar(box, stroke, s * 26 / 100, tone, 0, -s * 8 / 100, 0);
        Bar(box, s * 20 / 100, stroke, tone, s * 6 / 100, 0, 0);
    } else {
        Circle(box, s * 45 / 100, tone, 0, 0);  // 未知：回落成圆点
    }
    return box;
}

}  // namespace pi_card
