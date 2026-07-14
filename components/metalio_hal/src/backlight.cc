#include "backlight.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/ledc.h>

#define TAG "Backlight"


Backlight::Backlight() {
    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<Backlight*>(arg);
            self->OnTransitionTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &transition_timer_));
}

Backlight::~Backlight() {
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
    }
}

void Backlight::RestoreBrightness() {
    // Load brightness from settings
    Settings settings("display");  
    int saved_brightness = settings.GetInt("brightness", kBacklightDefaultPercent);
    
    // 检查亮度值是否过小，抬到允许的最小默认值
    if (saved_brightness < static_cast<int>(kBacklightMinPercent)) {
        ESP_LOGW(TAG, "Brightness value (%d) is too small, setting to default (%d)",
                 saved_brightness, kBacklightMinPercent);
        saved_brightness = kBacklightMinPercent;
    }
    
    SetBrightness(saved_brightness);
}

void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }

    if (permanent) {
        // 无条件落盘：brightness_ 是渐变定时器追赶中的中间态，不能拿它来判断
        // 要不要写 NVS，否则"拖动预览 + 松手落盘"模式下松手那次必被短路吞掉。
        // 用 last_persisted_brightness_ 去重，避免同一目标值重复擦写 NVS。
        if (!persisted_ || last_persisted_brightness_ != brightness) {
            Settings settings("display", true);
            settings.SetInt("brightness", brightness);
            last_persisted_brightness_ = brightness;
            persisted_ = true;
        }
    }

    // 渐变提前返回：改比较 target_brightness_，目标未变且已到位才 return，
    // 不再用会被中间态污染的 brightness_ 判断。
    if (target_brightness_ == brightness && brightness_ == brightness) {
        return;
    }

    target_brightness_ = brightness;
    step_ = (target_brightness_ > brightness_) ? 1 : -1;

    if (transition_timer_ != nullptr) {
        // 启动定时器，每 5ms 更新一次
        esp_timer_start_periodic(transition_timer_, 5 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

void Backlight::OnTransitionTimer() {
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
        return;
    }

    brightness_ += step_;
    SetBrightnessImpl(brightness_);

    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
    }
}

PwmBacklight::PwmBacklight(gpio_num_t pin, bool output_invert, uint32_t freq_hz) : Backlight() {
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz, //背光pwm频率需要高一点，防止电感啸叫
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert,
        }
    };
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

PwmBacklight::~PwmBacklight() {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
    // LEDC resolution set to 10bits, thus: 100% = 1023
    uint32_t duty_cycle = (1023 * brightness) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

