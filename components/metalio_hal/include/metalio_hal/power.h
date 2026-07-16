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

// 扩展电池遥测（BQ27220 standard commands）：温度 / 剩余续航 / 健康度 / 满充容量 /
// 循环次数等。由 1Hz sysmon 任务采样并发布快照，此处为非阻塞读（无 I2C）。
// 从未成功采样过时返回 false（各字段保持默认）。
struct BatteryExt {
    uint16_t voltage_mv = 0;
    int16_t  current_ma = 0;   // + 充电 / - 放电
    int16_t  temp_c10   = 0;   // 0.1 ℃
    int16_t  tte_min    = -1;  // 剩余续航分钟；非放电/未知 = -1
    int      soh_pct    = 0;   // 健康度 %
    int      fcc_mah    = 0;   // 满充容量 mAh
    int      remcap_mah = 0;   // 剩余容量 mAh
    int      cycles     = 0;   // 循环次数
};
bool GetBatteryExt(BatteryExt& out);

// TCA9555 输入脚检测：USB 插入（P10）/ 无线充电在场（P11）。每次读做一次 IO 扩展器
// I2C 读（快，非阻塞）。⚠️ 有效极性（高/低电平代表"有效"）以原理图为准，IOExpander.hpp
// 亦注"电平含义见原理图"——当前暂按"高=有效"，真机确认后翻 power.cc 内的
// kUsbInsertActiveHigh / kWirelessActiveHigh 常量即可，无需改调用点。读取失败返回 false。
bool IsUsbInserted();
bool IsWirelessCharging();

// 通过 IOExpander PWR_KEY_PULSE 发 10 次 100ms 脉冲强制整机断电。
// 正常情况下不返回。
void ForcePowerOff();

}  // namespace mhal::power
