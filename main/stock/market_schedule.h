// market_schedule.h
// 边界感知自适应轮询调度 — header-only，纯算法零外部依赖，可脱离硬件单测。
//
// 思路：行情类轮询（marketStat / quote / chart）共享同一套节奏决策：
//   1. inSession=true                     → activeMs（盘中正常刷新节奏）
//   2. inSession=false 且 临近边界 ±2min  → activeMs（卡边界一瞬秒拿新状态/价）
//   3. inSession=false 且 远离边界        → idleMs（节省 HTTP + 减少功耗）
//
// "边界"= 各市场开盘/午休/收盘 BJ wall-clock 候选点（A/HK/US 三市汇总 ~10 个）。
// DST / 节假日不本地推算 —— EDT/EST 两个 US 开盘时刻都列上，节假日 marketStat 会
// 答"已休市"。错的那个最多浪费几次 HTTP，不会让状态出错。
//
// 公开 API：
//   PollPolicy{activeMs, idleMs}
//   nextPollDelayMs(bjEpoch, policy, inSession) → 距离下次该 fire 的毫秒数
//   utcToBjEpoch(utcEpoch)                      → +8h，避免调用方写死 28800
//   secsToNextBoundary / secsSincePrevBoundary  → 暴露给单测和需要细控的调用方
//
// epoch 约定：本模块的所有"BJ epoch"指**把 BJ wall-clock 时间当作 UTC 解释得到的 epoch 秒**。
// 这样无论调用方真实时区是什么，传 (utc_epoch + 8*3600) 进来就行，无需切 setenv("TZ").

#ifndef MARKET_SCHEDULE_H
#define MARKET_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace MarketSchedule {

// ---- 调度参数 ----
constexpr uint32_t kMinDelayMs    = 1000;    // 兜底下限，防止 0/极小值
constexpr uint32_t kMaxDelayMs    = 60000;
constexpr int      kNearWindowSec = 120;     // 边界 ±2min 内算"临近"
constexpr int      kEarlyWakeSec  = 4;       // 临近时提前 ~4s 唤醒，争取卡边界

struct PollPolicy {
    uint32_t activeMs;   // 盘中 or 边界 ±2min 用
    uint32_t idleMs;     // 远离边界用
};

// 北京 wall-clock 上的"候选切换时刻"。每天重复；工作日 / 节假日均列出 —— 节假日
// 那天 marketStat 会答"已休市"，纯浪费几次密集 polling，不会拿到错的状态。
// dayOffset=1 用于跨午夜（美股 BJ 凌晨收盘）。
struct Boundary {
    int hour;       // 0-23 BJ
    int minute;     // 0-59
    int dayOffset;  // 0 = 当天；1 = 次日
};

inline const Boundary* boundaries(size_t& outCount) {
    static constexpr Boundary kTable[] = {
        // A 股 + HK
        { 9, 30, 0},   // A/HK 开盘
        {11, 30, 0},   // A 股午休开始
        {12,  0, 0},   // HK 午休开始
        {13,  0, 0},   // A/HK 下午开盘
        {15,  0, 0},   // A 股收盘
        {16,  0, 0},   // HK 收盘
        // 美股（EDT / EST 两套时刻都列上，无需本地算 DST）
        {21, 30, 0},   // 美股开盘 (EDT)
        {22, 30, 0},   // 美股开盘 (EST)
        { 4,  0, 1},   // 美股收盘 (EDT)，BJ 次日凌晨
        { 5,  0, 1},   // 美股收盘 (EST)，BJ 次日凌晨
    };
    outCount = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

// 给定 BJ-as-UTC epoch，返回当天 00:00 (BJ-as-UTC epoch)。
inline time_t bjStartOfDay(time_t bjEpoch) {
    return (bjEpoch / 86400) * 86400;
}

// 把一条 boundary 从某个 originDay 展开成绝对 BJ-as-UTC epoch。
inline time_t boundaryAt(const Boundary& b, time_t originDayStart) {
    return originDayStart + b.dayOffset * 86400
         + b.hour * 3600 + b.minute * 60;
}

// 距离下一个候选边界的秒数（>0）。
// 用三个 origin day（昨天/今天/明天）展开，覆盖 dayOffset=0/1 与跨午夜场景。
inline long secsToNextBoundary(time_t bjEpoch) {
    size_t n = 0;
    const Boundary* tbl = boundaries(n);

    time_t today = bjStartOfDay(bjEpoch);
    time_t origins[3] = { today - 86400, today, today + 86400 };

    long best = -1;
    for (size_t i = 0; i < n; i++) {
        for (int o = 0; o < 3; o++) {
            time_t cand = boundaryAt(tbl[i], origins[o]);
            long diff = (long)(cand - bjEpoch);
            if (diff <= 0) continue;
            if (best < 0 || diff < best) best = diff;
        }
    }
    return best < 0 ? 86400 : best;
}

// 距离上一个候选边界的秒数（>=0）。
inline long secsSincePrevBoundary(time_t bjEpoch) {
    size_t n = 0;
    const Boundary* tbl = boundaries(n);

    time_t today = bjStartOfDay(bjEpoch);
    time_t origins[3] = { today - 86400, today, today + 86400 };

    long best = -1;
    for (size_t i = 0; i < n; i++) {
        for (int o = 0; o < 3; o++) {
            time_t cand = boundaryAt(tbl[i], origins[o]);
            long diff = (long)(bjEpoch - cand);
            if (diff < 0) continue;
            if (best < 0 || diff < best) best = diff;
        }
    }
    return best < 0 ? 86400 : best;
}

inline bool isNearBoundary(time_t bjEpoch) {
    return secsToNextBoundary(bjEpoch) < kNearWindowSec
        || secsSincePrevBoundary(bjEpoch) < kNearWindowSec;
}

// 主接口。
//   inSession=true              → activeMs
//   inSession=false, 近边界     → activeMs，但若 toNext 距离比 activeMs 还短，
//                                  直接锚到 toNext - kEarlyWakeSec，争取边界那一秒下一次 poll 就到
//   inSession=false, 远离边界   → idleMs
// 结果 clamp 到 [kMinDelayMs, kMaxDelayMs]。
inline uint32_t nextPollDelayMs(time_t bjEpoch,
                                 const PollPolicy& p,
                                 bool inSession) {
    uint32_t delayMs;
    if (inSession) {
        delayMs = p.activeMs;
    } else if (isNearBoundary(bjEpoch)) {
        long toNext = secsToNextBoundary(bjEpoch);
        long aim = toNext - kEarlyWakeSec;
        long aimMs = (aim > 0) ? aim * 1000L : 0;
        if (aimMs <= 0 || (uint32_t)aimMs > p.activeMs) {
            delayMs = p.activeMs;
        } else {
            delayMs = (uint32_t)aimMs;
        }
    } else {
        delayMs = p.idleMs;
    }

    if (delayMs < kMinDelayMs) delayMs = kMinDelayMs;
    if (delayMs > kMaxDelayMs) delayMs = kMaxDelayMs;
    return delayMs;
}

// 把 UTC epoch 加 +8h 得到 "BJ-as-UTC epoch"。
inline time_t utcToBjEpoch(time_t utcEpoch) {
    return utcEpoch + 8 * 3600;
}

}  // namespace MarketSchedule

#endif  // MARKET_SCHEDULE_H
