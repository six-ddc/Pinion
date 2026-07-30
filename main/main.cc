#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <cstdlib>
#include <ctime>

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/display.h"
#include "metalio_hal/hal.h"
#include "metalio_hal/network.h"
#include "metalio_hal/sysmon.h"

#include "device_config.h"
#include "pi_screen/pi_screen.h"
#include "screen_util.h"
#include "settings.h"
#include "volc_speech_keys.h"

#define TAG "main"

extern "C" void app_main(void) {
    // 东八区：待机时钟走 localtime_r；对时本身在网络连通后起 SNTP。
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(mhal::Init());

    // 语音密钥来自 NVS（Web 后台的配置页写入），固件不打包。必须早于任何
    // ASR/TTS 会话——这里就是最早的时机（NVS 已 init，UI 还没建）。未配置时
    // 注入空值，volc_asr_start / volc_tts_speak_begin 会直接失败并 LOGE。
    volc_speech_set_keys(device_config::GetVoiceAppKey().c_str(),
                         device_config::GetVoiceAccessKey().c_str());

    // P0：开机吃 NVS——音量取 "audio"/"output_volume"（无历史值默认 70），
    // 亮度走 backlight::Restore()（内部有 NVS 默认 75%、下限 5% 逻辑，
    // mhal::Init() 已恢复过一次，这里显式再调一次保持语义自明）。不再拉满。
    {
        Settings audio_settings("audio", false);
        int32_t vol = audio_settings.GetInt("output_volume", 70);
        // 与 AudioCodec::Start()（audio_codec.cc:31）的静音兜底对齐：<=0 抬到
        // 10。否则这里用 NVS 原始 0 覆盖 Start() 已钳制的结果，会让"静音"跨
        // 重启永久保留（官方 Claw4 固件重启会回 10），是迁移引入的行为回归。
        if (vol <= 0) vol = 10;
        mhal::audio::SetVolume(static_cast<int>(vol));
    }
    mhal::backlight::Restore();

    // 唯一 UI：直进 pi 对话屏。lifecycle 必须在 lv_screen_load 之前挂上，
    // LOAD 回调里才会跑 pi_agent_task_start() 与 PWR_KEY 注册。
    if (mhal::display::Lock()) {
        lv_obj_t* old_scr = lv_screen_active();
        lv_obj_t* pi = PiScreen::Create();
        screen_attach_lifecycle(
            pi, [](screen_lifecycle_event_t e) { PiScreen::LifecycleCallback(e); });
        lv_screen_load(pi);
        if (old_scr != nullptr && old_scr != pi) {
            lv_obj_delete(old_scr);
        }
        mhal::display::Unlock();
    }

    // 起网放后台：Wi-Fi/4G 就绪可能要几十秒，不阻塞首帧。pi 的 agent env
    // 在首条 prompt 时才惰性建立，届时网络通常已就绪。
    mhal::network::StartAsync();

    mhal::sysmon::Start();
}
