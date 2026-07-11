#pragma once

#include <driver/i2c_master.h>

// lib 内部跨模块接口（不对 main 暴露）。
namespace mhal::internal {

// 板载 I2C1 主总线（GPIO 7/8）。hal.cc 的 Init 建立后各模块共用；
// 摄像头时代的教训：同一物理引脚绝不允许第二个 i2c 控制器实例。
i2c_master_bus_handle_t I2cBus();

// display_hal.cc — 按序调用：面板上电初始化 → (100ms) → 触摸 → LVGL adapter
void InitPanel();
void InitTouch();
void StartLvglAdapter();

// power.cc
void BatteryBootGuard();            // 开机 0% 且未充电 → 强制关机
void StartWirelessChargeMonitor();  // 0x60 无线充芯片探测/配置任务

// bt_module.cc — UART2 初始化 + 常驻 RX 解析注册（可选发默认模式）
void InitBtModule(bool apply_default_mode);

}  // namespace mhal::internal
