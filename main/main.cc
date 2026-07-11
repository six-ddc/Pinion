#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "system_info.h"

#define TAG "main"

namespace {

// Board::StartNetwork() (Wi-Fi scan/connect, or captive-portal config mode
// when no SSID is provisioned yet) can block for tens of seconds. Running
// it on a background task lets Board::GetInstance() finish bringing up the
// display first, so the pi idle screen (clock/breathing dot) is already on
// screen while the network comes up in parallel (blueprint R6).
void StartNetworkTask(void*) {
    Board::GetInstance().StartNetwork();
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi credentials and pi_screen settings.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Board::GetInstance() runs the board/display constructor chain (I2C,
    // IOExpander, LCD, touch, LVAdapterDisplay -> SetupUI() -> pi idle
    // screen), same as the old Application::Start() path did, but without
    // pulling in Application/audio/protocol at all.
    Board::GetInstance();

    xTaskCreate(StartNetworkTask, "start_network", 4096, nullptr, 2, nullptr);
}
