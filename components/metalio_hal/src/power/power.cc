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

    // 读不到 gauge 时的重试：沿用原有 5 次 / 100ms 间隔逻辑。
    bool got_reading = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (gauge.GetBatteryLevel(level, charging, discharging)) {
            got_reading = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!got_reading) {
        ESP_LOGW(TAG, "Boot battery check: gauge unavailable, skip shutdown");
        return;
    }

    ESP_LOGI(TAG, "Boot battery check: level=%d%%, charging=%s", level,
             charging ? "true" : "false");
    if (level != 0 || charging) {
        return;
    }

    // 首次采样命中"低电压且未充电"只是嫌疑，不能单次采样即触发不可逆的
    // ForcePowerOff——紧跟在电源轨切换之后，瞬时耦合噪声可能骗到一次假读数。
    // 间隔 ~50ms 连续复采几次防抖：任一次读到正常电压/充电立即放行；
    // 只有全部复采都一致确认"低电压且未充电"才真正断电。
    constexpr int kConfirmSamples = 5;
    int confirmed = 0;
    int low_count = 0;
    for (int i = 0; i < kConfirmSamples; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int confirm_level = 0;
        bool confirm_charging = false;
        bool confirm_discharging = false;
        if (!gauge.GetBatteryLevel(confirm_level, confirm_charging, confirm_discharging)) {
            continue;
        }
        ++confirmed;
        if (confirm_level == 0 && !confirm_charging) {
            ++low_count;
        } else {
            ESP_LOGI(TAG, "Boot battery check: normal reading during confirm, skip shutdown");
            return;
        }
    }

    if (confirmed > 0 && low_count == confirmed) {
        ESP_LOGW(TAG, "Battery 0%% confirmed over %d/%d samples, forcing power off", low_count,
                 confirmed);
        power::ForcePowerOff();
    } else {
        ESP_LOGW(TAG, "Boot battery check: low-battery not confirmed (%d/%d valid), skip shutdown",
                 confirmed, kConfirmSamples);
    }
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

bool GetBatterySnapshot(int& level, bool& charging, bool& discharging) {
    return Bq27220Gauge::GetInstance().GetCachedSnapshot(level, charging, discharging);
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
