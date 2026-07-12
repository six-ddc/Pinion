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

#include "pi_screen/pi_screen.h"
#include "screen_util.h"

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

    // 产品决策：开机音量/亮度一律拉满，不吃 NVS 里的历史小值
    // （TTS 音量是软件缩放，历史值 10 会小到听不见）。
    mhal::audio::SetVolume(80);
    mhal::backlight::SetBrightness(100);

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
