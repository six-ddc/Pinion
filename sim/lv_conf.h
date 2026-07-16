/* sim — host (SDL2) LVGL config for the pi_screen simulator.
 *
 * Only overrides; everything else falls back to lv_conf_internal.h defaults.
 * Mirrors the load-bearing sdkconfig flags: LV_USE_FONT_COMPRESSED (the three
 * pi_* fonts are RLE-compressed — without it the first label render crashes,
 * same as on device) and LV_FONT_FMT_TXT_LARGE (puhui glyph tables). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

#define LV_USE_OS LV_OS_PTHREAD

/* no 64K builtin pool on host — the SDL framebuffer alone is 720*720*4 */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL.h>

#define LV_USE_FONT_COMPRESSED 1
#define LV_FONT_FMT_TXT_LARGE 1

#define LV_USE_SNAPSHOT 1

/* pi_card v1 能力 3：qrcode 控件（依赖 canvas，两端均默认已开）。 */
#define LV_USE_QRCODE 1

/* pi_card Phase3：chart 控件（standby 常驻卡的 battery/rssi 历史折线，两端均默认已开）。 */
#define LV_USE_CHART 1

#endif /* LV_CONF_H */
