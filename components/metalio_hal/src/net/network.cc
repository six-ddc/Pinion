// 双网络门面 —— 原 DualNetworkBoard（类型选择/切换）+ WifiBoard
// （esp-wifi-connect 起网/配网）+ Nt26Board（4G）的去 UI 化合体。
// 原 Display::SetStatus/ShowNotification 一律改为 OnEvent 回调；
// Application::Reboot → esp_restart。
#include "metalio_hal/network.h"

#include <atomic>
#include <mutex>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>
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
std::atomic<bool> s_wifi_started{false};   // WifiStation::Start() 已成功调用且未 Stop
std::atomic<bool> s_portal_active{false};  // WifiConfigurationAp 正在跑
std::string s_portal_ssid;                 // 配网热点 SSID（s_portal_active 为 true 期间有效）
Nt26Modem* s_modem = nullptr;

std::mutex s_ssid_mu;     // 叶子锁：临界区仅 std::string 赋值/拷贝
std::string s_wifi_ssid;  // 门面侧缓存，由 OnConnected 回调写入，GetWifiSsid() 加锁读副本

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

// 网络连通后起 SNTP 对时（幂等）。系统时间没同步前 UI 时钟只能显示
// 占位符（pi_screen 以年份 >= 2025 判定时间可信）。
void StartSntpOnce() {
    static bool started = false;
    if (started) return;
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
    } else {
        ESP_LOGW(TAG, "SNTP init failed");
    }
}

// 起 WifiStation 并等待联通（要求已有已存凭据）；成功置 s_wifi_started。
// 供 StartWifi() 正常路径与 StopConfigPortal() 后台重连复用。
bool StartWifiStationAndWait() {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([]() { Emit(Event::WifiScanning); });
    wifi_station.OnConnect([](const std::string& ssid) { Emit(Event::WifiConnecting, ssid); });
    wifi_station.OnConnected([](const std::string& ssid) {
        {
            std::lock_guard<std::mutex> lk(s_ssid_mu);
            s_wifi_ssid = ssid;
        }
        Emit(Event::WifiConnected, ssid);
    });

    if (s_wifi_started) {
        // 已经起过一次（例如开机时）：net.reconnect 等场景会再次打到这里——
        // WifiStation::Start() 假设"从未初始化过"的干净态，无条件
        // esp_netif_create_default_wifi_sta() 建 station netif，若 netif 已存在（if_key
        // "WIFI_STA_DEF" 撞车）esp_netif_new() 返回 NULL，内部 assert(netif) 直接重启
        // 设备——真机实测复现过（serial.log 第 796-844 行；backtrace 经 addr2line 精确定位到
        // wifi_default.c:423 esp_netif_create_default_wifi_sta ← wifi_station.cc:103
        // WifiStation::Start() ← 本函数 ← StartWifi() ← Start() ← net.reconnect 触发的
        // StartAsync()）。net.reconnect 真正要的语义是"断开重连"（esp_wifi_disconnect +
        // esp_wifi_connect 级别），不是把 netif/驱动全部拆了重建：这里只调
        // esp_wifi_disconnect()——WifiStation 自己的 WIFI_EVENT_STA_DISCONNECTED 处理器
        // （wifi_station.cc:246-253）本来就会在 reconnect_count_ 预算内自动重新
        // esp_wifi_connect()（复用同一份已 esp_wifi_set_config 过的 STA 配置），不需要越权
        // 替它连、也不需要重建 netif 这种重手术。若这次调用时其实已经空闲断线（没有可断的
        // 连接），esp_wifi_disconnect() 只是无副作用地返回 ESP_ERR_WIFI_NOT_CONNECT。
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
        }
    } else {
        wifi_station.Start();
        s_wifi_started = true;
    }

    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        s_wifi_started = false;
        Emit(Event::WifiConnectFailed);
        return false;
    }
    StartSntpOnce();
    return true;
}

bool StartWifi() {
    {
        // 网页配网入口旗标（旧 WifiBoard::ResetWifiConfiguration 语义）：
        // force_ap=1 时本次启动直接进配网模式，并清零旗标。
        Settings settings("wifi", true);
        if (settings.GetInt("force_ap") == 1) {
            ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
            settings.SetInt("force_ap", 0);
            StartConfigPortal();  // 非阻塞：起完 AP 即返回
            return true;          // AP 已起（不代表已联网），语义到此为止
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

    return StartWifiStationAndWait();
}

bool StartCellular() {
    if (s_modem == nullptr) {
        s_modem = new Nt26Modem(NT26_TX_PIN, NT26_RX_PIN, NT26_MRDY_PIN, NT26_SRDY_PIN);
        s_modem->SetEventCallback([](Event e, const std::string& data) {
            if (e == Event::CellularConnected) {
                s_cellular_connected = true;
                StartSntpOnce();
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

std::string GetWifiSsid() {
    if (CachedType() != Type::WiFi) {
        return "";
    }
    if (!WifiStation::GetInstance().IsConnected()) {
        return "";
    }
    std::lock_guard<std::mutex> lk(s_ssid_mu);
    return s_wifi_ssid;
}

int GetWifiRssi() {
    if (CachedType() != Type::WiFi) {
        return 0;
    }
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;  // 未连/未init→0，不 abort，无 TOCTOU
    return ap.rssi;
}

std::string GetIpAddress() {
    if (CachedType() == Type::WiFi) {
        auto& station = WifiStation::GetInstance();
        return station.IsConnected() ? station.GetIpAddress() : "";
    }
    // 4G：从 Nt26Modem 持有的 iot_eth netif 读 IP（未连通/未拿到 IP 返回 ""）。
    if (s_modem != nullptr && s_cellular_connected) {
        esp_netif_t* netif = s_modem->GetNetif();
        esp_netif_ip_info_t info{};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            char buf[16];
            return esp_ip4addr_ntoa(&info.ip, buf, sizeof(buf));
        }
    }
    return "";
}

void AddWifiCredential(const std::string& ssid, const std::string& password) {
    SsidManager::GetInstance().AddSsid(ssid, password);
}

void StartConfigPortal() {
    // 只在 WiFi 栈干净（station 未起）时调用——正常只在开机 force_ap 路径触发，
    // 此时真机 ESP-Hosted/C5 上起 AP 100% 可靠。运行时（已联网）改由
    // RequestConfigPortalReboot() 走"置 force_ap + 重启"进来，绝不就地
    // STA→AP 切换（真机实测会崩）。此处保留 station 拆除仅作防御，正常不触发。
    if (s_wifi_started) {
        WifiStation::GetInstance().Stop();
        s_wifi_started = false;
    }

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage("zh-CN");
    wifi_ap.SetSsidPrefix("Metalio");
    wifi_ap.Start();

    ESP_LOGI(TAG, "WiFi config AP started: %s (%s)", wifi_ap.GetSsid().c_str(),
             wifi_ap.GetWebServerUrl().c_str());
    s_portal_ssid = wifi_ap.GetSsid();
    s_portal_active = true;
    Emit(Event::WifiConfigPortal, wifi_ap.GetSsid() + "|" + wifi_ap.GetWebServerUrl());

    // 非阻塞：AP 有自己的 http/dns/wifi 任务，本函数返回后热点继续存活。
    // 提交凭据成功后 esp-wifi-connect 自行保存并 esp_restart（不返回，
    // 无需我们处理）。
}

// 运行时触发配网的可靠入口：置 NVS wifi/force_ap=1（若当前是 4G 则切回
// WiFi，否则开机不会跑 WiFi 栈也就进不了配网），随后 esp_restart。开机
// StartWifi() 见 force_ap 即在"station 未起的干净态"下起 AP——这是真机
// 验证过的可靠路径，规避就地 STA→AP 切换在 C5 上的崩溃。不返回。
void RequestConfigPortalReboot() {
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    if (CachedType() != Type::WiFi) {
        SaveType(Type::WiFi);
    }
    ESP_LOGI(TAG, "config portal requested -> restart into clean AP mode");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

// 退出配网并回正常联网：force_ap 已在开机时清零，直接重启即可正常连网。
void RebootToNormal() {
    ESP_LOGI(TAG, "exit config portal -> restart to normal");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

std::string GetConfigPortalSsid() { return s_portal_active ? s_portal_ssid : std::string(); }

void StopConfigPortal() {
    if (!s_portal_active) {
        return;
    }
    WifiConfigurationAp::GetInstance().Stop();
    s_portal_active = false;
    ESP_LOGI(TAG, "WiFi config AP stopped");

    // 离开配网页时若原来有已存凭据，后台恢复联网；无凭据则保持 wifi 关闭。
    auto ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        return;
    }
    xTaskCreate(
        [](void*) {
            StartWifiStationAndWait();
            vTaskDelete(nullptr);
        },
        "net_reconn", 8192, nullptr, 4, nullptr);
}

bool IsConfigPortalActive() { return s_portal_active; }

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

std::string GetOperator() {
    if (s_modem == nullptr) {
        return "";
    }
    return s_modem->GetCarrierName();
}

}  // namespace mhal::network
