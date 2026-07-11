// mhal::Init —— 原 METALIO_CLAW_4 板级构造函数的去业务化版本。
// 顺序与旧板一致（板级验证过）：I2C → IOExpander 上电序列 → 电量计 →
// 开机电量保护 → BT 模组 → SD → LCD → 触摸 → LVGL → 无线充监控 → 背光。
#include "metalio_hal/hal.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
#include "SdCardManager.hpp"
#include "bq27220_gauge.h"
#include "config.h"
#include "hal_internal.h"
#include "metalio_hal/backlight.h"

#define TAG "mhal"

namespace mhal {

namespace {
i2c_master_bus_handle_t s_i2c_bus = nullptr;

void InitI2c() {
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = (i2c_port_t)1,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &s_i2c_bus));
}

void InitIoExpander() {
    auto& io = IOExpander::getInstance();
    io.begin(s_i2c_bus);
    io.setLevel(IOExpander::Pin::BT_POWER, true);
    io.setLevel(IOExpander::Pin::PA, true);
    io.setLevel(IOExpander::Pin::PA_SWITCH, true);
    io.setLevel(IOExpander::Pin::RST_4G, true);
    // CAM_PWDN 高 = 摄像头断电（本固件无相机功能，保持断电省电）。
    io.setLevel(IOExpander::Pin::CAM_PWDN, true);
    io.setLevel(IOExpander::Pin::SD, false);
}
}  // namespace

namespace internal {
i2c_master_bus_handle_t I2cBus() { return s_i2c_bus; }
}  // namespace internal

esp_err_t Init(const InitOptions& opts) {
    InitI2c();
    InitIoExpander();

    // 电量计挂上 I2C；失败不致命（GetBatteryLevel 内部节流自愈）。
    (void)Bq27220Gauge::GetInstance().Begin(s_i2c_bus);
    if (opts.battery_boot_guard) {
        internal::BatteryBootGuard();
    }

    internal::InitBtModule(opts.bt_default_mode);

    if (opts.mount_sd_card) {
        if (!SdCardManager::GetInstance().Mount()) {
            ESP_LOGW(TAG, "SD card not mounted at boot (card may be absent)");
        }
    }

    // LCD 上电稳定后再初始化 GT911，最后起 LVGL（触摸已就绪）。
    internal::InitPanel();
    vTaskDelay(pdMS_TO_TICKS(100));
    internal::InitTouch();
    internal::StartLvglAdapter();

    internal::StartWirelessChargeMonitor();

    if (opts.restore_backlight) {
        backlight::Restore();
    }

    ESP_LOGI(TAG, "hardware init done");
    return ESP_OK;
}

}  // namespace mhal
