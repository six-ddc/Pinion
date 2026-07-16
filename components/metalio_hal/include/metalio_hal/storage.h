#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// mhal::storage —— SD 卡状态只读查询。
//
// 挂载动作在 mhal::Init()（InitOptions::mount_sd_card）内部完成，失败不致命；
// 上层（如会话归档）用 IsSdMounted() 决定是否启用依赖 SD 的功能。本头不含
// 任何 UI/业务引用，读状态零成本、任意线程可调。
// ---------------------------------------------------------------------------
namespace mhal {
namespace storage {

// mhal::Init 时挂载成功（此后常驻挂载，无热插拔监测）。
bool IsSdMounted();

// SD 卡总容量 / 剩余可用空间（字节）。未挂载（IsSdMounted() == false）或
// 查询失败时返回 false，不改写输出参数。
bool GetSdFreeBytes(uint64_t& total_bytes, uint64_t& free_bytes);

// VFS 挂载点（恒定 "/sdcard"，与挂载是否成功无关）。
const char* GetMountPoint();

}  // namespace storage
}  // namespace mhal
