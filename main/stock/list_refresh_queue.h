// list_refresh_queue.h
// 列表页 per-row 抓取队列调度器。首轮补齐不等周期，周期只约束刷新阶段。
// header-only、无外部依赖、可脱离硬件单测。

#ifndef SCENES_STOCK_LIST_REFRESH_QUEUE_H
#define SCENES_STOCK_LIST_REFRESH_QUEUE_H

#include <cstddef>
#include <cstdint>

namespace ListRefreshQueue {

struct ItemState {
    bool     valid              = false;
    uint32_t last_attempt_at_ms = 0;  // 0 = 从未尝试
};

struct Policy {
    uint32_t refresh_step_ms = 1500;  // 全部 valid 后，队列步进刷新间隔
    uint32_t retry_ms        = 1500;  // 单行失败后的重试间隔
};

enum class Reason : uint8_t {
    None = 0,
    InitialFill,
    Retry,
    Refresh,
};

struct State {
    uint32_t         now_ms          = 0;
    uint32_t         last_step_at_ms = 0;  // 上一次成功入队的时间
    std::size_t      cursor          = 0;  // round-robin 起点
    std::size_t      count           = 0;
    bool             in_flight       = false;
    const ItemState* items           = nullptr;
};

struct Decision {
    bool        should_fetch = false;
    std::size_t index        = 0;
    std::size_t next_cursor  = 0;
    Reason      reason       = Reason::None;
};

inline bool due(uint32_t now_ms, uint32_t last_ms, uint32_t delay_ms) {
    if (last_ms == 0) return true;
    return (uint32_t)(now_ms - last_ms) >= delay_ms;
}

inline Decision makeDecision(std::size_t index, std::size_t count,
                             Reason reason) {
    Decision d;
    d.should_fetch = true;
    d.index = index;
    d.next_cursor = count > 0 ? (index + 1) % count : 0;
    d.reason = reason;
    return d;
}

inline Decision none(std::size_t cursor, std::size_t count) {
    Decision d;
    d.next_cursor = count > 0 ? cursor % count : 0;
    return d;
}

// 决策优先级：
// 1. 从未尝试过的行：立即补齐首轮，不受 refresh_step_ms 限制。
// 2. 失败且到 retry_ms 的行：按单行重试节奏补。
// 3. 已有数据的行：按 refresh_step_ms 做 round-robin 刷新。
inline Decision planNext(const State& s, const Policy& p) {
    if (s.in_flight || !s.items || s.count == 0) return none(s.cursor, s.count);

    std::size_t start = s.cursor % s.count;
    for (std::size_t off = 0; off < s.count; off++) {
        std::size_t idx = (start + off) % s.count;
        const ItemState& item = s.items[idx];
        if (!item.valid && item.last_attempt_at_ms == 0) {
            return makeDecision(idx, s.count, Reason::InitialFill);
        }
    }

    for (std::size_t off = 0; off < s.count; off++) {
        std::size_t idx = (start + off) % s.count;
        const ItemState& item = s.items[idx];
        if (!item.valid && due(s.now_ms, item.last_attempt_at_ms, p.retry_ms)) {
            return makeDecision(idx, s.count, Reason::Retry);
        }
    }

    if (!due(s.now_ms, s.last_step_at_ms, p.refresh_step_ms)) {
        return none(s.cursor, s.count);
    }

    for (std::size_t off = 0; off < s.count; off++) {
        std::size_t idx = (start + off) % s.count;
        if (s.items[idx].valid) {
            return makeDecision(idx, s.count, Reason::Refresh);
        }
    }

    return none(s.cursor, s.count);
}

}  // namespace ListRefreshQueue

#endif  // SCENES_STOCK_LIST_REFRESH_QUEUE_H
