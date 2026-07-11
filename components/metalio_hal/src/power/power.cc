// 电池/电源：BQ27220 门面 + 开机电量保护 + 无线充（0x60）电流配置监控。
// 后两者原样自 metalio-claw-4.cc（CheckBatteryLevelAtBoot / Wxcho / I2cWxchoTask）。
#include "metalio_hal/power.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
#include "bq27220_gauge.h"
#include "hal_internal.h"
#include "i2c_device.h"

#define TAG "mhal_power"

namespace mhal {
namespace {

// 无线充电芯片（I2C 0x60，热插拔：放上充电板才在线）。
class Wxcho : public I2cDevice {
public:
    Wxcho(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        ESP_LOGW(TAG, "Device found at address 0x60,init");
    }

    /*
     * MTP_ILIM_SET 寄存器 0x1E 位段[2:0]: 过流保护限流值
     *   0x00:1.4A 0x01:1.65A 0x02:1.1A 0x03:0.74A
     *   0x04:0.365A 0x05:0.45A 0x06:0.29A 0x07:0.215A
     * 0x15: 温度保护开关
     */
    void ConfigureChargeCurrent() {
        WriteReg(0x1e, 0x00);  // 设置充电电流
        ESP_LOGW(TAG, "write 0X1E reg: 0x00");
        WriteReg(0x15, 0x00);  // 关闭温度保护
        ESP_LOGW(TAG, "write 0x15 reg: 0x00");
    }
};

void WirelessChargeMonitorTask(void*) {
    Wxcho* wxcho = nullptr;
    bool last_found = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        const bool found = i2c_master_probe(internal::I2cBus(), 0x60, 100) == ESP_OK;
        if (found) {
            if (wxcho == nullptr) {
                wxcho = new Wxcho(internal::I2cBus(), 0x60);
            }
            // 每次重新放上充电板都重写限流配置（芯片掉电会丢配置）。
            if (!last_found) {
                wxcho->ConfigureChargeCurrent();
            }
        }
        last_found = found;
    }
}

}  // namespace

namespace internal {

void BatteryBootGuard() {
    auto& gauge = Bq27220Gauge::GetInstance();
    int level = 0;
    bool charging = false;
    bool discharging = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (gauge.GetBatteryLevel(level, charging, discharging)) {
            ESP_LOGI(TAG, "Boot battery check: level=%d%%, charging=%s", level,
                     charging ? "true" : "false");
            if (level == 0 && !charging) {
                ESP_LOGW(TAG, "Battery 0%%, forcing power off");
                power::ForcePowerOff();
            }
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "Boot battery check: gauge unavailable, skip shutdown");
}

void StartWirelessChargeMonitor() {
    BaseType_t ret = xTaskCreatePinnedToCore(WirelessChargeMonitorTask, "i2c_wxcho_task",
                                             4 * 1024, nullptr, 5, nullptr, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wireless charge monitor task");
    }
}

}  // namespace internal

namespace power {

bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
    return Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging);
}

bool GetVoltageMv(uint16_t& mv) { return Bq27220Gauge::GetInstance().GetVoltageMv(mv); }

bool GetCurrentMa(int16_t& ma) { return Bq27220Gauge::GetInstance().ReadCurrentMa(ma); }

void ForcePowerOff() {
    auto& io = IOExpander::getInstance();
    constexpr int kPulseHalfMs = 100;
    constexpr int kPulseCount = 10;
    for (int i = 0; i < kPulseCount; ++i) {
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
    }
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

}  // namespace power
}  // namespace mhal
