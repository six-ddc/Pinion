#pragma once

#include "lvgl.h"

// 屏幕操作门面。面板（MIPI-DSI 720x720 RGB888）、GT911 触摸与
// esp_lv_adapter（PPA 加速 + TRIPLE_FULL 防撕裂）都在 mhal::Init() 内
// 完成初始化；本头只暴露运行期需要的句柄与 LVGL 锁。
namespace mhal::display {

// Init() 之后有效；之前返回 nullptr。
lv_display_t* GetLvDisplay();

// esp_lv_adapter 的 LVGL 互斥锁。非 LVGL 任务上下文操作 widget 前必须持锁。
// timeout_ms < 0 表示无限等待。
bool Lock(int timeout_ms = -1);
void Unlock();

int Width();
int Height();

}  // namespace mhal::display
