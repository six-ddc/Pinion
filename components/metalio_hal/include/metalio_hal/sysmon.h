#pragma once

#include <cstdint>

// 系统监控：独立任务周期输出双核 CPU 占用率（基于各核 Idle 任务运行时长，
// 依赖 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS/USE_TRACE_FACILITY/
// RUN_TIME_STATS_USING_ESP_TIMER）、内部 RAM 水位、电池状态到日志
// （TAG "系统监控"，串口验收时肉眼可读）。
namespace mhal::sysmon {

// 幂等；重复调用忽略。
void Start(uint32_t period_ms = 1000);

}  // namespace mhal::sysmon
