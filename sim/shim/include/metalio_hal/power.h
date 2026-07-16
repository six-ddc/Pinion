/* sim shim — mhal::power（pi_quick_panel / pi_settings 用）。实现在
 * shim/src/mhal_shim.cc：电量/电压返回固定假值（78% 充电中 / 4012mV），
 * ForcePowerOff 打日志后退出进程。 */
#pragma once

#include <cstdint>

namespace mhal::power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
bool GetBatterySnapshot(int& level, bool& charging, bool& discharging);
bool GetVoltageMv(uint16_t& mv);

// 扩展电池遥测假桩（pi_card dashboard 用）：voltage/current 随时间小幅波动，
// 其余字段为固定演示值。
struct BatteryExt {
    uint16_t voltage_mv = 0;
    int16_t current_ma = 0;
    int16_t temp_c10 = 0;
    int16_t tte_min = -1;
    int soh_pct = 0;
    int fcc_mah = 0;
    int remcap_mah = 0;
    int cycles = 0;
};
bool GetBatteryExt(BatteryExt& out);

// TCA9555 输入脚检测桩：sim 里 USB 视为插着、无线充未在场。
bool IsUsbInserted();
bool IsWirelessCharging();

void ForcePowerOff();

}  // namespace mhal::power
