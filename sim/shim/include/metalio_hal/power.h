/* sim shim — mhal::power（pi_quick_panel / pi_settings 用）。实现在
 * shim/src/mhal_shim.cc：电量/电压返回固定假值（78% 充电中 / 4012mV），
 * ForcePowerOff 打日志后退出进程。 */
#pragma once

#include <cstdint>

namespace mhal::power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
bool GetVoltageMv(uint16_t& mv);
void ForcePowerOff();

}  // namespace mhal::power
