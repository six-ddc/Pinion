// sim shim — IOExpander click registry; clicks come from simTriggerClick().
#include "IOExpander.hpp"

IOExpander& IOExpander::getInstance() {
    static IOExpander instance;
    return instance;
}

esp_err_t IOExpander::onClick(Pin pin, ClickCallback callback, bool pressed_level,
                              uint32_t max_duration_ms) {
    (void)pressed_level;
    (void)max_duration_ms;
    if (!callback) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(mu_);
    handlers_[pin] = std::move(callback);
    return ESP_OK;
}

esp_err_t IOExpander::offClick(Pin pin) {
    std::lock_guard<std::mutex> lk(mu_);
    handlers_.erase(pin);
    return ESP_OK;
}

void IOExpander::simTriggerClick(Pin pin) {
    ClickCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = handlers_.find(pin);
        if (it == handlers_.end()) return;
        cb = it->second;
    }
    cb();
}

esp_err_t IOExpander::onLongPress(Pin pin, uint32_t duration_ms, LongPressCallback callback,
                                  bool pressed_level) {
    (void)duration_ms;
    (void)pressed_level;
    if (!callback)
        return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(mu_);
    lp_handlers_[pin] = std::move(callback);
    return ESP_OK;
}

esp_err_t IOExpander::offLongPress(Pin pin) {
    std::lock_guard<std::mutex> lk(mu_);
    lp_handlers_.erase(pin);
    return ESP_OK;
}

void IOExpander::simTriggerLongPress(Pin pin) {
    LongPressCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = lp_handlers_.find(pin);
        if (it == lp_handlers_.end())
            return;
        cb = it->second;
    }
    cb();
}
