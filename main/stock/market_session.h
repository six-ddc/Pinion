// market_session.h
// 各市场当天交易时段总分钟数 — 用于分时图（CHART_MIN_1D）X 轴留白计算。
//
// 盘中只画到当前时刻就停，后面留空，让用户能看出「今天还剩多少时间」。
// 实现上：lv_chart_set_point_count(sessionTotal)，数据存前 N 个 id，
// 剩下保持 LV_CHART_POINT_NONE 即可自然留白。
//
// 时段长度：
//   A 股 (sh/sz/bj)：09:30-11:30 + 13:00-15:00 = 4h00 = 240 min
//   港股 (hk)      ：09:30-12:00 + 13:00-16:00 = 5h30 = 330 min
//   美股 (us)      ：09:30-16:00 + 一根 16:00 收盘 tick = 391 个 minute slot
//                  （腾讯 minute 接口对美股最多返回 391 行）

#ifndef MARKET_SESSION_H
#define MARKET_SESSION_H

#include <stddef.h>

// 返回 symbol 所属市场当天交易时段总分钟数。
// 不识别的 symbol 默认 391（取最长上限，对未知市场更不容易把数据画歪）。
// symbol 是腾讯原生格式：sh/sz/bj<6> / hk<5> / us<TICKER>.<N|OQ>。
inline int marketSessionMinutes(const char* symbol) {
    if (!symbol) return 391;
    if (symbol[0] == 'h' && symbol[1] == 'k') return 330;
    if (symbol[0] == 'u' && symbol[1] == 's') return 391;
    if ((symbol[0] == 's' && (symbol[1] == 'h' || symbol[1] == 'z'))
        || (symbol[0] == 'b' && symbol[1] == 'j')) {
        return 240;
    }
    return 391;
}

#endif  // MARKET_SESSION_H
