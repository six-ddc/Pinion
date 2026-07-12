// sim shim — mhal::bt 桌面桩。回调从 detached std::thread 触发，模拟设备上
// UART RX / AT 任务的线程语义（UI 必须封送回 LVGL 线程才碰 widget）。
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "metalio_hal/bluetooth.h"

namespace mhal::bt {
namespace {

std::mutex g_mu;
Callbacks g_cbs;
Mode g_mode = Mode::None;
ConnState g_conn = ConnState::Idle;
int g_scan_gen = 0;  // 递增代号：老扫描线程醒来发现代号不对就放弃

Callbacks SnapshotCbs() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cbs;
}

void SetConn(ConnState st) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_conn = st;
    }
    Callbacks cbs = SnapshotCbs();
    if (cbs.on_conn_state)
        cbs.on_conn_state(st);
}

}  // namespace

void SetCallbacks(Callbacks cbs) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cbs = std::move(cbs);
}

void ApplyDefaultMode() { SetMode(Mode::Rx); }

void SetMode(Mode m) {
    std::fprintf(stderr, "[sim][bt] SetMode(%d)\n", static_cast<int>(m));
    std::thread([m]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // AT 序列 ~700ms 的缩影
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_mode = m;
            g_conn = ConnState::Idle;
            g_scan_gen++;  // 切模作废在途扫描/连接
        }
        Callbacks cbs = SnapshotCbs();
        if (cbs.on_mode_changed)
            cbs.on_mode_changed(m);
        if (cbs.on_conn_state)
            cbs.on_conn_state(ConnState::Idle);
        if (cbs.on_status_text) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "SET MODE %d", static_cast<int>(m));
            cbs.on_status_text(buf);
        }
    }).detach();
}

void StartScan() {
    std::fprintf(stderr, "[sim][bt] StartScan\n");
    int gen;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        gen = ++g_scan_gen;
    }
    SetConn(ConnState::Scanning);
    std::thread([gen]() {
        static const Device kFakeDevices[] = {
            {"04FE12AB34CD", "Metalio BT Speaker"},
            {"7C669D8801F2", "JBL GO 3"},
            {"A1B2C3D4E5F6", ""},
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        for (const auto& d : kFakeDevices) {
            {
                std::lock_guard<std::mutex> lk(g_mu);
                if (gen != g_scan_gen)
                    return;  // 已被新扫描/切模作废
            }
            Callbacks cbs = SnapshotCbs();
            if (cbs.on_device_found)
                cbs.on_device_found(d);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (gen != g_scan_gen)
                return;
        }
        SetConn(ConnState::Idle);  // 扫描窗口结束
    }).detach();
}

void Connect(const std::string& addr_hex) {
    std::fprintf(stderr, "[sim][bt] Connect(%s)\n", addr_hex.c_str());
    int gen;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        gen = ++g_scan_gen;  // 连接也作废在途扫描
    }
    SetConn(ConnState::Connecting);
    std::thread([gen]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (gen != g_scan_gen)
                return;
        }
        SetConn(ConnState::Connected);
        Callbacks cbs = SnapshotCbs();
        if (cbs.on_status_text)
            cbs.on_status_text("CONNECT SUCCESS");
    }).detach();
}

void EnterCallMode() {}
void EnterMusicMode() {}

void PowerCycle() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_mode = Mode::None;
    g_conn = ConnState::Idle;
    g_scan_gen++;
}

Mode GetMode() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_mode;
}

ConnState GetConnState() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_conn;
}

}  // namespace mhal::bt
