#pragma once

#include <cstdint>

// 背光亮度门面（GPIO52 LEDC PWM 25kHz，5ms 步进渐变）。
// 持久化沿用 NVS namespace "display" / key "brightness"。
namespace mhal::backlight {

// percent 0-100；persist=true 时写入 NVS（下次开机 Restore 生效）。
void SetBrightness(uint8_t percent, bool persist = false);
uint8_t GetBrightness();

// 从 NVS 恢复亮度（默认 75%，下限 5%）。mhal::Init() 默认已调用。
void Restore();

}  // namespace mhal::backlight
