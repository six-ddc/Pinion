// kline_parse.h
// 腾讯财经日 K / 周 K 接口响应里"原始字段 → POD Row"的纯解析。
// header-only、无外部依赖、可脱离硬件单测。
//
// 接口响应 key 因市场而异：
//   day  → "day"（所有市场）
//   week → A/HK 是 "qfqweek"，US 是 "week"
// 行格式：[date, open, close, high, low, volume, ...turnoverRate, amount(万元)]
//   - A/US 6 段是主体；HK 多 3 段 (`{}`/换手率/成交额)，本仓库不消费后 3 段
//   - A 股 volume 原始单位"手"，需 ×100 转股；amount 接口给"万元"，所有市场都 ×10000

#ifndef API_KLINE_PARSE_H
#define API_KLINE_PARSE_H

#include "date_parse.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kline_parse {

// 候选 key 列表（4 元素，末位 nullptr）。
// 调用方按顺序在 cJSON 文档里探，第一个非空数组即为 rows。
// out[0] = periodParam ("day" / "week")
// out[1] = "qfqweek"  fallback (A/HK 周 K)
// out[2] = "qfqday"   fallback (A/HK 日 K 在某些版本)
inline void fillCandidateKeys(const char* periodParam, const char* out[4]) {
    out[0] = periodParam ? periodParam : "day";
    out[1] = "qfqweek";
    out[2] = "qfqday";
    out[3] = nullptr;
}

// 单行 K 线的"业务字段"（已做完单位换算和 valid 校验）。
struct Row {
    uint32_t timestamp_s = 0;
    float open = 0;
    float close = 0;
    float high = 0;
    float low = 0;
    double volume = 0;        // 已统一到"股"（A 股 ×100）
    double amount = 0;        // 已统一到"元"（接口"万元" ×10000）
    float turnover_rate = 0;  // %（A 股有，HK/US 通常 0）
    bool valid = false;
};

// 把单行原始字段（date string + 6 个数字）转成 Row。
// rawAmountTenThousand: 是否需要将 amount 字段 ×10000 转元（true=接口单位"万元"）。
// aShareVolumeUnit:    是否需要将 volume 字段 ×100 转股（true=接口单位"手"）。
inline Row makeRow(const char* date,
                   float open, float close, float high, float low,
                   double rawVolume, double rawAmount,
                   float turnoverRate,
                   bool aShareVolumeUnit, bool rawAmountTenThousand) {
    Row r;
    if (!date || !(open > 0) || !(close > 0) || !(high > 0) || !(low > 0)) {
        return r;  // valid=false
    }
    r.timestamp_s   = static_cast<uint32_t>(dateStrToEpoch(date));
    r.open          = open;
    r.close         = close;
    r.high          = high;
    r.low           = low;
    r.volume        = aShareVolumeUnit ? rawVolume * 100.0 : rawVolume;
    r.amount        = rawAmountTenThousand ? rawAmount * 10000.0 : rawAmount;
    r.turnover_rate = turnoverRate;
    r.valid         = true;
    return r;
}

}  // namespace kline_parse

#endif  // API_KLINE_PARSE_H
