// beijing_clock.cc

#include "beijing_clock.h"

#include "esp_timer.h"

#include <cstdint>

namespace beijing_clock {
namespace {

bool s_valid = false;
time_t s_ref_bj_epoch = 0;   // 基准北京 epoch（BJ-as-UTC）
int64_t s_ref_mono_us = 0;   // 设基准时的 esp_timer 单调时间

// Howard Hinnant days_from_civil：把 y-m-d 转成自 1970-01-01 的天数（proleptic）。
int64_t DaysFromCivil(int y, int m, int d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace

void SetReference(int year, int mon, int day, int hour, int min, int sec) {
    if (year < 2000 || mon < 1 || mon > 12 || day < 1 || day > 31) return;
    int64_t days = DaysFromCivil(year, mon, day);
    s_ref_bj_epoch = static_cast<time_t>(days * 86400 + hour * 3600 + min * 60 + sec);
    s_ref_mono_us = esp_timer_get_time();
    s_valid = true;
}

time_t NowEpochS() {
    if (!s_valid) return 0;
    int64_t elapsed_s = (esp_timer_get_time() - s_ref_mono_us) / 1000000;
    return s_ref_bj_epoch + static_cast<time_t>(elapsed_s);
}

bool Valid() { return s_valid; }

}  // namespace beijing_clock
