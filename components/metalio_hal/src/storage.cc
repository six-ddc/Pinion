// mhal::storage —— SdCardManager 的只读门面（挂载在 mhal::Init 里做）。
#include "metalio_hal/storage.h"

#include <esp_vfs_fat.h>

#include "SdCardManager.hpp"

namespace mhal {
namespace storage {

bool IsSdMounted() { return SdCardManager::GetInstance().IsMounted(); }

// 用 ESP-IDF 官方 esp_vfs_fat_info(base_path)：newlib 无 sys/statvfs.h，且
// esp_vfs_fat_sdmmc_mount() 内部自选卷号、门面拿不到 FatFs 卷号字符串去调
// f_getfree。esp_vfs_fat_info 按挂载点路径查，内部走 f_getfree，等价效果、最稳。
bool GetSdFreeBytes(uint64_t& total_bytes, uint64_t& free_bytes) {
    if (!IsSdMounted()) {
        return false;
    }
    return esp_vfs_fat_info(SdCardManager::kMountPoint, &total_bytes, &free_bytes) == ESP_OK;
}

const char* GetMountPoint() { return SdCardManager::kMountPoint; }

}  // namespace storage
}  // namespace mhal
