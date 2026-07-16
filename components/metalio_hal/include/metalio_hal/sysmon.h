#pragma once

#include <cstdint>

// 系统监控：独立任务周期输出双核 CPU 占用率（基于各核 Idle 任务运行时长，
// 依赖 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS/USE_TRACE_FACILITY/
// RUN_TIME_STATS_USING_ESP_TIMER）、内部 RAM 水位、电池状态到日志
// （TAG "系统监控"，串口验收时肉眼可读）。
namespace mhal::sysmon {

// 幂等；重复调用忽略。
void Start(uint32_t period_ms = 1000);

// 非阻塞读上一次采样发布的 CPU 占用率快照（%）。Start() 后首个采样周期完成前、
// 或未 Start 时返回 false（输出参数不改写）。
bool GetCpuUsage(int& core0, int& core1, int& avg);

// 非阻塞读上一次采样发布的内部 RAM 水位快照（KB）。同上，无数据返回 false。
bool GetHeapKb(unsigned& free_kb, unsigned& min_free_kb);

}  // namespace mhal::sysmon
