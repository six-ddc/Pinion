#pragma once

#include "esp_lcd_touch.h"
#include "lvgl.h"

// 芯片坐标 trace 日志：1 开启，0 关闭（默认）。
#ifndef TOUCH_FEED_DEBUG
#define TOUCH_FEED_DEBUG 0
#endif

// 单一读源：独立任务读 GT911，LVGL indev read_cb 只读 mutex 保护的快照。
void touch_feed_init(esp_lcd_touch_handle_t handle, uint32_t period_ms);
void touch_feed_attach_indev(lv_indev_t* indev);
void touch_feed_stop();

// 当前按下的手指数（0/1/2）。若 >=2 且指针非空，填入前两指的芯片坐标。
// 供多点手势（如 K 线双指缩放）使用；LVGL indev 仍只用单点。
uint8_t touch_feed_finger_count(int16_t* x0, int16_t* y0, int16_t* x1, int16_t* y1);
