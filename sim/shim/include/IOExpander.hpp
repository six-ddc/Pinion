/* sim shim — public surface of the TCA9555 IOExpander wrapper that pi_screen
 * uses (getInstance / Pin::PWR_KEY / onClick / offClick / onLongPress /
 * offLongPress / onPress / onRelease / offPress / offRelease).
 *
 * The physical PWR_KEY is modelled as a held line: the SDL side mirrors the
 * F1 key state via simSetPressed(), and simPoll() — called every main-loop
 * iteration with the LVGL lock held — runs the same click / long-press /
 * press / release edge state machines the real monitor task runs, so
 * press-and-hold (start on press, commit on release, quick tap = nothing)
 * behaves identically to hardware. */
#ifndef IO_EXPANDER_HPP
#define IO_EXPANDER_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

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
    using LongPressCallback = std::function<void()>;
    using EdgeCallback = std::function<void()>;

    static IOExpander& getInstance();

    esp_err_t onClick(Pin pin, ClickCallback callback, bool pressed_level = false,
                      uint32_t max_duration_ms = 500);
    esp_err_t offClick(Pin pin);

    esp_err_t onLongPress(Pin pin, uint32_t duration_ms, LongPressCallback callback,
                          bool pressed_level = false);
    esp_err_t offLongPress(Pin pin);

    esp_err_t onPress(Pin pin, EdgeCallback callback, bool pressed_level = false);
    esp_err_t onRelease(Pin pin, EdgeCallback callback, bool pressed_level = false);
    esp_err_t offPress(Pin pin);
    esp_err_t offRelease(Pin pin);

    /* sim-only: feed the physical held state (F1 down/up) and run the edge
     * state machines. Call simPoll() every loop iteration with the LVGL lock
     * held — the pi_screen callbacks do lv_async_call. */
    void simSetPressed(Pin pin, bool pressed);
    void simPoll(uint32_t now_ms);

    /* sim-only: directly fire a pin's click / long-press handler once
     * (main.cc maps F1 = click, F2 = long-press to these). */
    void simTriggerClick(Pin pin);
    void simTriggerLongPress(Pin pin);

private:
    struct PinState {
        bool     pressed = false;      // fed by simSetPressed()
        bool     was_pressed = false;  // last polled level (edge tracking)
        uint32_t press_start_ms = 0;
        bool     lp_fired = false;
    };

    std::mutex mu_;
    std::map<Pin, ClickCallback>     click_handlers_;
    std::map<Pin, uint32_t>          click_max_ms_;
    std::map<Pin, LongPressCallback> lp_handlers_;
    std::map<Pin, uint32_t>          lp_duration_ms_;
    std::map<Pin, EdgeCallback>      press_handlers_;
    std::map<Pin, EdgeCallback>      release_handlers_;
    std::map<Pin, PinState>          state_;
};

#endif
