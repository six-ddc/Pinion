/* sim shim — mhal::sysmon 桌面桩（pi_card dashboard 用）。实现在
 * shim/src/mhal_shim.cc：Start 空实现；GetCpuUsage/GetHeapKb 返回随时间
 * 波动的假数据，喂 chart 用（真机语义见 components/metalio_hal 同名头）。 */
#pragma once

#include <cstdint>

namespace mhal::sysmon {

void Start(uint32_t period_ms = 1000);

bool GetCpuUsage(int& core0, int& core1, int& avg);

bool GetHeapKb(unsigned& free_kb, unsigned& min_free_kb);

}  // namespace mhal::sysmon
