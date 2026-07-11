// 系统监控任务，原样自 metalio-claw-4.cc 板级构造函数内的匿名 lambda。
#include "metalio_hal/sysmon.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bq27220_gauge.h"

namespace mhal::sysmon {
namespace {

uint32_t s_period_ms = 1000;
bool s_started = false;

void MonitorTask(void*) {
    auto& gauge = Bq27220Gauge::GetInstance();

    // ---- ESP32-P4 双核 CPU 占用率采样 ----
    // 依赖 sdkconfig：CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y、
    // CONFIG_FREERTOS_USE_TRACE_FACILITY=y、
    // CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y。
    // 每核的 Idle Task 只在无其他 task 时被调度；两次采样
    // ulTaskGetIdleRunTimeCounterForCore()（单位 us）做差即得空闲时长：
    //   usage% = 100 - idle_delta_us * 100 / total_delta_us
    constexpr int kCoreCount = portNUM_PROCESSORS;
    configRUN_TIME_COUNTER_TYPE prev_idle[kCoreCount] = {0};
    for (int c = 0; c < kCoreCount; ++c) {
        prev_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    }
    uint64_t prev_us = (uint64_t)esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(s_period_ms));

        uint64_t now_us = (uint64_t)esp_timer_get_time();
        uint64_t dt_us = now_us - prev_us;
        int usage[kCoreCount] = {0};
        int total_usage = 0;
        if (dt_us > 0) {
            for (int c = 0; c < kCoreCount; ++c) {
                configRUN_TIME_COUNTER_TYPE now_idle = ulTaskGetIdleRunTimeCounterForCore(c);
                configRUN_TIME_COUNTER_TYPE didle = now_idle - prev_idle[c];
                uint64_t idle_pct = (uint64_t)didle * 100ULL / dt_us;
                if (idle_pct > 100) idle_pct = 100;
                usage[c] = 100 - (int)idle_pct;
                total_usage += usage[c];
                prev_idle[c] = now_idle;
            }
        }
        prev_us = now_us;
        const int avg_usage = (kCoreCount > 0) ? (total_usage / kCoreCount) : 0;
        const int core1_usage = (kCoreCount > 1) ? usage[1] : 0;

        constexpr const char* kMonitorTag = "系统监控";
        ESP_LOGI(kMonitorTag, "@@@CPU   | 内核0: %3d%% | 内核1: %3d%% | 平均: %3d%%",
                 usage[0], core1_usage, avg_usage);

        const unsigned free_kb =
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        const unsigned min_free_kb =
            static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
        ESP_LOGI(kMonitorTag, "@@@内存  | 剩余: %6u KB | 历史最小: %6u KB", free_kb,
                 min_free_kb);

        int battery_level;
        bool charging, discharging;
        if (gauge.GetBatteryLevel(battery_level, charging, discharging)) {
            uint16_t mv = 0;
            const bool mv_ok = gauge.GetVoltageMv(mv);
            if (mv_ok) {
                ESP_LOGI(kMonitorTag,
                         "@@@电池  | 电量: %3d%% | 电压: %5u mV | 充电: %s | 放电: %s",
                         battery_level, mv, charging ? "是" : "否", discharging ? "是" : "否");
            } else {
                ESP_LOGI(kMonitorTag,
                         "@@@电池  | 电量: %3d%% | 电压: 读取失败 | 充电: %s | 放电: %s",
                         battery_level, charging ? "是" : "否", discharging ? "是" : "否");
            }
        }
    }
}

}  // namespace

void Start(uint32_t period_ms) {
    if (s_started) {
        return;
    }
    s_started = true;
    s_period_ms = period_ms;
    xTaskCreate(MonitorTask, "_task", 8192, nullptr, 5, nullptr);
}

}  // namespace mhal::sysmon
