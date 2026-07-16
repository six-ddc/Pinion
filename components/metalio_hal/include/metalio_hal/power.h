#pragma once

#include <cstdint>

// 电池/电源门面（BQ27220 电量计，I2C 0x55）。
// 电量 = 电压线性插值(3.3V=0%, 4.2V=100%) + 60 样本滑动平均；
// charging/discharging 由电流符号判定（±5mA 阈值）。
// 电量计不在位时各读函数返回 false（内部自带节流重探自愈）。
namespace mhal::power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging);

// 非阻塞、原子读上一次成功采样发布的快照（由 1Hz sysmon 任务或任一调用
// GetBatteryLevel 的调用方顺带发布），无 I2C、无滤波器改写。从未成功采样过
// 时返回 false（level=0）。
bool GetBatterySnapshot(int& level, bool& charging, bool& discharging);

bool GetVoltageMv(uint16_t& mv);
bool GetCurrentMa(int16_t& ma);

// 通过 IOExpander PWR_KEY_PULSE 发 10 次 100ms 脉冲强制整机断电。
// 正常情况下不返回。
void ForcePowerOff();

}  // namespace mhal::power
