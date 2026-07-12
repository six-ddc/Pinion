#pragma once

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

// VFS 挂载点（恒定 "/sdcard"，与挂载是否成功无关）。
const char* GetMountPoint();

}  // namespace storage
}  // namespace mhal
