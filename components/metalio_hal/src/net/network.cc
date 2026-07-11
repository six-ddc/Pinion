// 双网络门面 —— 原 DualNetworkBoard（类型选择/切换）+ WifiBoard
// （esp-wifi-connect 起网/配网）+ Nt26Board（4G）的去 UI 化合体。
// 原 Display::SetStatus/ShowNotification 一律改为 OnEvent 回调；
// Application::Reboot → esp_restart。
#include "metalio_hal/network.h"

#include <atomic>

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ssid_manager.h>
#include <wifi_configuration_ap.h>
#include <wifi_station.h>

#include "config.h"
#include "nt26_modem.h"
#include "settings.h"

#define TAG "mhal_net"

namespace mhal::network {
namespace {

// 与旧固件 METALIO_CLAW_4 构造参数一致：NVS 无记录时默认 4G。
constexpr int32_t kDefaultNetType = 1;

EventCallback s_event_cb;
std::atomic<bool> s_cellular_connected{false};
Nt26Modem* s_modem = nullptr;

void Emit(Event e, const std::string& data = "") {
    if (s_event_cb) {
        s_event_cb(e, data);
    }
}

Type LoadType() {
    Settings settings("network", true);
    return settings.GetInt("type", kDefaultNetType) == 1 ? Type::Cellular : Type::WiFi;
}

void SaveType(Type t) {
    Settings settings("network", true);
    settings.SetInt("type", t == Type::Cellular ? 1 : 0);
}

Type CachedType() {
    static Type type = LoadType();
    return type;
}

bool StartWifi() {
    {
        // 网页配网入口旗标（旧 WifiBoard::ResetWifiConfiguration 语义）：
        // force_ap=1 时本次启动直接进配网模式，并清零旗标。
        Settings settings("wifi", true);
        if (settings.GetInt("force_ap") == 1) {
            ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
            settings.SetInt("force_ap", 0);
            StartConfigPortal();  // 不返回
        }
    }

    auto ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        // 旧固件在这里死循环等待；lib 语义改为返回 false —— 调用方可
        // AddWifiCredential() 后重试，或 StartConfigPortal() 配网。
        ESP_LOGW(TAG, "no stored WiFi credentials");
        Emit(Event::WifiNoCredentials);
        return false;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([]() { Emit(Event::WifiScanning); });
    wifi_station.OnConnect([](const std::string& ssid) { Emit(Event::WifiConnecting, ssid); });
    wifi_station.OnConnected([](const std::string& ssid) { Emit(Event::WifiConnected, ssid); });
    wifi_station.Start();

    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        Emit(Event::WifiConnectFailed);
        return false;
    }
    return true;
}

bool StartCellular() {
    if (s_modem == nullptr) {
        s_modem = new Nt26Modem(NT26_TX_PIN, NT26_RX_PIN, NT26_MRDY_PIN, NT26_SRDY_PIN);
        s_modem->SetEventCallback([](Event e, const std::string& data) {
            if (e == Event::CellularConnected) {
                s_cellular_connected = true;
            } else if (e == Event::CellularDisconnected) {
                s_cellular_connected = false;
            }
            Emit(e, data);
        });
    }
    return s_modem->Start();
}

}  // namespace

void OnEvent(EventCallback cb) { s_event_cb = std::move(cb); }

bool Start() {
    if (CachedType() == Type::WiFi) {
        ESP_LOGI(TAG, "starting network: WiFi");
        return StartWifi();
    }
    ESP_LOGI(TAG, "starting network: Cellular (NT26)");
    return StartCellular();
}

void StartAsync() {
    xTaskCreate(
        [](void*) {
            Start();
            vTaskDelete(nullptr);
        },
        "net_start", 8192, nullptr, 4, nullptr);
}

Type GetType() { return CachedType(); }

void SwitchType() {
    SaveType(CachedType() == Type::WiFi ? Type::Cellular : Type::WiFi);
    ESP_LOGI(TAG, "network type switched, restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool IsConnected() {
    if (CachedType() == Type::WiFi) {
        return WifiStation::GetInstance().IsConnected();
    }
    return s_cellular_connected;
}

void AddWifiCredential(const std::string& ssid, const std::string& password) {
    SsidManager::GetInstance().AddSsid(ssid, password);
}

void StartConfigPortal() {
    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage("zh-CN");
    wifi_ap.SetSsidPrefix("Metalio");
    wifi_ap.Start();

    ESP_LOGI(TAG, "WiFi config AP started: %s (%s)", wifi_ap.GetSsid().c_str(),
             wifi_ap.GetWebServerUrl().c_str());
    Emit(Event::WifiConfigPortal, wifi_ap.GetSsid() + "|" + wifi_ap.GetWebServerUrl());

    // 配网页提交凭据后由 esp-wifi-connect 自行重启设备；这里等待即可。
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t SendAtCommand(const std::string& cmd, std::string& response, uint32_t timeout_ms,
                        bool bypass_init_check) {
    if (s_modem == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_modem->SendAtCommand(cmd, response, timeout_ms, bypass_init_check);
}

int GetSignalStrength() {
    if (s_modem == nullptr) {
        return -1;
    }
    return s_modem->GetSignalStrength();
}

std::string GetRegistrationStateJson() {
    if (s_modem == nullptr) {
        return "{}";
    }
    return s_modem->GetRegistrationState().ToString();
}

}  // namespace mhal::network
