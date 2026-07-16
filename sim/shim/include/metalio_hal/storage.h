#pragma once

#include <cstdint>

// sim shim — 与 components/metalio_hal/include/metalio_hal/storage.h 同一
// 声明；host 实现（mhal_shim.cc）用「目录存在 = 有卡」模拟：挂载点默认
// ./pi_sim_sd（PI_SIM_SD 环境变量可改路径），删掉目录即模拟"无 SD"。
namespace mhal {
namespace storage {

bool IsSdMounted();
const char* GetMountPoint();

// 固定假值（~29.7GB 总容量 / ~17GB 可用），未挂载时返回 false。
bool GetSdFreeBytes(uint64_t& total_bytes, uint64_t& free_bytes);

}  // namespace storage
}  // namespace mhal
