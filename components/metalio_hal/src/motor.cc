#include "metalio_hal/motor.h"

#include <algorithm>

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/ledc.h>

#define TAG "Motor"

namespace mhal::motor {

namespace {

// !! 不能用 LEDC_TIMER_0 / LEDC_CHANNEL_0 —— 那是 backlight.cc 的
// PwmBacklight 占用的，共用会把振动写 duty 变成改背光亮度，并把
// channel 0 的 GPIO 输出重新映射。
constexpr gpio_num_t       kGpio        = GPIO_NUM_22;
constexpr ledc_mode_t      kMode        = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t     kTimer       = LEDC_TIMER_1;
constexpr ledc_channel_t   kChannel     = LEDC_CHANNEL_1;
constexpr ledc_timer_bit_t kDutyRes     = LEDC_TIMER_10_BIT;  // 0..1023
constexpr uint32_t         kFreqHz      = 5000;
constexpr uint32_t         kDutyMax     = (1U << kDutyRes) - 1U;

bool s_ledc_inited = false;
esp_timer_handle_t s_stop_timer = nullptr;

void StopTimerCallback(void* /*arg*/) { Stop(); }

}  // namespace

void Init() {
    if (s_ledc_inited) {
        return;
    }

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = kMode;
    timer_cfg.timer_num       = kTimer;
    timer_cfg.duty_resolution = kDutyRes;
    timer_cfg.freq_hz         = kFreqHz;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num   = kGpio;
    ch_cfg.speed_mode = kMode;
    ch_cfg.channel    = kChannel;
    ch_cfg.intr_type  = LEDC_INTR_DISABLE;
    ch_cfg.timer_sel  = kTimer;
    ch_cfg.duty       = 0;
    ch_cfg.hpoint     = 0;
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &StopTimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "motor_stop",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_stop_timer));

    s_ledc_inited = true;
    ESP_LOGI(TAG, "LEDC ready: gpio=%d freq=%lu res=%d-bit", kGpio,
             static_cast<unsigned long>(kFreqHz), kDutyRes);
}

void Buzz(uint32_t duration_ms, int strength_pct) {
    Init();
    if (!s_ledc_inited) {
        return;
    }

    // 重复调用先取消上一次挂起的自动停止定时器，避免叠加/提前停振。
    esp_timer_stop(s_stop_timer);  // 若未运行，返回 ESP_ERR_INVALID_STATE，忽略即可

    strength_pct = std::clamp(strength_pct, 0, 100);
    uint32_t duty = (static_cast<uint32_t>(strength_pct) * kDutyMax) / 100U;
    ledc_set_duty(kMode, kChannel, duty);
    ledc_update_duty(kMode, kChannel);

    if (duration_ms > 0) {
        esp_timer_start_once(s_stop_timer, static_cast<uint64_t>(duration_ms) * 1000ULL);
    }
}

void Stop() {
    if (!s_ledc_inited) {
        return;
    }
    if (s_stop_timer != nullptr) {
        esp_timer_stop(s_stop_timer);
    }
    ledc_set_duty(kMode, kChannel, 0);
    ledc_update_duty(kMode, kChannel);
}

}  // namespace mhal::motor
