#pragma once

// sim shim — 与 components/metalio_hal/include/metalio_hal/storage.h 同一
// 声明；host 实现（mhal_shim.cc）用「目录存在 = 有卡」模拟：挂载点默认
// ./pi_sim_sd（PI_SIM_SD 环境变量可改路径），删掉目录即模拟"无 SD"。
namespace mhal {
namespace storage {

bool IsSdMounted();
const char* GetMountPoint();

}  // namespace storage
}  // namespace mhal
