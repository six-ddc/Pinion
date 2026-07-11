#pragma once

#include <functional>
#include <string>

// BT 音频模组控制门面。模组挂在 UART2（TX GPIO26 / RX GPIO27, 115200），
// 电源走 IOExpander BT_POWER。协议为厂商 AT 指令集：
//   模式1（接收）:  AT+RX=2 → 700ms → AT+MODE=1
//   模式2（发射/配对）: AT+TX=1 → 700ms → AT+MODE=2，可扫描/连接
//   模式3（音乐接收）:  AT+RX=1 → 700ms → AT+MODE=3
//   扫描: AT+INQUIRING；连接: AT+CONNECT=<12位hex地址>
//   通话模式: AT+PP=1 → 200ms → AT+BTSCO=1；音乐模式: AT+BTSCO=0 → AT+PP=1
// 模组回包（"SET MODE n"/"INQUIRING START"/"AT+BT:<addr><name>"/
// "CONNECT SUCCESS"…）由 lib 常驻解析，状态经 Callbacks 上报。
// （逻辑整体自旧 bluetooth_screen.cc 剥离，UI 部分已删。）
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
    // 人读状态文案（含未识别的原始回包行），原 bluetooth_screen 状态栏内容。
    std::function<void(const std::string&)> on_status_text;
};

// 回调在 UART RX / AT 发送任务上下文触发；要动 LVGL 请自行切线程。
// 传默认构造的 Callbacks{} 即取消订阅。
void SetCallbacks(Callbacks cbs);

// 切到默认模式1。mhal::Init(bt_default_mode=true) 开机已自动调用。
void ApplyDefaultMode();
void SetMode(Mode m);          // 后台任务发送对应 AT 序列
void StartScan();              // 需先处于 Mode::Tx
void Connect(const std::string& addr_hex);
void EnterCallMode();          // 需已连接
void EnterMusicMode();         // 需已连接
void PowerCycle();             // BT_POWER 断电 300ms 重上电，状态复位

Mode GetMode();
ConnState GetConnState();

}  // namespace mhal::bt
