#pragma once

#include <cstdint>

// 电池/电源门面（BQ27220 电量计，I2C 0x55）。
// 电量 = 电压线性插值(3.3V=0%, 4.2V=100%) + 60 样本滑动平均；
// charging/discharging 由电流符号判定（±5mA 阈值）。
// 电量计不在位时各读函数返回 false（内部自带节流重探自愈）。
namespace mhal::power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
bool GetVoltageMv(uint16_t& mv);
bool GetCurrentMa(int16_t& ma);

// 通过 IOExpander PWR_KEY_PULSE 发 10 次 100ms 脉冲强制整机断电。
// 正常情况下不返回。
void ForcePowerOff();

}  // namespace mhal::power
