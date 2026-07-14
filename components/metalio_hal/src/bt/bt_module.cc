// BT 音频模组控制 —— 自旧 bluetooth_screen.cc 剥离的硬件/协议部分：
// AT 指令序列、UART 回包解析状态机、IOExpander 电源复位。原先直接写
// LVGL 的地方（post_status/add_device_to_list/lv_async_call 刷按钮）全部
// 翻转为 Callbacks 观察者；RX 解析从「屏幕 LOAD 期间才注册」改为
// InitBtModule() 后常驻（开机 "SET MODE 1" 回包不再丢失）。
#include "metalio_hal/bluetooth.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
#include "SimpleUart.hpp"
#include "config.h"
#include "hal_internal.h"

#define TAG "mhal_bt"

namespace mhal::bt {
namespace {

constexpr int kAddrHexLen = 12;

Mode s_active_mode = Mode::None;
ConnState s_conn_state = ConnState::Idle;
std::string s_rx_buffer;
Callbacks s_cbs;
std::mutex s_cbs_mutex;

// AT 序列必须原子：模组协议要求同一组指令间的 700ms/200ms 间隔不可被打断，
// 而 ModeCmdTask/CallModeTask/MusicModeTask/BtResetTask 都是各自开任务、可能
// 被快速连点交织触发。用一把模块级互斥量把每个任务函数体整体串行化——
// 快速连点会排队依次执行，而不是交织出半截 AT 序列。
SemaphoreHandle_t s_at_seq_mutex = nullptr;

void EmitStatus(const char* text) {
    std::lock_guard<std::mutex> lock(s_cbs_mutex);
    if (s_cbs.on_status_text) s_cbs.on_status_text(text);
}

void SetModeState(Mode m) {
    s_active_mode = m;
    std::lock_guard<std::mutex> lock(s_cbs_mutex);
    if (s_cbs.on_mode_changed) s_cbs.on_mode_changed(m);
}

void SetConnState(ConnState st) {
    s_conn_state = st;
    std::lock_guard<std::mutex> lock(s_cbs_mutex);
    if (s_cbs.on_conn_state) s_cbs.on_conn_state(st);
}

void EmitDeviceFound(const char* address, const char* name) {
    std::lock_guard<std::mutex> lock(s_cbs_mutex);
    if (s_cbs.on_device_found) s_cbs.on_device_found(Device{address, name});
}

bool IsHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void TrimLine(std::string& line) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    size_t start = 0;
    while (start < line.size() && line[start] == ' ') {
        ++start;
    }
    if (start > 0) {
        line = line.substr(start);
    }
}

// 设备行格式："AT+BT:" + 12 位 hex 地址 + 名称
bool ParseBtDeviceLine(const std::string& line, char* address, size_t addr_sz, char* name,
                       size_t name_sz) {
    constexpr const char* kPrefix = "AT+BT:";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }
    const std::string payload = line.substr(strlen(kPrefix));
    if (payload.size() < static_cast<size_t>(kAddrHexLen)) {
        return false;
    }
    for (int i = 0; i < kAddrHexLen; ++i) {
        if (!IsHexChar(payload[i])) {
            return false;
        }
    }
    snprintf(address, addr_sz, "%.*s", kAddrHexLen, payload.c_str());
    snprintf(name, name_sz, "%s", payload.c_str() + kAddrHexLen);
    return true;
}

void HandleResponseLine(const std::string& raw_line) {
    std::string line = raw_line;
    TrimLine(line);
    if (line.empty()) {
        return;
    }

    ESP_LOGI(TAG, "RX: %s", line.c_str());

    if (line.find("SET MODE 1") != std::string::npos) {
        SetModeState(Mode::Rx);
        SetConnState(ConnState::Idle);
        EmitStatus("模式1 已设置");
        return;
    }
    if (line.find("SET MODE 2") != std::string::npos) {
        SetModeState(Mode::Tx);
        SetConnState(ConnState::Idle);
        EmitStatus("模式2 已设置，可扫描设备");
        return;
    }
    if (line.find("SET MODE 3") != std::string::npos) {
        SetModeState(Mode::MusicRx);
        SetConnState(ConnState::Idle);
        EmitStatus("模式3 已设置");
        return;
    }

    if (line.find("RECONNECT") != std::string::npos) {
        EmitStatus(line.c_str());
        return;
    }

    if (line.find("INQUIRING START") != std::string::npos) {
        SetConnState(ConnState::Scanning);
        EmitStatus("正在扫描...");
        return;
    }

    char address[kAddrHexLen + 1];
    char name[64];
    if (ParseBtDeviceLine(line, address, sizeof(address), name, sizeof(name))) {
        EmitDeviceFound(address, name);
        char status[96];
        snprintf(status, sizeof(status), "发现设备: %s", name[0] ? name : address);
        EmitStatus(status);
        return;
    }

    if (line.find("INQ COMPLETE") != std::string::npos) {
        SetConnState(ConnState::Idle);
        EmitStatus("扫描完成");
        return;
    }

    if (line.find("CONNECTING") != std::string::npos) {
        SetConnState(ConnState::Connecting);
        EmitStatus("正在连接...");
        return;
    }

    if (line.find("CONNECT SUCCESS") != std::string::npos) {
        SetConnState(ConnState::Connected);
        EmitStatus("连接成功");
        return;
    }

    if (line.find("CONNECT TIMEOUT") != std::string::npos) {
        SetConnState(ConnState::Idle);
        EmitStatus("连接失败 (超时)");
        return;
    }

    if (line.find("SETUP SCO") != std::string::npos) {
        EmitStatus("通话模式 (SCO 已建立)");
        return;
    }

    if (line.find("DISC SCO") != std::string::npos) {
        EmitStatus("音乐模式 (SCO 已断开)");
        return;
    }

    EmitStatus(line.c_str());
}

void OnUartData(const std::vector<uint8_t>& data) {
    s_rx_buffer.append(data.begin(), data.end());

    size_t pos = 0;
    while (true) {
        size_t nl = s_rx_buffer.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = s_rx_buffer.substr(pos, nl - pos);
        HandleResponseLine(line);
        pos = nl + 1;
    }
    if (pos > 0) {
        s_rx_buffer.erase(0, pos);
    }

    if (s_rx_buffer.size() > 2048) {
        ESP_LOGW(TAG, "RX buffer overflow, clearing");
        s_rx_buffer.clear();
    }
}

void ModeCmdTask(void* param) {
    const Mode mode = static_cast<Mode>(reinterpret_cast<intptr_t>(param));
    SimpleUart& uart = SimpleUart::getInstance();

    xSemaphoreTake(s_at_seq_mutex, portMAX_DELAY);

    // 两条 AT 之间 700ms 间隔是模组协议要求。
    switch (mode) {
        case Mode::Rx:
            EmitStatus("切换模式1...");
            uart.sendString("AT+RX=2\r\n");
            ESP_LOGI(TAG, "TX: AT+RX=2");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=1\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=1");
            break;
        case Mode::Tx:
            EmitStatus("切换模式2...");
            uart.sendString("AT+TX=1\r\n");
            ESP_LOGI(TAG, "TX: AT+TX=1");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=2\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=2");
            break;
        case Mode::MusicRx:
            EmitStatus("切换模式3...");
            uart.sendString("AT+RX=1\r\n");
            ESP_LOGI(TAG, "TX: AT+RX=1");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=3\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=3");
            break;
        default:
            break;
    }

    xSemaphoreGive(s_at_seq_mutex);
    vTaskDelete(nullptr);
}

void SendModeCommand(Mode mode) {
    if (!SimpleUart::getInstance().isInitialized()) {
        EmitStatus("UART 未初始化");
        ESP_LOGE(TAG, "SimpleUart not initialized");
        return;
    }
    xTaskCreate(ModeCmdTask, "bt_mode_cmd", 4096,
                reinterpret_cast<void*>(static_cast<intptr_t>(mode)), 5, nullptr);
}

void CallModeTask(void*) {
    xSemaphoreTake(s_at_seq_mutex, portMAX_DELAY);
    SimpleUart& uart = SimpleUart::getInstance();
    EmitStatus("切换通话模式...");
    uart.sendString("AT+PP=1\r\n");
    ESP_LOGI(TAG, "TX: AT+PP=1");
    vTaskDelay(pdMS_TO_TICKS(200));
    uart.sendString("AT+BTSCO=1\r\n");
    ESP_LOGI(TAG, "TX: AT+BTSCO=1");
    xSemaphoreGive(s_at_seq_mutex);
    vTaskDelete(nullptr);
}

void MusicModeTask(void*) {
    xSemaphoreTake(s_at_seq_mutex, portMAX_DELAY);
    SimpleUart& uart = SimpleUart::getInstance();
    EmitStatus("切换音乐模式...");
    uart.sendString("AT+BTSCO=0\r\n");
    ESP_LOGI(TAG, "TX: AT+BTSCO=0");
    vTaskDelay(pdMS_TO_TICKS(200));
    uart.sendString("AT+PP=1\r\n");
    ESP_LOGI(TAG, "TX: AT+PP=1");
    xSemaphoreGive(s_at_seq_mutex);
    vTaskDelete(nullptr);
}

void BtResetTask(void*) {
    xSemaphoreTake(s_at_seq_mutex, portMAX_DELAY);
    EmitStatus("正在复位蓝牙...");
    auto& io = IOExpander::getInstance();
    io.setLevel(IOExpander::Pin::BT_POWER, false);
    ESP_LOGI(TAG, "BT_POWER off");
    vTaskDelay(pdMS_TO_TICKS(300));
    io.setLevel(IOExpander::Pin::BT_POWER, true);
    ESP_LOGI(TAG, "BT_POWER on");
    SetModeState(Mode::None);
    SetConnState(ConnState::Idle);
    EmitStatus("蓝牙电源已复位");
    xSemaphoreGive(s_at_seq_mutex);
    vTaskDelete(nullptr);
}

}  // namespace

void SetCallbacks(Callbacks cbs) {
    std::lock_guard<std::mutex> lock(s_cbs_mutex);
    s_cbs = std::move(cbs);
}

void ApplyDefaultMode() {
    // 预置内部状态为模式1：即使模组没回 "SET MODE 1"，GetMode() 也能反映
    // 当前硬件模式（与旧固件 BluetoothScreen::ApplyDefaultMode 语义一致）。
    s_active_mode = Mode::Rx;
    SendModeCommand(Mode::Rx);
}

void SetMode(Mode m) {
    if (m == Mode::None) {
        return;
    }
    SendModeCommand(m);
}

void StartScan() {
    if (s_active_mode != Mode::Tx) {
        EmitStatus("请先切换到模式2");
        return;
    }
    if (!SimpleUart::getInstance().isInitialized()) {
        EmitStatus("UART 未初始化");
        return;
    }
    SimpleUart::getInstance().sendString("AT+INQUIRING\r\n");
    ESP_LOGI(TAG, "TX: AT+INQUIRING");
    EmitStatus("开始扫描...");
}

void Connect(const std::string& addr_hex) {
    if (!SimpleUart::getInstance().isInitialized()) {
        EmitStatus("UART 未初始化");
        return;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s\r\n", addr_hex.c_str());
    SimpleUart::getInstance().sendString(cmd);
    ESP_LOGI(TAG, "TX: AT+CONNECT=%s", addr_hex.c_str());
    SetConnState(ConnState::Connecting);
}

void EnterCallMode() {
    if (s_conn_state != ConnState::Connected) {
        EmitStatus("请先连接蓝牙设备");
        return;
    }
    xTaskCreate(CallModeTask, "bt_call_mode", 4096, nullptr, 5, nullptr);
}

void EnterMusicMode() {
    if (s_conn_state != ConnState::Connected) {
        EmitStatus("请先连接蓝牙设备");
        return;
    }
    xTaskCreate(MusicModeTask, "bt_music_mode", 4096, nullptr, 5, nullptr);
}

void PowerCycle() { xTaskCreate(BtResetTask, "bt_reset", 4096, nullptr, 5, nullptr); }

Mode GetMode() { return s_active_mode; }

ConnState GetConnState() { return s_conn_state; }

}  // namespace mhal::bt

namespace mhal::internal {

void InitBtModule(bool apply_default_mode) {
    if (mhal::bt::s_at_seq_mutex == nullptr) {
        mhal::bt::s_at_seq_mutex = xSemaphoreCreateMutex();
    }

    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.begin(BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN, 115200, UART_NUM_2)) {
        ESP_LOGE(TAG, "BT UART initialization failed");
        return;
    }
    ESP_LOGI(TAG, "BT UART initialized");
    uart.registerCallback(mhal::bt::OnUartData);
    if (apply_default_mode) {
        mhal::bt::ApplyDefaultMode();
    }
}

}  // namespace mhal::internal
