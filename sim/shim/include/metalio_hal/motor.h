/* sim shim — mhal::motor（device.vibrate invoke 用）。桌面无振动马达，
 * 实现在 shim/src/mhal_shim.cc 里只打日志。 */
#pragma once

#include <cstdint>

namespace mhal::motor {

void Init();
void Buzz(uint32_t duration_ms = 200, int strength_pct = 100);
void Stop();

}  // namespace mhal::motor
