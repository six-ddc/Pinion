#pragma once

#include <esp_err.h>
#include <cstdint>
#include <functional>
#include <string>

// 双网络门面：Wi-Fi（ESP32-C5 ESP-Hosted SDIO 全栈）+ 4G（NT26 UART1
// 2Mbps + iot_eth）。当前网络类型持久化于 NVS "network"/"type"
// （0=WiFi，1=Cellular），与旧固件一致。
//
// 原 board 层在起网过程中直接写 Display 状态栏；lib 化后全部翻转为
// OnEvent() 回调，UI 想显示什么自己订阅。
namespace mhal::network {

enum class Type { WiFi = 0, Cellular = 1 };

enum class Event {
    // Wi-Fi 路径
    WifiScanning,
    WifiConnecting,        // data = ssid
    WifiConnected,         // data = ssid
    WifiNoCredentials,     // NVS "wifi" 无任何已存 SSID
    WifiConnectFailed,     // 60s 内未连上
    WifiConfigPortal,      // data = "ssid|url"（softAP 配网页已起）
    // 4G 路径
    ModemDetecting,
    CellularConnecting,
    CellularConnected,
    CellularDisconnected,
    CellularErrorNoSim,
    CellularErrorRegDenied,
    CellularErrorInitFailed,
    CellularErrorTimeout,
};

using EventCallback = std::function<void(Event event, const std::string& data)>;

// 事件回调（在网络栈自身任务上下文触发；要动 LVGL 请自行切线程/加锁）。
// 必须在 Start 之前注册才能收到起网过程事件。
void OnEvent(EventCallback cb);

// 按当前 Type 起网。阻塞（Wi-Fi 最长 60s；4G 检测+注册最长约 90s），
// 返回是否已连通。在专用任务里调，或直接用 StartAsync()。
bool Start();
// xTaskCreate 包装：后台起网，结果通过 OnEvent 观察。
void StartAsync();

Type GetType();
// 持久化另一种网络类型并 esp_restart()（沿用旧固件的切换语义，不返回）。
void SwitchType();

bool IsConnected();

// —— 连接信息快照（设置页显示用；全部即取即回，不阻塞） ——
// WiFi 已连接时的 SSID；非 WiFi 模式或未连接返回 ""。
std::string GetWifiSsid();
// WiFi RSSI（dBm，负值）；非 WiFi 模式或未连接返回 0。
int GetWifiRssi();
// 当前 IP（WiFi 路径经 esp-wifi-connect 缓存；4G 路径读 iot_eth netif）；
// 未连接或尚未拿到 IP 返回 ""。
std::string GetIpAddress();

// —— Wi-Fi 配网 ——
// 追加一组凭据到 NVS "wifi"（SsidManager），下次 Start 即可用。
void AddWifiCredential(const std::string& ssid, const std::string& password);
// 起 softAP + 网页配网。非阻塞：起完 AP 即返回，热点由 AP 自身的
// http/dns/wifi 任务维持存活。若 station 已起（已联网/已初始化），
// 会先 WifiStation::Stop() 干净拆掉再起 AP，避免 esp_wifi_init 状态冲突。
// 用户提交凭据成功后 esp-wifi-connect 自行保存并 esp_restart（不返回）；
// 用户中途离开配网页则调 StopConfigPortal() 收尾。可反复 Start/Stop。
void StartConfigPortal();
// 停 softAP + 网页配网（幂等，未起时 no-op）。若存在已存 WiFi 凭据，会
// 后台起一个任务恢复 station 联网（xTaskCreate，不阻塞调用线程）；无
// 凭据则保持 wifi 关闭。
void StopConfigPortal();
// 配网热点当前是否在跑（Start 未 Stop 之间为 true）。纯读，任意线程可调。
bool IsConfigPortalActive();
// 配网中返回热点 SSID（如 "Metalio-A191"），否则返回 ""。纯读，任意线程可调。
// 用于"重启进配网"后网络页构建时直接取名显示（事件在开机时已发过、页面未开）。
std::string GetConfigPortalSsid();

// 运行时触发配网的可靠入口：置 NVS wifi/force_ap=1（4G 模式会切回 WiFi）后
// esp_restart，开机在干净态起 AP。**不返回。** 已联网/已初始化 WiFi 栈时必须
// 走此路，而非就地 StartConfigPortal —— 后者的 STA→AP 切换在 C5 上会崩溃。
void RequestConfigPortalReboot();
// 退出配网、重启回正常联网（force_ap 已在开机清零，普通重启即正常连网）。不返回。
void RebootToNormal();

// —— 4G 专属（WiFi 模式或 modem 未就绪时返回 ESP_ERR_INVALID_STATE）——
// 透传 AT 命令（线程安全）。bypass_init_check 见旧 Nt26Board 注释：
// 未插卡时 modem 不会 initialized 但 AT 通道可用（如切卡场景）。
esp_err_t SendAtCommand(const std::string& cmd, std::string& response,
                        uint32_t timeout_ms = 5000, bool bypass_init_check = false);
// CSQ 信号强度（0-31，99/-1=未知）；modem 未就绪返回 -1。
int GetSignalStrength();
// AT+CEREG 注册状态 JSON（{"stat":1,...}；stat 1/5=已注册）。
std::string GetRegistrationStateJson();

}  // namespace mhal::network
