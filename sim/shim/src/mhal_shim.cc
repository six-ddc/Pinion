// sim shim — mhal::{audio,backlight,power} 桌面桩（pi_quick_panel 依赖）。
// 音量/亮度记在内存 + Settings 文件（pi_sim_settings.ini），与设备的 NVS
// key 同名（"audio"/"output_volume"、"display"/"brightness"）；电量固定假值；
// ForcePowerOff 打日志后退出进程（设备上是整机断电，不返回）。
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/power.h"
#include "metalio_hal/storage.h"
#include "settings.h"

namespace {
int g_volume = -1;      // -1 = 尚未从 Settings 读入
int g_brightness = -1;  // 同上
}  // namespace

namespace mhal::audio {

void SetVolume(int percent, bool persist) {
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    g_volume = percent;
    if (persist) {
        Settings s("audio", true);
        s.SetInt("output_volume", percent);
    }
    std::fprintf(stderr, "[sim][audio] SetVolume(%d, persist=%d)\n", percent, persist ? 1 : 0);
}

int GetVolume() {
    if (g_volume < 0) {
        Settings s("audio", false);
        g_volume = static_cast<int>(s.GetInt("output_volume", 70));
    }
    return g_volume;
}

}  // namespace mhal::audio

namespace mhal::backlight {

void SetBrightness(uint8_t percent, bool persist) {
    if (percent > 100)
        percent = 100;
    g_brightness = percent;
    if (persist) {
        Settings s("display", true);
        s.SetInt("brightness", percent);
    }
    std::fprintf(stderr, "[sim][backlight] SetBrightness(%u, persist=%d)\n",
                 static_cast<unsigned>(percent), persist ? 1 : 0);
}

uint8_t GetBrightness() {
    if (g_brightness < 0) {
        Settings s("display", false);
        g_brightness = static_cast<int>(s.GetInt("brightness", 75));
    }
    return static_cast<uint8_t>(g_brightness);
}

void Restore() { SetBrightness(GetBrightness(), false); }

}  // namespace mhal::backlight

namespace mhal::power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
    level = 78;
    charging = true;
    discharging = false;
    return true;
}

bool GetVoltageMv(uint16_t& mv) {
    mv = 4012;
    return true;
}

void ForcePowerOff() {
    std::fprintf(stderr, "[sim][power] ForcePowerOff — exiting\n");
    std::exit(0);
}

}  // namespace mhal::power

namespace mhal::storage {

// 「目录存在 = 有卡」：默认 ./pi_sim_sd（相对运行目录），PI_SIM_SD 可改。
// 删掉/改名目录即可模拟"无 SD"场景（会话归档整体禁用）。
const char* GetMountPoint() {
    static std::string p = [] {
        const char* env = std::getenv("PI_SIM_SD");
        return std::string(env != nullptr && env[0] != '\0' ? env : "./pi_sim_sd");
    }();
    return p.c_str();
}

bool IsSdMounted() {
    struct stat st;
    return ::stat(GetMountPoint(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace mhal::storage

// pi_sys_info.h 的 sim 侧实现（设备侧在 main/sys_info.cc，走 esp_app_desc）
extern "C" const char* pi_sys_fw_version(void) { return "sim"; }
