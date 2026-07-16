// stock_fetch_scheduler.h
// 详情屏的 fetch 调度决策器 —— 决定"现在该不该入队拉取"。header-only、无外部
// 依赖、可脱离硬件单测。
//
// marketStat / quote / chart 三条 fetch 共用同一套"是否要 fetch + 用什么延迟"的
// 纯函数决策，与 LVGL / enqueue 解耦，便于回归。

#ifndef SCENES_STOCK_STOCK_FETCH_SCHEDULER_H
#define SCENES_STOCK_STOCK_FETCH_SCHEDULER_H

#include "market_schedule.h"

#include <cstdint>
#include <ctime>

namespace StockFetchScheduler {

// 调度策略：4 个延迟参数覆盖 4 种状态。
struct Policy {
    uint32_t active_ms;    // 盘中正常间隔（或临近 session 边界）
    uint32_t idle_ms;      // 非盘中且远离边界的稀疏间隔
    uint32_t retry_ms;     // 上次失败 / 还没有 valid 数据时的快速重试
    uint32_t pre_ntp_ms;   // NTP 未同步时的兜底间隔（无法用 MarketSchedule）

    constexpr Policy(uint32_t active = 1000, uint32_t idle = 60000,
                     uint32_t retry = 1000, uint32_t preNtp = 1000)
        : active_ms(active), idle_ms(idle), retry_ms(retry), pre_ntp_ms(preNtp) {}
};

// 调用点的瞬时状态。
struct State {
    uint32_t      now_ms          = 0;     // millis()
    uint32_t      last_fetch_at_ms = 0;    // 0 = 从未 fetch（视为已就绪）
    bool          clock_ready     = false; // NTP 是否已同步
    bool          valid           = false; // 上一次 fetch 是否成功（决定 retry vs active）
    bool          in_session      = false; // 当前 symbol 市场是否盘中
    bool          block_fetch     = false; // 上层禁止 fetch（如 hover 中）
    bool          in_flight       = false; // worker in-flight 标志
    std::time_t   bj_epoch        = 0;     // 北京时间 epoch；clock_ready=false 时不用
};

// 计算当前轮次应该用的延迟（毫秒）。纯函数，不依赖 millis()。
inline uint32_t computeDelayMs(const State& s, const Policy& p) {
    if (!s.valid) return p.retry_ms;
    if (!s.clock_ready) return p.pre_ntp_ms;
    MarketSchedule::PollPolicy mp{ p.active_ms, p.idle_ms };
    return MarketSchedule::nextPollDelayMs(s.bj_epoch, mp, s.in_session);
}

// 综合判定：是否应该现在入队一次 fetch。
// 调用方：if (shouldFetch(...)) { enqueue + 更新 last_fetch_at = millis(); }
inline bool shouldFetch(const State& s, const Policy& p) {
    if (s.block_fetch || s.in_flight) return false;
    uint32_t delay = computeDelayMs(s, p);
    if (s.last_fetch_at_ms == 0) return true;          // 从未 fetch → 立即
    return (s.now_ms - s.last_fetch_at_ms) >= delay;
}

}  // namespace StockFetchScheduler

#endif  // SCENES_STOCK_STOCK_FETCH_SCHEDULER_H
