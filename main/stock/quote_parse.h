// quote_parse.h
// 腾讯 qt.gtimg.cn `q=` 报价响应的 header-only 解析。纯逻辑无外部依赖，可脱离硬件单测。
//
// 解析接受 [bodyStart, bodyEnd) range：多 symbol 响应（如 `q=sh600519,sz000002`）不能
// 用 `lastIndexOf('"')` 找终止符（会跑到最后一只股的尾引号），需按块切；另提供
// findQuoteBlock 用于切块。
//
// GBK 安全性：
//  - 报价响应是 GBK 编码（中文名）。GBK 双字节字符的低字节范围是 0x40-0xFE，包含
//    `~` (0x7E)，所以 naive 按 `~` 切字段会被汉字字节切错。
//  - 对应规避：用 `~<echoCode>~` 锚点跳过名字段，从锚点之后再按 `~` 切（锚点
//    之后全是 ASCII 数字 / 日期 / 货币码，安全）。
//  - `"` (0x22) **不在** GBK 低字节范围，所以扫 `"` 找块边界 100% 安全。

#ifndef QUOTE_PARSE_H
#define QUOTE_PARSE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace quote_parse {

enum class Market { A_SH, A_SZ, HK, US, UNKNOWN };

inline Market marketOf(const char* sym) {
    if (!sym) return Market::UNKNOWN;
    if (strncmp(sym, "sh", 2) == 0) return Market::A_SH;
    if (strncmp(sym, "sz", 2) == 0) return Market::A_SZ;
    if (strncmp(sym, "hk", 2) == 0) return Market::HK;
    if (strncmp(sym, "us", 2) == 0) return Market::US;
    return Market::UNKNOWN;
}

inline bool isAShare(Market m) { return m == Market::A_SH || m == Market::A_SZ; }

// 取响应里 fields[2] 回显的"代号回声"段 — sz000002 → "000002"，usVEEV.N → "VEEV.N"。
// buf 至少 24 字节，成功后 NUL-terminated。
inline bool echoCodeOf(const char* sym, char* buf, size_t bufLen) {
    if (!sym || marketOf(sym) == Market::UNKNOWN || bufLen == 0) return false;
    size_t len = strlen(sym + 2);
    if (len + 1 > bufLen) return false;
    memcpy(buf, sym + 2, len + 1);
    return true;
}

// 腾讯 qt.gtimg.cn `q=` 响应字段索引（以 `~<echoCode>~` 锚点之后的字段为 0）。
// 字段表实测自 2024 ~ 2026 真实响应；A/HK/US 三市索引一致（部分字段 HK/US 缺失为空）。
// 修改时请同步更新 test/test_quote_batch_parse 的 snapshot 断言。
namespace TencentQuoteFields {
    constexpr size_t kCurrent       = 0;   // 当前价
    constexpr size_t kLastClose     = 1;   // 昨收
    constexpr size_t kOpen          = 2;   // 今开
    // 3..27 含名字、代码回声、成交量/额、买卖五档、时间戳等中间字段
    constexpr size_t kChg           = 28;  // 涨跌额
    constexpr size_t kPercent       = 29;  // 涨跌幅 %
    constexpr size_t kHigh          = 30;  // 最高
    constexpr size_t kLow           = 31;  // 最低
    // 32: <currentPrice/cumVolume/cumAmount> 复合字段
    constexpr size_t kVolume        = 33;  // 成交量（A 股原始单位"手"，需 ×100 转股）
    constexpr size_t kAmount        = 34;  // 成交额（A 股原始单位"万元"，需 ×10000 转元）
    constexpr size_t kTurnoverRate  = 35;  // 换手率 %（A 股有；港/美股可能 0）
    // 36..39: 市盈率 / 总市值 / 流通市值 等
    constexpr size_t kAmplitude     = 40;  // 振幅 %（A 股直给；其它市场缺失则由 high/low/last_close 算）

    constexpr size_t kMinFieldCount = 32;  // < 此值视为响应畸形拒绝
}

// 单股报价数值字段（不含 symbol / name / fetched_at — 调用方注入）。
struct QuoteFields {
    float current       = 0;
    float last_close    = 0;
    float open          = 0;
    float chg           = 0;
    float percent       = 0;
    float high          = 0;
    float low           = 0;
    float amplitude     = 0;
    float turnover_rate = 0;
    double volume       = 0;   // 已统一到"股"（A 股 ×100）
    double amount       = 0;   // 已统一到"元"（A 股 ×10000）
    float avg_price     = 0;   // amount / volume，volume>0 才填
    bool valid          = false;
};

// 在 [bodyStart, bodyEnd) 范围内解析单股 ~<echoCode>~ 锚点之后的字段。
// body 不需要 NUL 终止；只读 [0, bodyEnd) 字节。
inline bool parseQuote(const char* body, int bodyStart, int bodyEnd,
                       const char* sym, QuoteFields& out) {
    if (!body || !sym || bodyEnd <= bodyStart) return false;

    char echo[24];
    if (!echoCodeOf(sym, echo, sizeof(echo))) return false;

    char anchor[28];
    int alen = (int)strlen(echo);
    if (alen + 2 >= (int)sizeof(anchor)) return false;
    anchor[0] = '~';
    memcpy(anchor + 1, echo, alen);
    anchor[1 + alen] = '~';
    anchor[2 + alen] = '\0';
    int anchorLen = alen + 2;

    int pos = -1;
    int searchEnd = bodyEnd - anchorLen;
    for (int i = bodyStart; i <= searchEnd; i++) {
        if (body[i] == '~' && memcmp(body + i, anchor, anchorLen) == 0) {
            pos = i;
            break;
        }
    }
    if (pos < 0) return false;
    int start = pos + anchorLen;

    // GBK 安全：`"` 不在 GBK 低字节范围，扫第一个 `"` 即为该行尾引号。
    int end = -1;
    for (int i = start; i < bodyEnd; i++) {
        if (body[i] == '"') { end = i; break; }
    }
    if (end < 0) end = bodyEnd;
    if (end <= start) return false;

    constexpr size_t kMaxFields = 64;
    int fieldStart[kMaxFields];
    int fieldEnd[kMaxFields];
    size_t fc = 0;
    int cur = start;
    fieldStart[0] = cur;
    while (cur < end && fc < kMaxFields - 1) {
        if (body[cur] == '~') {
            fieldEnd[fc] = cur;
            fc++;
            fieldStart[fc] = cur + 1;
        }
        cur++;
    }
    fieldEnd[fc] = end;
    fc++;

    if (fc < TencentQuoteFields::kMinFieldCount) return false;

    auto fieldFloat = [&](size_t idx) -> float {
        if (idx >= fc) return 0.0f;
        int s = fieldStart[idx], e = fieldEnd[idx];
        if (e <= s) return 0.0f;
        // strtof 在 `~` / `"` 处天然停止，body 不需要 NUL 终止。
        return strtof(body + s, nullptr);
    };
    auto fieldDouble = [&](size_t idx) -> double {
        if (idx >= fc) return 0.0;
        int s = fieldStart[idx], e = fieldEnd[idx];
        if (e <= s) return 0.0;
        return strtod(body + s, nullptr);
    };

    Market m = marketOf(sym);

    using namespace TencentQuoteFields;
    out.current     = fieldFloat(kCurrent);
    out.last_close  = fieldFloat(kLastClose);
    out.open        = fieldFloat(kOpen);
    out.chg         = fieldFloat(kChg);
    out.percent     = fieldFloat(kPercent);
    out.high        = fieldFloat(kHigh);
    out.low         = fieldFloat(kLow);

    double rawVol = fieldDouble(kVolume);
    double rawAmt = fieldDouble(kAmount);
    if (isAShare(m)) {
        out.volume = rawVol * 100.0;        // 手 → 股
        out.amount = rawAmt * 10000.0;      // 万元 → 元
    } else {
        out.volume = rawVol;
        out.amount = rawAmt;
    }

    out.turnover_rate = fieldFloat(kTurnoverRate);
    float amp = fieldFloat(kAmplitude);
    if (amp > 0) {
        out.amplitude = amp;
    } else if (out.last_close > 0 && out.high > 0 && out.low > 0) {
        out.amplitude = (out.high - out.low) / out.last_close * 100.0f;
    }

    if (out.volume > 0) {
        out.avg_price = (float)(out.amount / out.volume);
    }

    out.valid = (out.current > 0 && out.last_close > 0);
    return out.valid;
}

// 在 [bodyStart, bodyEnd) 内定位 `v_<reqSym>="..."` 块边界。
// 成功时 outStart 指向块的 `v_` 字节，outEnd 指向尾引号 `"` 的后一位。
// 用于多 symbol 响应把整个 body 切成单股块。
inline bool findQuoteBlock(const char* body, int bodyStart, int bodyEnd,
                           const char* reqSym, int& outStart, int& outEnd) {
    if (!body || !reqSym || bodyEnd <= bodyStart) return false;
    char needle[32];
    int symLen = (int)strlen(reqSym);
    if (symLen + 4 > (int)sizeof(needle)) return false;
    needle[0] = 'v'; needle[1] = '_';
    memcpy(needle + 2, reqSym, symLen);
    needle[2 + symLen] = '=';
    int needleLen = 3 + symLen;

    int searchEnd = bodyEnd - needleLen;
    int pos = -1;
    for (int i = bodyStart; i <= searchEnd; i++) {
        if (body[i] == 'v' && memcmp(body + i, needle, needleLen) == 0) {
            pos = i;
            break;
        }
    }
    if (pos < 0) return false;

    int eqPos = pos + needleLen - 1;  // 指向 '='
    int quoteOpen = -1;
    for (int i = eqPos + 1; i < bodyEnd; i++) {
        if (body[i] == '"') { quoteOpen = i; break; }
    }
    if (quoteOpen < 0) return false;
    int quoteClose = -1;
    for (int i = quoteOpen + 1; i < bodyEnd; i++) {
        if (body[i] == '"') { quoteClose = i; break; }
    }
    if (quoteClose < 0) return false;
    outStart = pos;
    outEnd = quoteClose + 1;
    return true;
}

}  // namespace quote_parse

#endif  // QUOTE_PARSE_H
