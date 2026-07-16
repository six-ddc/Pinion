// market_hours.h
// 本地推算"某 symbol 的市场当前是否盘中"。header-only、纯算法、可脱离硬件单测。
//
// Claw4 靠 marketStat 轮询拿权威开闭市状态；Claw6 的 stock_chart 控件为省一路
// HTTP 改为本地近似推算：节假日/DST 误判的代价只是几次多余轮询（active vs idle
// 间隔），不会产生错误数据——与 market_schedule.h 的边界表同一取舍口径。
//
// epoch 约定与 market_schedule.h 相同：传入 "BJ-as-UTC epoch"（utc + 8h）。

#ifndef MARKET_HOURS_H
#define MARKET_HOURS_H

#include "quote_parse.h"

#include <time.h>

namespace market_hours {

// 当天分钟数（BJ wall-clock）。
inline int minuteOfDay(time_t bjEpoch) { return static_cast<int>((bjEpoch % 86400) / 60); }

// 星期（0=周日 … 6=周六）。1970-01-01 是周四。
inline int dayOfWeek(time_t bjEpoch) { return static_cast<int>((bjEpoch / 86400 + 4) % 7); }

// symbol 所属市场当前是否盘中（近似：不考虑节假日；美股按 EDT/EST 并集放宽）。
inline bool inSession(const char* symbol, time_t bjEpoch) {
    using quote_parse::Market;
    Market m = quote_parse::marketOf(symbol);
    int mod = minuteOfDay(bjEpoch);
    int dow = dayOfWeek(bjEpoch);
    bool weekday = dow >= 1 && dow <= 5;
    switch (m) {
        case Market::A_SH:
        case Market::A_SZ:
            return weekday && ((mod >= 9 * 60 + 30 && mod < 11 * 60 + 30) || (mod >= 13 * 60 && mod < 15 * 60));
        case Market::HK:
            return weekday && ((mod >= 9 * 60 + 30 && mod < 12 * 60) || (mod >= 13 * 60 && mod < 16 * 60));
        case Market::US:
            // 美股常规时段 BJ 21:30(EDT)/22:30(EST) → 次日 04:00/05:00：取并集 21:30–05:00。
            // 晚段落在 BJ 周一~周五，凌晨段落在 BJ 周二~周六。
            if (mod >= 21 * 60 + 30) return weekday;
            if (mod < 5 * 60) return dow >= 2 && dow <= 6;
            return false;
        default:
            return false;
    }
}

}  // namespace market_hours

#endif  // MARKET_HOURS_H
