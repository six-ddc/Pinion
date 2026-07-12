/* sim shim — public surface of the TCA9555 IOExpander wrapper that pi_screen
 * uses (getInstance / Pin::PWR_KEY / onClick / offClick). Clicks are injected
 * from the SDL side via simTriggerClick() instead of a physical button. */
#ifndef IO_EXPANDER_HPP
#define IO_EXPANDER_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

#include "esp_err.h"

class IOExpander {
public:
    /* Same logical pin list as the real header (values are indices only). */
    enum class Pin : uint8_t {
        GPS_POWER = 0,
        PA_SWITCH,
        CAM_PWDN,
        SD,
        PWR_KEY_PULSE,
        PWR_KEY,
        BT_POWER,
        RST_4G,
        PA,
        ACCEL_INT,
        USB_INSERT_DET,
        WIRELESS_CHARGE_DET,
        kPinCount,
    };

    using ClickCallback = std::function<void()>;

    static IOExpander& getInstance();

    esp_err_t onClick(Pin pin, ClickCallback callback, bool pressed_level = false,
                      uint32_t max_duration_ms = 500);
    esp_err_t offClick(Pin pin);

    /* sim-only: fire the registered click callback for `pin` (call with the
     * LVGL lock held — the pi_screen callback does lv_async_call). */
    void simTriggerClick(Pin pin);

private:
    std::mutex mu_;
    std::map<Pin, ClickCallback> handlers_;
};

#endif
