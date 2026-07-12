/* sim shim — mhal::backlight（pi_quick_panel 用）。实现在
 * shim/src/mhal_shim.cc：persist=true 时写 Settings 文件并打日志。 */
#pragma once

#include <cstdint>

namespace mhal::backlight {

void SetBrightness(uint8_t percent, bool persist = false);
uint8_t GetBrightness();
void Restore();

}  // namespace mhal::backlight
