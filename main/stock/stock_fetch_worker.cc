// stock_fetch_worker.cc — 见头文件说明。

#include "stock_fetch_worker.h"

#include "beijing_clock.h"
#include "stock_api.h"

#include "board.h"
#include "dual_network_board.h"
#include "wifi_station.h"

#include "esp_lv_adapter.h"
#include "esp_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

namespace stock_fetch {
namespace {

constexpr char TAG[] = "stock_fetch";
constexpr UBaseType_t kCmdQueueSize = 8;

// 命令走值传递（xQueueSend memcpy），不带 std::string/指针。
struct FetchCmd {
    FetchKind kind;
    ChartMode mode;
    uint32_t session;
    char symbol[24];
    char end_date[12];
    int count;
    uint8_t batch_n;
    char batch_syms[kBatchMax][24];
};

QueueHandle_t s_cmd_q = nullptr;
TaskHandle_t s_task = nullptr;
Callbacks s_cb;

std::atomic<uint32_t> s_session{0};
std::atomic<bool> s_inflight[FETCH_KIND_COUNT] = {};

// worker 私有成品缓冲。单 in-flight（worker 串行处理命令）→ 复用安全。
StockQuote s_quotes[kBatchMax];
ChartSeries s_chart;
MarketStatus s_market;

void ClearInFlight(FetchKind k) { s_inflight[k].store(false, std::memory_order_release); }

// 锁 LVGL + 校验 session；返回是否应触发回调（仍需调用方 Unlock）。
bool LockAndValid(uint32_t cmd_session) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return false;
    if (cmd_session != s_session.load(std::memory_order_acquire)) {
        esp_lv_adapter_unlock();
        return false;
    }
    return true;
}

void ProcessQuoteBatch(const FetchCmd& cmd) {
    std::string syms[kBatchMax];
    for (uint8_t i = 0; i < cmd.batch_n && i < kBatchMax; i++) syms[i] = cmd.batch_syms[i];
    std::string err;
    bool ok = stock_api::FetchQuoteBatch(syms, cmd.batch_n, s_quotes, err);
    if (LockAndValid(cmd.session)) {
        if (s_cb.on_quote_batch) s_cb.on_quote_batch(s_quotes, cmd.batch_n, ok, err.c_str());
        esp_lv_adapter_unlock();
    }
    ClearInFlight(FETCH_QUOTE_BATCH);
}

void ProcessChart(const FetchCmd& cmd, bool is_range) {
    std::string err;
    bool ok = is_range
                  ? stock_api::FetchKlineRange(cmd.symbol, cmd.mode, cmd.end_date,
                                               cmd.count, s_chart, err)
                  : stock_api::FetchChart(cmd.symbol, cmd.mode, s_chart, err);
    if (LockAndValid(cmd.session)) {
        if (s_cb.on_chart) s_cb.on_chart(&s_chart, ok, err.c_str(), is_range);
        esp_lv_adapter_unlock();
    }
    ClearInFlight(is_range ? FETCH_CHART_RANGE : FETCH_CHART);
}

void ProcessMarketStat(const FetchCmd& cmd) {
    std::string err;
    bool ok = stock_api::FetchMarketStat(s_market, err);
    if (ok && s_market.valid) {
        // 顺带校时（全局，不需 LVGL 锁）。
        beijing_clock::SetReference(s_market.server_bj_year, s_market.server_bj_mon,
                                    s_market.server_bj_day, s_market.server_bj_hour,
                                    s_market.server_bj_min, s_market.server_bj_sec);
    }
    if (LockAndValid(cmd.session)) {
        if (s_cb.on_market_stat) s_cb.on_market_stat(&s_market, ok, err.c_str());
        esp_lv_adapter_unlock();
    }
    ClearInFlight(FETCH_MARKET_STAT);
}

void WorkerRun(void*) {
    FetchCmd cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.kind) {
            case FETCH_QUOTE_BATCH: ProcessQuoteBatch(cmd); break;
            case FETCH_CHART:       ProcessChart(cmd, false); break;
            case FETCH_CHART_RANGE: ProcessChart(cmd, true); break;
            case FETCH_MARKET_STAT: ProcessMarketStat(cmd); break;
            default: break;
        }
    }
}

// 通用入队：设 in-flight → send（0 超时）→ 失败回滚。
bool Enqueue(FetchCmd& cmd, FetchKind k) {
    if (s_cmd_q == nullptr) return false;
    bool expected = false;
    if (!s_inflight[k].compare_exchange_strong(expected, true)) return false;  // 已在途
    cmd.session = s_session.load(std::memory_order_acquire);
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        s_inflight[k].store(false, std::memory_order_release);
        return false;
    }
    return true;
}

}  // namespace

void Begin(const Callbacks& cb) {
    s_cb = cb;
    BumpSession();
    if (s_cmd_q != nullptr) return;  // 幂等：worker 常驻
    s_cmd_q = xQueueCreate(kCmdQueueSize, sizeof(FetchCmd));
    if (s_cmd_q == nullptr) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    if (xTaskCreatePinnedToCore(WorkerRun, "stock_fetch", 12288, nullptr, 4, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

uint32_t BumpSession() { return s_session.fetch_add(1, std::memory_order_acq_rel) + 1; }
uint32_t CurrentSession() { return s_session.load(std::memory_order_acquire); }

bool EnqueueQuoteBatch(const std::string* syms, size_t n) {
    if (syms == nullptr || n == 0 || n > kBatchMax) return false;
    FetchCmd cmd = {};
    cmd.kind = FETCH_QUOTE_BATCH;
    cmd.batch_n = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; i++) {
        if (syms[i].empty() || syms[i].size() >= sizeof(cmd.batch_syms[i])) return false;
        std::strncpy(cmd.batch_syms[i], syms[i].c_str(), sizeof(cmd.batch_syms[i]) - 1);
    }
    return Enqueue(cmd, FETCH_QUOTE_BATCH);
}

bool EnqueueChart(const char* symbol, ChartMode mode) {
    if (symbol == nullptr) return false;
    size_t n = std::strlen(symbol);
    if (n == 0 || n >= sizeof(FetchCmd::symbol)) return false;
    FetchCmd cmd = {};
    cmd.kind = FETCH_CHART;
    cmd.mode = mode;
    std::strncpy(cmd.symbol, symbol, sizeof(cmd.symbol) - 1);
    return Enqueue(cmd, FETCH_CHART);
}

bool EnqueueChartRange(const char* symbol, ChartMode mode, const char* end_date, int count) {
    if (symbol == nullptr || count <= 0) return false;
    size_t n = std::strlen(symbol);
    if (n == 0 || n >= sizeof(FetchCmd::symbol)) return false;
    FetchCmd cmd = {};
    cmd.kind = FETCH_CHART_RANGE;
    cmd.mode = mode;
    cmd.count = count;
    std::strncpy(cmd.symbol, symbol, sizeof(cmd.symbol) - 1);
    if (end_date != nullptr) {
        if (std::strlen(end_date) >= sizeof(cmd.end_date)) return false;
        std::strncpy(cmd.end_date, end_date, sizeof(cmd.end_date) - 1);
    }
    return Enqueue(cmd, FETCH_CHART_RANGE);
}

bool EnqueueMarketStat() {
    FetchCmd cmd = {};
    cmd.kind = FETCH_MARKET_STAT;
    return Enqueue(cmd, FETCH_MARKET_STAT);
}

bool InFlight(FetchKind kind) {
    if (kind >= FETCH_KIND_COUNT) return false;
    return s_inflight[kind].load(std::memory_order_acquire);
}

bool NetworkReady() {
    if (Board::GetInstance().GetNetwork() == nullptr) return false;
    auto* dn = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dn != nullptr && dn->GetNetworkType() == NetworkType::WIFI) {
        return WifiStation::GetInstance().IsConnected();
    }
    return true;  // 4G / 未知：尽力尝试
}

}  // namespace stock_fetch
