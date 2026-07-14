#include "nt26_modem.h"

#include <esp_log.h>
#include <esp_netif.h>
#include "freertos/task.h"

#define TAG "Nt26Modem"

using mhal::network::Event;

namespace {

constexpr uint32_t kWaitNetworkConnected = (1 << 0);
constexpr uint32_t kWaitNetworkFailed = (1 << 1);
constexpr uint32_t kWaitNetworkInFlight = (1 << 2);
constexpr uint32_t kWaitNetworkAll =
    kWaitNetworkConnected | kWaitNetworkFailed | kWaitNetworkInFlight;
constexpr uint32_t kNetworkWaitTimeoutMs = 60 * 1000;

}  // namespace

Nt26Modem::Nt26Modem(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t mrdy_pin,
                     gpio_num_t srdy_pin)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), mrdy_pin_(mrdy_pin), srdy_pin_(srdy_pin) {
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    esp_event_loop_create_default();
    esp_netif_init();

    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "nt26_cpu", &pm_lock_cpu_max_);

    StartSignalWorker();
}

Nt26Modem::~Nt26Modem() {
    // 先停后台刷新任务——它持有 modem_ 指针并可能正阻塞在 SendAt 上，
    // 必须在 modem_->Stop() 之前 join，避免 use-after-free。
    StopSignalWorker();

    if (auto* eg = network_wait_event_.exchange(nullptr)) {
        vEventGroupDelete(eg);
    }

    if (modem_) {
        modem_->Stop();
    }

    if (pm_lock_cpu_max_) {
        esp_pm_lock_delete(pm_lock_cpu_max_);
    }
}

void Nt26Modem::OnEvent(Event event, const std::string& data) {
    if (event_callback_) {
        event_callback_(event, data);
    }
}

bool Nt26Modem::Start() {
    OnEvent(Event::ModemDetecting);

    UartEthModem::Config config = {
        .uart_num = UART_NUM_1,
        .baud_rate = 2000000,
        .tx_pin = tx_pin_,
        .rx_pin = rx_pin_,
        .mrdy_pin = mrdy_pin_,
        .srdy_pin = srdy_pin_,
    };

    EventGroupHandle_t eg = xEventGroupCreate();
    if (!eg) {
        ESP_LOGE(TAG, "Failed to create network wait event group");
        OnEvent(Event::CellularErrorInitFailed);
        return false;
    }
    network_wait_event_.store(eg);

    modem_ = std::make_unique<UartEthModem>(config);
    modem_->SetDebug(false);
    modem_->SetNetworkEventCallback([this](UartEthModem::UartEthModemEvent event) {
        ESP_LOGI(TAG, "Modem event: %s", UartEthModem::GetNetworkEventName(event));
        // 回调运行在 modem 任务上下文：load 到局部再判空，避免与 Start()
        // 摘句柄的 exchange 竞争后写已释放句柄。
        switch (event) {
            case UartEthModem::UartEthModemEvent::Connected:
                if (auto* eg = network_wait_event_.load()) {
                    xEventGroupSetBits(eg, kWaitNetworkConnected);
                }
                OnEvent(Event::CellularConnected);
                break;
            case UartEthModem::UartEthModemEvent::Disconnected:
                OnEvent(Event::CellularDisconnected);
                break;
            case UartEthModem::UartEthModemEvent::ErrorNoSim:
                if (auto* eg = network_wait_event_.load()) {
                    xEventGroupSetBits(eg, kWaitNetworkFailed);
                }
                AsyncStop();
                OnEvent(Event::CellularErrorNoSim);
                break;
            case UartEthModem::UartEthModemEvent::ErrorRegistrationDenied:
                if (auto* eg = network_wait_event_.load()) {
                    xEventGroupSetBits(eg, kWaitNetworkFailed);
                }
                AsyncStop();
                OnEvent(Event::CellularErrorRegDenied);
                break;
            case UartEthModem::UartEthModemEvent::Connecting:
                OnEvent(Event::CellularConnecting);
                break;
            case UartEthModem::UartEthModemEvent::ErrorInitFailed:
            case UartEthModem::UartEthModemEvent::ErrorNoCarrier:
                if (auto* eg = network_wait_event_.load()) {
                    xEventGroupSetBits(eg, kWaitNetworkFailed);
                }
                AsyncStop();
                OnEvent(Event::CellularErrorInitFailed);
                break;
            case UartEthModem::UartEthModemEvent::InFlightMode:
                ESP_LOGW(TAG, "Modem in flight mode");
                if (auto* eg = network_wait_event_.load()) {
                    xEventGroupSetBits(eg, kWaitNetworkInFlight);
                }
                break;
            case UartEthModem::UartEthModemEvent::RequestingPdpContext:
                break;
        }
    });

    if (modem_->Start() != ESP_OK) {
        if (auto* g = network_wait_event_.exchange(nullptr)) {
            vEventGroupDelete(g);
        }
        OnEvent(Event::CellularErrorInitFailed);
        return false;
    }

    OnEvent(Event::CellularConnecting);

    // 唯一权威时限：等 60s 事件组（原 30s network_ready_timer_ 已删除，
    // 消除"30s 报超时、45s 又冒出已连接"的 late-connect）。
    ESP_LOGI(TAG, "Waiting for network ready...");
    EventBits_t bits = xEventGroupWaitBits(eg, kWaitNetworkAll, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(kNetworkWaitTimeoutMs));
    // 先原子摘下句柄再删，之后 modem 回调 load 到 nullptr 即不再 SetBits。
    if (auto* g = network_wait_event_.exchange(nullptr)) {
        vEventGroupDelete(g);
    }

    if (bits & kWaitNetworkConnected) {
        ESP_LOGI(TAG, "Network ready");
        return true;
    }
    if (bits & kWaitNetworkInFlight) {
        // 无 SIM / flight：保留 modem（AT 通道可用），不停机。
        ESP_LOGW(TAG, "Network unavailable (flight mode / no SIM)");
        return false;
    }
    if (bits & kWaitNetworkFailed) {
        // 错误回调已通过 AsyncStop() 停机，这里直接返回。
        ESP_LOGW(TAG, "Network registration failed");
        return false;
    }

    // 纯超时（60s 内无任何事件）：同步停机，杜绝返回后 modem 迟到 Connected。
    ESP_LOGW(TAG, "Network wait timed out, stopping modem");
    if (modem_) {
        modem_->Stop();
    }
    return false;
}

void Nt26Modem::AsyncStop() {
    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<Nt26Modem*>(arg);
            if (self->modem_) {
                self->modem_->Stop();
            }
            vTaskDelete(nullptr);
        },
        "nt26_async_stop", 4096, this, tskIDLE_PRIORITY + 1, nullptr);
}

void Nt26Modem::SetEventCallback(mhal::network::EventCallback callback) {
    event_callback_ = std::move(callback);
}

int Nt26Modem::GetSignalStrength() {
    if (modem_ == nullptr || !modem_->IsInitialized()) {
        return -1;
    }
    // 触发一次后台刷新，立即返回上一次缓存读数（不阻塞调用线程）。
    if (signal_nudge_) {
        xSemaphoreGive(signal_nudge_);
    }
    return cached_csq_.load();
}

Nt26CeregState Nt26Modem::GetRegistrationState() {
    Nt26CeregState state;
    if (modem_ == nullptr) {
        return state;
    }
    if (signal_nudge_) {
        xSemaphoreGive(signal_nudge_);
    }
    std::lock_guard<std::mutex> lock(cell_mutex_);
    state.stat = cached_cell_.stat;
    state.tac = cached_cell_.tac;
    state.ci = cached_cell_.ci;
    state.AcT = cached_cell_.act;
    return state;
}

void Nt26Modem::StartSignalWorker() {
    signal_nudge_ = xSemaphoreCreateBinary();
    if (signal_nudge_ == nullptr) {
        ESP_LOGE(TAG, "signal worker: nudge semaphore alloc failed");
        return;
    }
    signal_task_stop_.store(false);
    xTaskCreate(
        [](void* arg) { static_cast<Nt26Modem*>(arg)->SignalWorkerRun(); },
        "nt26_signal", 4096, this, tskIDLE_PRIORITY + 3, &signal_task_);
}

void Nt26Modem::StopSignalWorker() {
    if (signal_task_ == nullptr) {
        return;
    }
    signal_task_stop_.store(true);
    if (signal_nudge_) {
        xSemaphoreGive(signal_nudge_);  // 唤醒可能在等 nudge 的任务
    }
    // 等任务自删退出（它可能正阻塞在一次 SendAt 上，等它跑完当前刷新）。
    while (signal_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (signal_nudge_) {
        vSemaphoreDelete(signal_nudge_);
        signal_nudge_ = nullptr;
    }
}

void Nt26Modem::SignalWorkerRun() {
    while (!signal_task_stop_.load()) {
        // 纯按需：无 UI 读取就一直阻塞，待机零轮询。
        xSemaphoreTake(signal_nudge_, portMAX_DELAY);
        if (signal_task_stop_.load()) {
            break;
        }
        if (modem_ == nullptr) {
            continue;
        }
        // CSQ 与原 Nt26Modem::GetSignalStrength 一样受 IsInitialized 门控。
        if (modem_->IsInitialized()) {
            cached_csq_.store(modem_->GetSignalStrength());
        }
        // CEREG 与原 GetRegistrationState 一样只要 modem 实例存在就查。
        UartEthModem::CellInfo ci = modem_->GetCellInfo();
        {
            std::lock_guard<std::mutex> lock(cell_mutex_);
            cached_cell_ = ci;
        }
    }
    signal_task_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t Nt26Modem::SendAtCommand(const std::string& cmd, std::string& response,
                                   uint32_t timeout_ms, bool bypass_init_check) {
    if (!modem_) {
        ESP_LOGW(TAG, "SendAtCommand: modem 未实例化（当前可能是 WiFi 模式）");
        return ESP_ERR_INVALID_STATE;
    }
    if (!bypass_init_check && !modem_->IsInitialized()) {
        ESP_LOGW(TAG, "SendAtCommand: modem 尚未完成初始化");
        return ESP_ERR_INVALID_STATE;
    }
    return modem_->SendAt(cmd, response, timeout_ms);
}
