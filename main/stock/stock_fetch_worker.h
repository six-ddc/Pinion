// stock_fetch_worker.h
// 常驻后台 worker：把同步 HTTP 抓取隔离到独立 FreeRTOS 任务，LVGL 主线程只入队
// 命令、不阻塞。结果由 worker 在 esp_lv_adapter_lock 临界区内、校验 session 后
// 直接回调进 UI（weather/openclaw 先例）——无结果队列。
//
// 铁律：worker 里除回调那一小段（已持 LVGL 锁）外，绝不可调 lv_*。
// 回调在 worker 线程、持 LVGL 锁时被调用；仅当 cmd 的 session == 当前 session
// 才触发（screen 卸载/切股/切模式时 BumpSession 作废在途结果）。

#ifndef STOCK_FETCH_WORKER_H
#define STOCK_FETCH_WORKER_H

#include "stock_models.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace stock_fetch {

enum FetchKind : uint8_t {
    FETCH_QUOTE_BATCH = 0,
    FETCH_CHART = 1,        // minute/5d 整张刷新；K 线默认 22/26 根
    FETCH_CHART_RANGE = 2,  // K 线增量历史，按 endDate + count
    FETCH_MARKET_STAT = 3,
    FETCH_KIND_COUNT
};

constexpr uint8_t kBatchMax = 16;

// 回调在 worker 线程、持 LVGL 锁时触发。ok=false 时 err 为短英文原因。
// chart 回调的 is_range 区分 FETCH_CHART / FETCH_CHART_RANGE。
typedef void (*QuoteBatchCb)(const StockQuote* quotes, size_t count, bool ok,
                             const char* err);
typedef void (*ChartCb)(const ChartSeries* chart, bool ok, const char* err,
                        bool is_range);
typedef void (*MarketStatCb)(const MarketStatus* stat, bool ok, const char* err);

struct Callbacks {
    QuoteBatchCb on_quote_batch = nullptr;
    ChartCb on_chart = nullptr;
    MarketStatCb on_market_stat = nullptr;
};

// 创建 queue + task（幂等；worker 常驻，不随 screen 卸载销毁）。每次进 screen 调用
// 以（重新）登记回调并 BumpSession。
void Begin(const Callbacks& cb);

// 递增 session，作废所有在途结果（screen 卸载/切股/切模式时调）。返回新值。
uint32_t BumpSession();
uint32_t CurrentSession();

// 入队（LVGL 线程调用）。同类在途或队列满 → 返回 false，调用方跳过本次。
bool EnqueueQuoteBatch(const std::string* syms, size_t n);
bool EnqueueChart(const char* symbol, ChartMode mode);
bool EnqueueChartRange(const char* symbol, ChartMode mode, const char* end_date,
                       int count);
bool EnqueueMarketStat();

// 某类是否还有在途。
bool InFlight(FetchKind kind);

// 网络是否就绪（可发起 HTTP）。未就绪时不入队，避免 connect 长时间阻塞把 in-flight
// 卡死（表现为「一直加载、退出重进才好」）。WiFi 用 IsConnected 精确判断；4G/未知
// 退化为「网络对象存在即尝试」（4G 驱动自带超时）。
bool NetworkReady();

}  // namespace stock_fetch

#endif  // STOCK_FETCH_WORKER_H
