#pragma once

#include <cstdint>

// 振动马达门面（GPIO22 LEDC PWM 5kHz，10-bit duty）。
// !! 固定用 LEDC_TIMER_1 / LEDC_CHANNEL_1 —— TIMER_0/CHANNEL_0 被
// backlight.cc 的 PwmBacklight 占用，共用会导致「拉振动改成调背光」
// 并把 channel 0 的 GPIO 输出重新映射。两者互不冲突，可独立调用。
namespace mhal::motor {

// 幂等：配置 LEDC(TIMER_1/CHANNEL_1/GPIO22)。Buzz() 会懒调用，一般
// 不需要在 boot 链里手动调。
void Init();

// 非阻塞：立即以 strength_pct(0..100) 力度起振，duration_ms 后
// 由 esp_timer 一次性回调自动停止。可在 LVGL 线程直接调用。
// 重复调用会先取消上一次的自动停止定时器再重新计时，不会叠加/抢占。
void Buzz(uint32_t duration_ms = 200, int strength_pct = 100);

// 立即停止振动（取消任何挂起的自动停止定时器）。
void Stop();

}  // namespace mhal::motor
