#include "metalio_hal/backlight.h"

#include "backlight.h"
#include "config.h"

namespace mhal::backlight {

namespace {
PwmBacklight& Impl() {
    static PwmBacklight bl(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    return bl;
}
}  // namespace

void SetBrightness(uint8_t percent, bool persist) { Impl().SetBrightness(percent, persist); }

uint8_t GetBrightness() { return Impl().brightness(); }

void Restore() { Impl().RestoreBrightness(); }

}  // namespace mhal::backlight
