/* sim shim — mhal::audio 的音量子集（pi_quick_panel 用）。实现在
 * shim/src/mhal_shim.cc：音量落在 Settings 文件里并打日志。 */
#pragma once

namespace mhal::audio {

void SetVolume(int percent, bool persist = true);
int GetVolume();

}  // namespace mhal::audio
