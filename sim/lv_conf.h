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

/* 迟到属性修复取证（任务 #7 点 1）：真机 sdkconfig 开着 CONFIG_LV_USE_ASSERT_STYLE=y，sim
 * 之前没开，测不出"预览路径漏调 EnsureCardStyles() 导致对未初始化静态 lv_style_t 调
 * lv_obj_add_style"这类真机挂起风险——补开，sim 从此比真机断言配置更贴近，防以后再漏。
 * 全套回归干净通过后决定保留（见任务报告）。 */
#define LV_USE_ASSERT_STYLE 1

/* pi_card v1 能力 3：qrcode 控件（依赖 canvas，两端均默认已开）。 */
#define LV_USE_QRCODE 1

/* pi_card Phase3：chart 控件（standby 常驻卡的 battery/rssi 历史折线，两端均默认已开）。 */
#define LV_USE_CHART 1

/* 媒体封面解码（Stage E）：JPEG(tjpgd)/PNG(lodepng) + FS_MEMFS（tjpgd 从内存字节源
 * 解码要靠它把 lv_image_dsc_t 包成"文件"读，见 lv_tjpgd.c）。字母与设备端 sdkconfig
 * 的 CONFIG_LV_FS_MEMFS_LETTER=77（'M'）保持一致（非强制，两端各自独立取值也可，
 * 统一只是少一处心智负担）。 */
#define LV_USE_TJPGD 1
#define LV_USE_LODEPNG 1
#define LV_USE_FS_MEMFS 1
#define LV_FS_MEMFS_LETTER 'M'

#endif /* LV_CONF_H */
