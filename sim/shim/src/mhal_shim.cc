// sim shim — mhal::{audio,backlight,power} 桌面桩（pi_quick_panel 依赖）。
// 音量/亮度记在内存 + Settings 文件（pi_sim_settings.ini），与设备的 NVS
// key 同名（"audio"/"output_volume"、"display"/"brightness"）；电量固定假值；
// ForcePowerOff 打日志后退出进程（设备上是整机断电，不返回）。
#include <sys/stat.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/gps.h"
#include "metalio_hal/imu.h"
#include "metalio_hal/motor.h"
#include "metalio_hal/power.h"
#include "metalio_hal/storage.h"
#include "metalio_hal/sysmon.h"
#include "settings.h"

namespace {
int g_volume = -1;      // -1 = 尚未从 Settings 读入
int g_brightness = -1;  // 同上
bool g_gps_enabled = false;
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

namespace mhal::motor {

void Init() {}

void Buzz(uint32_t duration_ms, int strength_pct) {
    std::fprintf(stderr, "[sim][motor] Buzz(duration_ms=%u, strength_pct=%d)\n",
                 static_cast<unsigned>(duration_ms), strength_pct);
}

void Stop() { std::fprintf(stderr, "[sim][motor] Stop()\n"); }

}  // namespace mhal::motor

namespace mhal::power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
    level = 78;
    charging = true;
    discharging = false;
    return true;
}

bool GetBatterySnapshot(int& level, bool& charging, bool& discharging) {
    // sim 演示活性：随秒缓慢摆动 60..90，真机是原子快照
    long s = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
    level = 60 + static_cast<int>(s % 31);
    charging = true;
    discharging = false;
    return true;
}

bool GetVoltageMv(uint16_t& mv) {
    mv = 4012;
    return true;
}

bool GetBatteryExt(BatteryExt& out) {
    double t = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() /
               1000.0;
    out.voltage_mv = static_cast<uint16_t>(3900 + 40.0 * std::sin(t / 5.0));
    out.current_ma = static_cast<int16_t>(-350 + 30.0 * std::sin(t / 3.7));
    out.temp_c10 = 265;
    out.tte_min = 185;
    out.soh_pct = 96;
    out.fcc_mah = 1850;
    out.remcap_mah = 1240;
    out.cycles = 42;
    return true;
}

bool IsUsbInserted() { return true; }        // sim：视为插着 USB
bool IsWirelessCharging() { return false; }  // sim：无线充未在场

void ForcePowerOff() {
    std::fprintf(stderr, "[sim][power] ForcePowerOff — exiting\n");
    std::exit(0);
}

}  // namespace mhal::power

namespace mhal::imu {

bool Init() { return true; }

bool ReadAccel(int& x_mg, int& y_mg, int& z_mg) {
    x_mg = 0;
    y_mg = 0;
    z_mg = 1000;
    return true;
}

bool GetSnapshot(int& x_mg, int& y_mg, int& z_mg, int& pitch_deg, int& roll_deg) {
    // sim 演示活性：pitch/roll 随时间小幅摆动，看起来像放在桌上轻微晃动。
    double t = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() /
               1000.0;
    pitch_deg = static_cast<int>(3.0 * std::sin(t / 2.0));
    roll_deg = static_cast<int>(2.0 * std::sin(t / 3.3));
    x_mg = static_cast<int>(1000.0 * std::sin(pitch_deg * 3.14159265 / 180.0));
    y_mg = static_cast<int>(1000.0 * std::sin(roll_deg * 3.14159265 / 180.0));
    z_mg = 1000;
    return true;
}

}  // namespace mhal::imu

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

bool GetSdFreeBytes(uint64_t& total_bytes, uint64_t& free_bytes) {
    if (!IsSdMounted())
        return false;
    total_bytes = 31'914'983'424ULL;  // ~29.7GB
    free_bytes = 18'253'611'008ULL;   // ~17GB
    return true;
}

}  // namespace mhal::storage

namespace mhal::sysmon {

void Start(uint32_t /*period_ms*/) {
    std::fprintf(stderr, "[sim][sysmon] Start (no-op, GetCpuUsage/GetHeapKb are self-sampling)\n");
}

bool GetCpuUsage(int& core0, int& core1, int& avg) {
    double t = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() /
               1000.0;
    core0 = 15 + static_cast<int>(12.5 + 12.5 * std::sin(t / 4.0));
    core1 = 15 + static_cast<int>(12.5 + 12.5 * std::sin(t / 4.0 + 1.7));
    avg = (core0 + core1) / 2;
    return true;
}

bool GetHeapKb(unsigned& free_kb, unsigned& min_free_kb) {
    double t = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() /
               1000.0;
    free_kb = static_cast<unsigned>(300 + 20.0 * std::sin(t / 6.0));
    min_free_kb = 250;
    return true;
}

}  // namespace mhal::sysmon

namespace mhal::gps {

bool Enable(bool on) {
    g_gps_enabled = on;
    std::fprintf(stderr, "[sim][gps] Enable(%d)\n", on ? 1 : 0);
    return true;
}

bool IsEnabled() { return g_gps_enabled; }

bool GetFix(Fix& out) {
    // sim 可视化：PI_SIM_GPS=1 视为已启用（免走 Confirm 级 gps.enable），演示已定位状态。
    if (!g_gps_enabled && !(std::getenv("PI_SIM_GPS") && std::getenv("PI_SIM_GPS")[0] == '1')) {
        out = Fix{};
        return false;
    }
    // 固定演示定位点：上海。
    out.valid = true;
    out.lat = 31.2304;
    out.lon = 121.4737;
    out.alt_m = 12;
    out.speed_kmh = 0;
    out.sats = 9;
    return true;
}

}  // namespace mhal::gps

// pi_sys_info.h 的 sim 侧实现（设备侧在 main/sys_info.cc，走 esp_app_desc）
extern "C" const char* pi_sys_fw_version(void) { return "sim"; }
