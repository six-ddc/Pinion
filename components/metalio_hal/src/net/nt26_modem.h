#ifndef NT26_MODEM_H
#define NT26_MODEM_H

// 原 Nt26Board 剥离 xiaozhi Board 基类后的纯 NT26 4G modem 封装
// （UART1 2Mbps + MRDY/SRDY 握手 + iot_eth netif）。事件不再写
// Display，统一走 mhal::network::Event 回调。
#include <memory>
#include <string>

#include <driver/gpio.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <uart_eth_modem.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "metalio_hal/network.h"

struct Nt26CeregState {
    int stat = 0;
    std::string tac;
    std::string ci;
    int AcT = -1;

    std::string ToString() const {
        std::string json = "{";
        json += "\"stat\":" + std::to_string(stat);
        if (!tac.empty()) json += ",\"tac\":\"" + tac + "\"";
        if (!ci.empty()) json += ",\"ci\":\"" + ci + "\"";
        if (AcT >= 0) json += ",\"AcT\":" + std::to_string(AcT);
        json += "}";
        return json;
    }
};

class Nt26Modem {
public:
    Nt26Modem(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t mrdy_pin, gpio_num_t srdy_pin);
    ~Nt26Modem();

    void SetEventCallback(mhal::network::EventCallback callback);

    // 检测模组 + 起网并等待就绪（阻塞，最长约 60s）。返回是否已连通。
    bool Start();

    // CSQ 0-31；99/-1/modem 未就绪 = -1。
    int GetSignalStrength();

    Nt26CeregState GetRegistrationState();

    // 转发到 UartEthModem::SendAt。线程安全（modem 内部用 mutex 串行化）。
    // modem 未实例化或未初始化时返回 ESP_ERR_INVALID_STATE。
    // bypass_init_check=true: 只要 modem 实例存在就直接转发——外置 SIM 卡
    // 没插时 modem 不会进入 initialized 状态，但 AT 通道本身是通的，
    // 切卡场景必须能下发。
    esp_err_t SendAtCommand(const std::string& cmd, std::string& response,
                            uint32_t timeout_ms = 5000, bool bypass_init_check = false);

private:
    void OnEvent(mhal::network::Event event, const std::string& data = "");
    static void OnNetworkReadyTimeout(void* arg);
    // 在 modem 回调上下文之外异步 Stop()（原 Application::Schedule 语义，
    // 用一次性 task 实现）。
    void AsyncStop();

    std::unique_ptr<UartEthModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t mrdy_pin_;
    gpio_num_t srdy_pin_;

    mhal::network::EventCallback event_callback_;
    esp_pm_lock_handle_t pm_lock_cpu_max_ = nullptr;
    esp_timer_handle_t network_ready_timer_ = nullptr;
    EventGroupHandle_t network_wait_event_ = nullptr;
};

#endif  // NT26_MODEM_H
