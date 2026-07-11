// beijing_clock.h
// 北京时间源：以腾讯 marketStat 响应首段的服务器北京时间为基准，配 esp_timer
// 单调时钟推算当前北京时间。不碰系统时钟、不依赖 NTP/OTA（本项目无 SNTP）。
//
// epoch 约定与 market_schedule.h 一致：返回"把北京 wall-clock 当 UTC 解释得到的
// epoch 秒"，可直接传给 MarketSchedule::nextPollDelayMs。

#ifndef BEIJING_CLOCK_H
#define BEIJING_CLOCK_H

#include <ctime>

namespace beijing_clock {

// 用服务器北京时间设基准（来自 MarketStatus.server_bj_*）。
void SetReference(int year, int mon, int day, int hour, int min, int sec);

// 当前北京 epoch（BJ-as-UTC 秒）；未校准返回 0。
time_t NowEpochS();

// 是否已校准。
bool Valid();

}  // namespace beijing_clock

#endif  // BEIJING_CLOCK_H
