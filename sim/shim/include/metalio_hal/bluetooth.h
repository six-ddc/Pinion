/* sim shim — mhal::bt 桌面桩（pi_settings 用），API 与
 * components/metalio_hal/include/metalio_hal/bluetooth.h 一致。实现在
 * shim/src/bt_shim.cc：假模式切换；StartScan 后 ~1.5s 由后台线程流式吐
 * 3 个假设备；Connect 后 ~1s 变已连接。回调在后台线程触发（与设备的
 * UART RX 任务语义一致，UI 侧必须自行封送回 LVGL 线程）。 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mhal::bt {

enum class Mode : uint8_t { None = 0, Rx = 1, Tx = 2, MusicRx = 3 };
enum class ConnState : uint8_t { Idle, Scanning, Connecting, Connected };

struct Device {
    std::string addr_hex;  // 12 位 hex，无分隔符
    std::string name;      // 可能为空
};

struct Callbacks {
    std::function<void(Mode)> on_mode_changed;
    std::function<void(ConnState)> on_conn_state;
    std::function<void(const Device&)> on_device_found;
    std::function<void(const std::string&)> on_status_text;
};

void SetCallbacks(Callbacks cbs);

void ApplyDefaultMode();
void SetMode(Mode m);
void StartScan();
void Connect(const std::string& addr_hex);
void EnterCallMode();
void EnterMusicMode();
void PowerCycle();

Mode GetMode();
ConnState GetConnState();

}  // namespace mhal::bt
