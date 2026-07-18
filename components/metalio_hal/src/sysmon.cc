// 系统监控任务，原样自 metalio-claw-4.cc 板级构造函数内的匿名 lambda。
#include "metalio_hal/sysmon.h"

#include <mutex>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bq27220_gauge.h"

namespace mhal::sysmon {
namespace {

uint32_t s_period_ms = 1000;
bool s_started = false;

// CPU 占用率 / 内部 RAM 水位快照：MonitorTask 每周期算完发布，getter 非阻塞读。
// 数据面(DataHub)与调试侧共用，短锁保护（读多写少，1Hz 写）。
struct SysSnap {
    bool     valid       = false;
    int      core0       = 0;
    int      core1       = 0;
    int      avg         = 0;
    unsigned free_kb     = 0;
    unsigned min_free_kb = 0;
};
std::mutex s_snap_mu;
SysSnap    s_snap;

// —— 按任务分解 CPU + 栈水位（每 kTaskDumpEveryN 周期一次）——
// 回答"每核 99% 究竟是谁吃的"：uxTaskGetSystemState 两次采样各任务 runtime counter
//（us 计，CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER）做差 ÷ 采样间隔 = 该任务
// 的单核占比。同时打 usStackHighWaterMark（IDF 口径为字节，历史最小剩余）——LVGL
// 64KB 任务栈 / 48KB 绘制栈能不能砍、砍多少，就以它为准。快照缓冲放 PSRAM。
constexpr int kTaskDumpEveryN = 10;  // 默认周期 1s → 每 10s 一份任务榜
constexpr size_t kMaxTasks = 48;
constexpr int kTaskDumpTopN = 10;

struct TaskPrev {
    UBaseType_t num = 0;  // xTaskNumber：唯一递增，规避 TaskHandle 复用导致的基线错配
    configRUN_TIME_COUNTER_TYPE runtime = 0;
};

void DumpTaskStats(const char* tag, TaskStatus_t* snap, TaskPrev* prev, size_t* prev_n, uint64_t dt_us) {
    const UBaseType_t n = uxTaskGetSystemState(snap, kMaxTasks, nullptr);
    if (n == 0 || dt_us == 0) return;

    if (*prev_n == 0) {  // 首次只记基线不打（否则全体 0%）
        size_t pn0 = 0;
        for (UBaseType_t i = 0; i < n && pn0 < kMaxTasks; ++i) {
            prev[pn0++] = TaskPrev{snap[i].xTaskNumber, snap[i].ulRunTimeCounter};
        }
        *prev_n = pn0;
        return;
    }

    uint64_t deltas[kMaxTasks];
    for (UBaseType_t i = 0; i < n; ++i) {
        configRUN_TIME_COUNTER_TYPE last = 0;
        bool seen = false;
        for (size_t j = 0; j < *prev_n; ++j) {
            if (prev[j].num == snap[i].xTaskNumber) {
                last = prev[j].runtime;
                seen = true;
                break;
            }
        }
        // 计数器按其自身位宽自然回绕做差；首个周期无基线不计。
        deltas[i] = seen ? (uint64_t)(configRUN_TIME_COUNTER_TYPE)(snap[i].ulRunTimeCounter - last) : 0;
    }
    size_t pn = 0;
    for (UBaseType_t i = 0; i < n && pn < kMaxTasks; ++i) {
        prev[pn++] = TaskPrev{snap[i].xTaskNumber, snap[i].ulRunTimeCounter};
    }
    *prev_n = pn;

    // n ≤ 48：选择排序取 Top N 即可。
    bool used[kMaxTasks] = {false};
    for (int r = 0; r < kTaskDumpTopN; ++r) {
        int best = -1;
        for (UBaseType_t i = 0; i < n; ++i) {
            if (!used[i] && (best < 0 || deltas[i] > deltas[best])) best = (int)i;
        }
        if (best < 0) break;
        used[best] = true;
        const int pct = (int)(deltas[best] * 100ULL / dt_us);  // 单核占比
        if (pct < 1 && r > 0) break;  // 榜首之外 <1% 的不打，压日志量
        const TaskStatus_t& ts = snap[best];
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        const char core_ch = (ts.xCoreID == 0) ? '0' : (ts.xCoreID == 1) ? '1' : '-';
#else
        const char core_ch = '?';
#endif
        ESP_LOGI(tag, "@@@任务  | %-16s C%c P%-2u cpu %3d%% | 栈余 %5u B", ts.pcTaskName, core_ch,
                 (unsigned)ts.uxCurrentPriority, pct, (unsigned)ts.usStackHighWaterMark);
    }
}

void MonitorTask(void*) {
    auto& gauge = Bq27220Gauge::GetInstance();

    // 任务榜采样缓冲（PSRAM，不占内部 .bss；拿不到就静默关掉该功能）。
    auto* task_snap = static_cast<TaskStatus_t*>(
        heap_caps_malloc(sizeof(TaskStatus_t) * kMaxTasks, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* task_prev = static_cast<TaskPrev*>(
        heap_caps_calloc(kMaxTasks, sizeof(TaskPrev), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t task_prev_n = 0;
    int dump_cycle = 0;
    uint64_t last_dump_us = (uint64_t)esp_timer_get_time();

    // ---- ESP32-P4 双核 CPU 占用率采样 ----
    // 依赖 sdkconfig：CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y、
    // CONFIG_FREERTOS_USE_TRACE_FACILITY=y、
    // CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y。
    // 每核的 Idle Task 只在无其他 task 时被调度；两次采样
    // ulTaskGetIdleRunTimeCounterForCore()（单位 us）做差即得空闲时长：
    //   usage% = 100 - idle_delta_us * 100 / total_delta_us
    constexpr int kCoreCount = portNUM_PROCESSORS;
    configRUN_TIME_COUNTER_TYPE prev_idle[kCoreCount] = {0};
    for (int c = 0; c < kCoreCount; ++c) {
        prev_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    }
    uint64_t prev_us = (uint64_t)esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(s_period_ms));

        uint64_t now_us = (uint64_t)esp_timer_get_time();
        uint64_t dt_us = now_us - prev_us;
        int usage[kCoreCount] = {0};
        int total_usage = 0;
        if (dt_us > 0) {
            for (int c = 0; c < kCoreCount; ++c) {
                configRUN_TIME_COUNTER_TYPE now_idle = ulTaskGetIdleRunTimeCounterForCore(c);
                configRUN_TIME_COUNTER_TYPE didle = now_idle - prev_idle[c];
                uint64_t idle_pct = (uint64_t)didle * 100ULL / dt_us;
                if (idle_pct > 100) idle_pct = 100;
                usage[c] = 100 - (int)idle_pct;
                total_usage += usage[c];
                prev_idle[c] = now_idle;
            }
        }
        prev_us = now_us;
        const int avg_usage = (kCoreCount > 0) ? (total_usage / kCoreCount) : 0;
        const int core1_usage = (kCoreCount > 1) ? usage[1] : 0;

        constexpr const char* kMonitorTag = "系统监控";
        ESP_LOGI(kMonitorTag, "@@@CPU   | 内核0: %3d%% | 内核1: %3d%% | 平均: %3d%%",
                 usage[0], core1_usage, avg_usage);

        const unsigned free_kb =
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        const unsigned min_free_kb =
            static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
        // 内部 SRAM 是稀缺资源（P4 L2MEM，栈/DMA/WiFi/小分配只能用它）；PSRAM 32MB
        // 是大头但只承接 >=4KB 的普通分配。最大连续块（largest）比总剩余更能预警
        // "碎片化到建不出线程栈"（pthread 栈要一整块）。
        const unsigned largest_kb = static_cast<unsigned>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
        const unsigned psram_free_kb =
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
        ESP_LOGI(kMonitorTag, "@@@内存  | 剩余: %6u KB | 历史最小: %6u KB | 最大块: %4u KB | PSRAM余: %6u KB",
                 free_kb, min_free_kb, largest_kb, psram_free_kb);

#if CONFIG_HEAP_POISONING_COMPREHENSIVE || CONFIG_HEAP_POISONING_LIGHT
        // 堆破坏排查（随 poisoning 调试配置一起生效）：每秒巡检**内部堆**完整性
        //（被踩块在内部堆；不可用 check_integrity_all——遍历 32MB PSRAM 堆会挤占
        // PSRAM 带宽把 DSI 扫描饿到 underrun，屏幕节奏性闪蓝。comprehensive 毒化
        // 的逐次 memset 同样会拖慢渲染路径致持续闪屏，故只配 LIGHT 金丝雀）。
        // print_errors=true 立刻打出被踩块地址，定位窗口收敛到 1s 内的日志上下文。
        if (!heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)) {
            ESP_LOGE(kMonitorTag, "@@@堆完整性巡检失败！以上为被踩块详情");
        }
#endif

        // 发布 CPU/heap 快照供数据面非阻塞读。
        {
            std::lock_guard<std::mutex> lk(s_snap_mu);
            s_snap.core0       = usage[0];
            s_snap.core1       = core1_usage;
            s_snap.avg         = avg_usage;
            s_snap.free_kb     = free_kb;
            s_snap.min_free_kb = min_free_kb;
            s_snap.valid       = true;
        }

        int battery_level;
        bool charging, discharging;
        if (gauge.GetBatteryLevel(battery_level, charging, discharging)) {
            uint16_t mv = 0;
            const bool mv_ok = gauge.GetVoltageMv(mv);
            if (mv_ok) {
                ESP_LOGI(kMonitorTag,
                         "@@@电池  | 电量: %3d%% | 电压: %5u mV | 充电: %s | 放电: %s",
                         battery_level, mv, charging ? "是" : "否", discharging ? "是" : "否");
            } else {
                ESP_LOGI(kMonitorTag,
                         "@@@电池  | 电量: %3d%% | 电压: 读取失败 | 充电: %s | 放电: %s",
                         battery_level, charging ? "是" : "否", discharging ? "是" : "否");
            }
        }

        // 扩展电池遥测：温度/剩余续航/健康度/满充容量/循环次数，发布快照供数据面读。
        // 温度按 0.1℃ 整数打印（规避 newlib-nano 浮点格式坑）。
        if (gauge.SampleExtended()) {
            Bq27220Gauge::ExtTelemetry et;
            gauge.GetExtTelemetry(et);
            ESP_LOGI(kMonitorTag,
                     "@@@电池+ | 温度: %d(0.1℃) | 剩余续航: %d min | 健康: %d%% | "
                     "满充: %d mAh | 循环: %d | 剩余: %d mAh",
                     et.temp_c10, et.tte_min, et.soh_pct, et.fcc_mah, et.cycles, et.remcap_mah);
        }

        // 任务榜：每 kTaskDumpEveryN 周期打 Top N 任务的单核 CPU 占比 + 栈历史最小剩余。
        if (task_snap != nullptr && task_prev != nullptr && ++dump_cycle >= kTaskDumpEveryN) {
            dump_cycle = 0;
            const uint64_t dump_now = (uint64_t)esp_timer_get_time();
            DumpTaskStats(kMonitorTag, task_snap, task_prev, &task_prev_n, dump_now - last_dump_us);
            last_dump_us = dump_now;
        }
    }
}

}  // namespace

void Start(uint32_t period_ms) {
    if (s_started) {
        return;
    }
    s_started = true;
    s_period_ms = period_ms;
    xTaskCreate(MonitorTask, "_task", 8192, nullptr, 5, nullptr);
}

bool GetCpuUsage(int& core0, int& core1, int& avg) {
    std::lock_guard<std::mutex> lk(s_snap_mu);
    if (!s_snap.valid) {
        return false;
    }
    core0 = s_snap.core0;
    core1 = s_snap.core1;
    avg   = s_snap.avg;
    return true;
}

bool GetHeapKb(unsigned& free_kb, unsigned& min_free_kb) {
    std::lock_guard<std::mutex> lk(s_snap_mu);
    if (!s_snap.valid) {
        return false;
    }
    free_kb     = s_snap.free_kb;
    min_free_kb = s_snap.min_free_kb;
    return true;
}

}  // namespace mhal::sysmon
