#ifndef NT26_MODEM_H
#define NT26_MODEM_H

// 原 Nt26Board 剥离 xiaozhi Board 基类后的纯 NT26 4G modem 封装
// （UART1 2Mbps + MRDY/SRDY 握手 + iot_eth netif）。事件不再写
// Display，统一走 mhal::network::Event 回调。
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <uart_eth_modem.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
    // 非阻塞：返回后台任务缓存的上一次读数，并顺带触发一次异步刷新。
    // 绝不在调用线程发同步 AT（原实现会阻塞，UI 线程调用会卡渲染）。
    int GetSignalStrength();

    // 非阻塞：同 GetSignalStrength，返回缓存的注册态并触发异步刷新。
    Nt26CeregState GetRegistrationState();

    // 4G netif 只读句柄（iot_eth 起网后有效；未起网/未初始化返回 nullptr）。
    // 供 network.cc 取 IP 等 netif 级信息用，不转移所有权。
    esp_netif_t* GetNetif() const { return modem_ ? modem_->GetNetif() : nullptr; }

    // 转发到 UartEthModem::SendAt。线程安全（modem 内部用 mutex 串行化）。
    // modem 未实例化或未初始化时返回 ESP_ERR_INVALID_STATE。
    // bypass_init_check=true: 只要 modem 实例存在就直接转发——外置 SIM 卡
    // 没插时 modem 不会进入 initialized 状态，但 AT 通道本身是通的，
    // 切卡场景必须能下发。
    esp_err_t SendAtCommand(const std::string& cmd, std::string& response,
                            uint32_t timeout_ms = 5000, bool bypass_init_check = false);

private:
    void OnEvent(mhal::network::Event event, const std::string& data = "");
    // 在 modem 回调上下文之外异步 Stop()（原 Application::Schedule 语义，
    // 用一次性 task 实现）。
    void AsyncStop();

    // CSQ/CEREG 后台刷新任务：UI getter 只读缓存并 nudge，本任务在后台
    // 线程发同步 AT 并回填缓存，把阻塞从 UI 线程剥离。纯按需（无 nudge
    // 不轮询），故待机零额外功耗。
    void StartSignalWorker();
    void StopSignalWorker();
    void SignalWorkerRun();

    std::unique_ptr<UartEthModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t mrdy_pin_;
    gpio_num_t srdy_pin_;

    mhal::network::EventCallback event_callback_;
    esp_pm_lock_handle_t pm_lock_cpu_max_ = nullptr;
    // modem 回调（modem 任务上下文）与 Start()（网络任务）并发访问：Start
    // 用 exchange(nullptr) 摘下句柄后再删，回调 load 到局部非空才 SetBits。
    // 注意这只收窄不消灭窗口（回调 load 到非空后被抢占、Start 恰好删除仍可
    // 命中已释放句柄）——常态路径靠"超时/失败即 Stop 停机"保证无存活回调，
    // 此处是针对极端时序的保险层。
    std::atomic<EventGroupHandle_t> network_wait_event_{nullptr};

    // 信号/注册态缓存 + 后台刷新任务
    TaskHandle_t signal_task_ = nullptr;
    SemaphoreHandle_t signal_nudge_ = nullptr;
    std::atomic<bool> signal_task_stop_{false};
    std::atomic<int> cached_csq_{99};
    std::mutex cell_mutex_;
    UartEthModem::CellInfo cached_cell_;
};

#endif  // NT26_MODEM_H
