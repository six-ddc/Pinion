// stock_fetch_worker.h
// 行情抓取 worker（Claw6 版）：请求队列 + 结果队列，worker 线程绝不碰 LVGL。
//
// 与 Claw4 的差异（那版在持 esp_lv_adapter_lock 下回调 UI）：Claw6 的契约是
// "worker 不碰 LVGL，一切经队列由 LVGL 线程的 lv_timer 排空"（与 pi_screen 的
// DrainQueueTick 同构）。消费方（pi_card_stock 的模块 timer）非阻塞 Poll()，
// 按 session 匹配控件；session 不匹配（控件已删/已切模式）的结果直接释放。

#ifndef STOCK_FETCH_WORKER_H
#define STOCK_FETCH_WORKER_H

#include "stock_models.h"

#include <cstdint>

namespace stock_fetch_worker {

enum class Kind : uint8_t { Quote, Chart };

struct Request {
    uint32_t session = 0;  // 控件代次；结果带回，消费方据此匹配/丢弃
    Kind kind = Kind::Quote;
    char symbol[24] = {};
    ChartMode mode = CHART_MIN_1D;  // kind==Chart 时有效
};

struct Result {
    uint32_t session = 0;
    Kind kind = Kind::Quote;
    char symbol[24] = {};
    ChartMode mode = CHART_MIN_1D;
    bool ok = false;
    char err[48] = {};
    StockQuote* quote = nullptr;    // ok && kind==Quote：堆上，消费方 delete
    ChartSeries* series = nullptr;  // ok && kind==Chart：堆上，消费方 delete
};

// 惰性起 worker task + 两个队列（首次 Submit 前自动调用；幂等）。
void EnsureStarted();

// 入请求队列（满则丢弃返回 false，调用方保持"未 in-flight"状态下轮重试）。
bool Submit(const Request& r);

// 非阻塞取一条结果；无结果返回 false。LVGL 线程 timer 调用。
// 取到的 Result 里 quote/series 所有权归调用方（不匹配也要 FreePayload）。
bool Poll(Result& out);

// 释放一条结果的堆载荷（session 不匹配丢弃时用）。
void FreePayload(Result& r);

}  // namespace stock_fetch_worker

#endif  // STOCK_FETCH_WORKER_H
