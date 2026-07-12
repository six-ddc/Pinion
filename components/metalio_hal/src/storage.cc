// mhal::storage —— SdCardManager 的只读门面（挂载在 mhal::Init 里做）。
#include "metalio_hal/storage.h"

#include "SdCardManager.hpp"

namespace mhal {
namespace storage {

bool IsSdMounted() { return SdCardManager::GetInstance().IsMounted(); }

const char* GetMountPoint() { return SdCardManager::kMountPoint; }

}  // namespace storage
}  // namespace mhal
