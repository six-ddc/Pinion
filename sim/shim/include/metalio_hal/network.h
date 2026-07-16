/* sim shim — mhal::network 桌面桩（pi_settings 用）。实现在
 * shim/src/net_shim.cc：假装 WiFi 已连接 "MetalioHome"（-52dBm /
 * 192.168.1.36）；OnEvent 存回调；SwitchType 写类型后模拟 esp_restart
 * （打日志退出进程）；StartConfigPortal 发事件后立即返回（非阻塞，与
 * 设备语义一致）；StopConfigPortal 清 portal 态，若原来已连会模拟恢复
 * 联网（发 WifiConnected）。 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"

namespace mhal::network {

enum class Type { WiFi = 0, Cellular = 1 };

enum class Event {
    WifiScanning,
    WifiConnecting,
    WifiConnected,
    WifiNoCredentials,
    WifiConnectFailed,
    WifiConfigPortal,
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

void OnEvent(EventCallback cb);
bool Start();
void StartAsync();

Type GetType();
void SwitchType();

bool IsConnected();

std::string GetWifiSsid();
int GetWifiRssi();
std::string GetIpAddress();

void AddWifiCredential(const std::string& ssid, const std::string& password);
void StartConfigPortal();
void StopConfigPortal();
bool IsConfigPortalActive();
std::string GetConfigPortalSsid();
void RequestConfigPortalReboot();
void RebootToNormal();

esp_err_t SendAtCommand(const std::string& cmd, std::string& response, uint32_t timeout_ms = 5000,
                        bool bypass_init_check = false);
int GetSignalStrength();
std::string GetRegistrationStateJson();
std::string GetOperator();

}  // namespace mhal::network
