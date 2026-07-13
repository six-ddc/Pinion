// sim shim — IOExpander input-event registry. The physical PWR_KEY is a held
// line fed by simSetPressed() (F1 down/up); simPoll() runs the click /
// long-press / press / release edge state machines exactly like the real
// monitor task, so press-and-hold behaves identically to hardware.
#include "IOExpander.hpp"

IOExpander& IOExpander::getInstance() {
    static IOExpander instance;
    return instance;
}

esp_err_t IOExpander::onClick(Pin pin, ClickCallback callback, bool pressed_level,
                              uint32_t max_duration_ms) {
    (void)pressed_level;
    if (!callback) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(mu_);
    click_handlers_[pin] = std::move(callback);
    click_max_ms_[pin] = max_duration_ms;
    return ESP_OK;
}

esp_err_t IOExpander::offClick(Pin pin) {
    std::lock_guard<std::mutex> lk(mu_);
    click_handlers_.erase(pin);
    click_max_ms_.erase(pin);
    return ESP_OK;
}

esp_err_t IOExpander::onLongPress(Pin pin, uint32_t duration_ms, LongPressCallback callback,
                                  bool pressed_level) {
    (void)pressed_level;
    if (!callback) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(mu_);
    lp_handlers_[pin] = std::move(callback);
    lp_duration_ms_[pin] = duration_ms;
    return ESP_OK;
}

esp_err_t IOExpander::offLongPress(Pin pin) {
    std::lock_guard<std::mutex> lk(mu_);
    lp_handlers_.erase(pin);
    lp_duration_ms_.erase(pin);
    return ESP_OK;
}

esp_err_t IOExpander::onPress(Pin pin, EdgeCallback callback, bool pressed_level) {
    (void)pressed_level;
    if (!callback) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(mu_);
    press_handlers_[pin] = std::move(callback);
    return ESP_OK;
}

esp_err_t IOExpander::onRelease(Pin pin, EdgeCallback callback, bool pressed_level) {
    (void)pressed_level;
    if (!callback) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(mu_);
    release_handlers_[pin] = std::move(callback);
    return ESP_OK;
}

esp_err_t IOExpander::offPress(Pin pin) {
    std::lock_guard<std::mutex> lk(mu_);
    press_handlers_.erase(pin);
    return ESP_OK;
}

esp_err_t IOExpander::offRelease(Pin pin) {
    std::lock_guard<std::mutex> lk(mu_);
    release_handlers_.erase(pin);
    return ESP_OK;
}

void IOExpander::simSetPressed(Pin pin, bool pressed) {
    std::lock_guard<std::mutex> lk(mu_);
    state_[pin].pressed = pressed;
}

void IOExpander::simTriggerClick(Pin pin) {
    ClickCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = click_handlers_.find(pin);
        if (it != click_handlers_.end()) cb = it->second;
    }
    if (cb) cb();
}

void IOExpander::simTriggerLongPress(Pin pin) {
    LongPressCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = lp_handlers_.find(pin);
        if (it != lp_handlers_.end()) cb = it->second;
    }
    if (cb) cb();
}

void IOExpander::simPoll(uint32_t now_ms) {
    // Collect callbacks under the lock, fire after releasing it (matches the
    // real monitor task; also avoids surprises if a callback ever re-enters).
    std::vector<EdgeCallback> pending;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : state_) {
            const Pin pin = kv.first;
            PinState& s = kv.second;
            const bool is_pressed = s.pressed;

            if (is_pressed && !s.was_pressed) {
                // Press edge.
                s.press_start_ms = now_ms;
                s.lp_fired = false;
                auto it = press_handlers_.find(pin);
                if (it != press_handlers_.end()) pending.push_back(it->second);
            } else if (is_pressed && !s.lp_fired) {
                // Held: fire long-press once threshold is crossed.
                auto lp = lp_handlers_.find(pin);
                auto dur = lp_duration_ms_.find(pin);
                if (lp != lp_handlers_.end() && dur != lp_duration_ms_.end() &&
                    now_ms - s.press_start_ms >= dur->second) {
                    pending.push_back(lp->second);
                    s.lp_fired = true;
                }
            } else if (!is_pressed && s.was_pressed) {
                // Release edge: emit a click if the hold was short enough, then
                // the raw release.
                const uint32_t held = now_ms - s.press_start_ms;
                auto ck = click_handlers_.find(pin);
                auto cmax = click_max_ms_.find(pin);
                if (ck != click_handlers_.end() && cmax != click_max_ms_.end() &&
                    held <= cmax->second) {
                    pending.push_back(ck->second);
                }
                auto rel = release_handlers_.find(pin);
                if (rel != release_handlers_.end()) pending.push_back(rel->second);
            }
            s.was_pressed = is_pressed;
        }
    }
    for (auto& cb : pending) cb();
}
