#pragma once

#include <esp_err.h>

// metalio_hal — metalio-claw-4 (ESP32-P4, 720x720 MIPI-DSI) 硬件能力库。
//
// 一站式初始化 + 分能力门面：
//   mhal::Init()            硬件全量初始化（见 InitOptions 各开关）
//   metalio_hal/display.h   面板/触摸/LVGL（Init 内起显，此头拿句柄与锁）
//   metalio_hal/backlight.h 背光亮度
//   metalio_hal/network.h   Wi-Fi(ESP-Hosted C5) + 4G(NT26) 双网
//   metalio_hal/bluetooth.h BT 音频模组（UART AT 指令）
//   metalio_hal/audio.h     mic/speaker PCM 编解码
//   metalio_hal/power.h     电池电量计 / 强制关机
//   metalio_hal/sysmon.h    CPU/内存/电池周期监控
//   IOExpander.hpp          TCA9555 按键与电源轨直通 API
//   settings.h              NVS 通用包装
//
// lib 不引用任何 UI/screen/业务逻辑；需要通知上层的地方一律走注册回调。
namespace mhal {

struct InitOptions {
    // 挂载 SD 卡到 /sdcard（卡不在位只告警，不致命）。
    bool mount_sd_card = true;
    // 开机把 BT 音频模组切到默认模式1（AT+RX=2 → AT+MODE=1，接收模式）。
    bool bt_default_mode = true;
    // 开机电量保护：电量 0% 且未充电时发 PWR_KEY_PULSE 序列强制关机。
    bool battery_boot_guard = true;
    // 初始化完成后从 NVS 恢复背光亮度（渐变点亮）。
    bool restore_backlight = true;
};

// 硬件初始化总入口，按板级验证过的顺序执行：
//   I2C1 总线 → TCA9555 IOExpander(上电序列: BT/PA/4G/CAM/SD 电源轨)
//   → BQ27220 电量计 → 开机电量保护 → BT 模组 UART2 + 默认模式
//   → SD 卡挂载 → MIPI-DSI LCD(NV3051F/FL7707N) → GT911 触摸
//   → LVGL adapter 起显 → 无线充电流配置监控任务 → 背光恢复。
// 返回 ESP_OK 后 LVGL 已在跑，可用 display::Lock() 加载首屏。
// 网络不在此启动：单独调 network::Start()/StartAsync()（可能阻塞分钟级）。
esp_err_t Init(const InitOptions& opts = {});

}  // namespace mhal
