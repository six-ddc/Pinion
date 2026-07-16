// sim shim — mhal::network 桌面桩。见 shim/include/metalio_hal/network.h 头注。
//
// P1 起模拟起网过程：首次 OnEvent 注册（pi_net_events::Init，发生在
// pi_screen Create）后起一条后台线程，~0.8s 后发 Scanning/Connecting、
// ~2s 后置 g_connected 并发 Connected——状态栏"连接中呼吸 → 按 RSSI/CSQ
// 亮格"的整条链路在 sim 里可见。IsConnected/GetWifiSsid/GetWifiRssi 等
// 快照接口在 connected 之前返回未连接语义。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "metalio_hal/network.h"
#include "settings.h"

namespace mhal::network {
namespace {

std::mutex g_mu;
EventCallback g_cb;
std::atomic<bool> g_connected{false};
std::atomic<bool> g_boot_thread_started{false};
std::atomic<bool> g_portal_active{false};

// sim 默认 WiFi（设备默认 4G；这里要演示"WiFi 已连接"的六页视觉）
Type LoadType() {
    Settings s("network", false);
    return s.GetInt("type", 0) == 1 ? Type::Cellular : Type::WiFi;
}

void Emit(Event e, const std::string& data = "") {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        cb = g_cb;
    }
    if (cb)
        cb(e, data);
}

// 模拟起网：与设备一致，事件在"网络栈自身线程"触发。
void StartBootSim() {
    if (g_boot_thread_started.exchange(true))
        return;
    std::thread([] {
        using namespace std::chrono_literals;
        Type t = LoadType();
        std::this_thread::sleep_for(800ms);
        if (t == Type::WiFi) {
            std::fprintf(stderr, "[sim][net] boot: WifiScanning\n");
            Emit(Event::WifiScanning);
            std::this_thread::sleep_for(600ms);
            std::fprintf(stderr, "[sim][net] boot: WifiConnecting(MetalioHome)\n");
            Emit(Event::WifiConnecting, "MetalioHome");
            std::this_thread::sleep_for(600ms);
            g_connected = true;
            std::fprintf(stderr, "[sim][net] boot: WifiConnected(MetalioHome)\n");
            Emit(Event::WifiConnected, "MetalioHome");
        } else {
            std::fprintf(stderr, "[sim][net] boot: ModemDetecting\n");
            Emit(Event::ModemDetecting);
            std::this_thread::sleep_for(600ms);
            std::fprintf(stderr, "[sim][net] boot: CellularConnecting\n");
            Emit(Event::CellularConnecting);
            std::this_thread::sleep_for(600ms);
            g_connected = true;
            std::fprintf(stderr, "[sim][net] boot: CellularConnected\n");
            Emit(Event::CellularConnected);
        }
    }).detach();
}

}  // namespace

void OnEvent(EventCallback cb) {
    bool has_cb = static_cast<bool>(cb);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cb = std::move(cb);
    }
    // 设备语义：注册须在 Start 之前；sim 没有真正的 Start 调用（main.cc 的
    // StartAsync 不在 sim 启动链里），以首次订阅当作起网触发点。
    if (has_cb)
        StartBootSim();
}

bool Start() { return true; }
void StartAsync() {
    // Phase3 测试用日志：即便 StartBootSim 因幂等早退（已起过），这行都能证明 net.reconnect
    // invoke 命令确实调用到了这里（sim/shim 本阶段允许为测试加日志）。
    std::fprintf(stderr, "[sim][net] StartAsync() called\n");
    StartBootSim();
}

Type GetType() { return LoadType(); }

void SwitchType() {
    Type next = LoadType() == Type::WiFi ? Type::Cellular : Type::WiFi;
    {
        Settings s("network", true);
        s.SetInt("type", next == Type::Cellular ? 1 : 0);
    }
    std::fprintf(stderr, "[sim][net] SwitchType -> %s; esp_restart (exiting)\n",
                 next == Type::Cellular ? "4G" : "WiFi");
    std::exit(0);  // 设备上 esp_restart 不返回；sim 退出进程等价
}

bool IsConnected() { return g_connected.load(); }

std::string GetWifiSsid() { return (g_connected && LoadType() == Type::WiFi) ? "MetalioHome" : ""; }
int GetWifiRssi() { return (g_connected && LoadType() == Type::WiFi) ? -52 : 0; }
std::string GetIpAddress() {
    if (!g_connected)
        return "";
    // 设备侧 WiFi/4G 两路都透出 IP（4G 走 iot_eth netif）；sim 给固定演示值。
    return LoadType() == Type::WiFi ? "192.168.1.36" : "10.64.21.7";
}

void AddWifiCredential(const std::string& ssid, const std::string&) {
    std::fprintf(stderr, "[sim][net] AddWifiCredential(%s)\n", ssid.c_str());
}

void StartConfigPortal() {
    std::fprintf(stderr, "[sim][net] StartConfigPortal — softAP up (fake)\n");
    g_portal_active = true;
    Emit(Event::WifiConfigPortal, "Metalio-SIM|http://192.168.4.1");
    // 非阻塞：设备上 AP 有自己的 http/dns/wifi 任务，这里立即返回即可模拟。
}

// 设备上会重启进配网；sim 无重启，直接进入配网态模拟"重启后的干净 AP"。
void RequestConfigPortalReboot() {
    std::fprintf(stderr, "[sim][net] RequestConfigPortalReboot — (no reboot in sim) enter portal\n");
    StartConfigPortal();
}

// 设备上重启回正常；sim 直接清配网态、模拟恢复联网。
void RebootToNormal() {
    std::fprintf(stderr, "[sim][net] RebootToNormal — exit portal, resume wifi\n");
    if (g_portal_active.exchange(false) && g_connected) {
        Emit(Event::WifiConnected, "MetalioHome");
    }
}

std::string GetConfigPortalSsid() {
    return g_portal_active.load() ? std::string("Metalio-SIM") : std::string();
}

void StopConfigPortal() {
    if (!g_portal_active.exchange(false))
        return;
    std::fprintf(stderr, "[sim][net] StopConfigPortal — softAP down (fake)\n");
    if (g_connected) {
        std::fprintf(stderr, "[sim][net] StopConfigPortal — resuming WifiConnected(MetalioHome)\n");
        Emit(Event::WifiConnected, "MetalioHome");
    }
}

bool IsConfigPortalActive() { return g_portal_active.load(); }

esp_err_t SendAtCommand(const std::string&, std::string& response, uint32_t, bool) {
    response = "OK";
    return ESP_OK;
}

// CSQ 26 -> -61dBm（4G 演示值）；未连上前未知（-1）
int GetSignalStrength() { return g_connected ? 26 : -1; }

std::string GetRegistrationStateJson() { return "{\"stat\":1,\"tac\":\"1A2B\",\"ci\":\"01F3\"}"; }

std::string GetOperator() { return "CHINA MOBILE"; }

}  // namespace mhal::network
