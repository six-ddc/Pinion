// tencent_endpoints.h
// 腾讯财经接口的 URL builder + symbol 规范化——header-only 纯字符串运算、无外部依赖、
// 可脱离硬件单测。覆盖各市场 endpoint 路径差异、美股后缀剥离、kline param 6 段格式等。
//
// 调用方拿到 std::string 后再按需追加 cache buster (`&_=<millis>`) 等运行期行为。

#ifndef API_TENCENT_ENDPOINTS_H
#define API_TENCENT_ENDPOINTS_H

#include "quote_parse.h"   // Market enum (header-only, host-safe)

#include <cstdio>
#include <cstring>
#include <string>

namespace tencent_endpoints {

using quote_parse::Market;
using quote_parse::marketOf;

// ---------------------------------------------------------------------------
// 报价（qt.gtimg.cn）
// ---------------------------------------------------------------------------

// 报价请求的 symbol：美股**不能**带 `.N`/`.OQ` 后缀（腾讯会返回
// `v_pv_none_match="1";`），其它市场原样返回。响应里 idx[2] 仍然回显
// 带后缀的代号，所以解析端的 anchor 不受影响。
inline std::string quoteSymbolOf(const char* sym) {
    if (!sym) return {};
    if (marketOf(sym) == Market::US) {
        const char* dot = std::strchr(sym, '.');
        if (dot) return std::string(sym, static_cast<size_t>(dot - sym));
    }
    return sym;
}

// 报价 URL — qParam 已经拼好（单股或 "sym1,sym2,..." 批量）。
// 不含 cache buster；调用方按需追加 "&_=<ts>"。
inline std::string quoteUrl(const char* qParam) {
    return std::string("http://qt.gtimg.cn/?q=") + (qParam ? qParam : "");
}

inline std::string marketStatUrl() {
    return "http://qt.gtimg.cn/q=marketStat";
}

// ---------------------------------------------------------------------------
// 分时（web.ifzq.gtimg.cn appstock/app）
//   A/SH/SZ: minute/query
//   HK:      hkMinute/query
//   US:      UsMinute/query    （注意大写 U / M）
// ---------------------------------------------------------------------------

inline const char* minuteController(Market m) {
    switch (m) {
        case Market::HK: return "hkMinute";
        case Market::US: return "UsMinute";
        default:         return "minute";
    }
}

inline std::string minuteUrl(const char* symbol, Market m) {
    return std::string("https://web.ifzq.gtimg.cn/appstock/app/")
           + minuteController(m) + "/query?code=" + (symbol ? symbol : "");
}

// ---------------------------------------------------------------------------
// 5 日分时
//   A/SH/SZ/HK: day/query
//   US:         dayus/query    （**不是** day/query，会 param error）
// ---------------------------------------------------------------------------

inline const char* fiveDayController(Market m) {
    return (m == Market::US) ? "dayus" : "day";
}

inline std::string fiveDayUrl(const char* symbol, Market m) {
    return std::string("https://web.ifzq.gtimg.cn/appstock/app/")
           + fiveDayController(m) + "/query?code=" + (symbol ? symbol : "");
}

// ---------------------------------------------------------------------------
// 日 K / 周 K
//   A/SH/SZ: fqkline
//   HK:      hkfqkline
//   US:      usfqkline
//   param 6 段：<symbol>,<period>,<start>,<endDate>,<count>,<fq>
//   start 通常空；endDate 空 → 拉最新 count 根；fq ∈ qfq/hfq/bfq
// ---------------------------------------------------------------------------

inline const char* klineController(Market m) {
    switch (m) {
        case Market::HK: return "hkfqkline";
        case Market::US: return "usfqkline";
        default:         return "fqkline";
    }
}

// period: "day" / "week"。endDate 为空时拉最新 count 根。fq 默认 "qfq"。
inline std::string klineUrl(const char* symbol, Market m,
                            const char* period, const char* endDate,
                            int count, const char* fq = "qfq") {
    std::string url("https://web.ifzq.gtimg.cn/appstock/app/");
    url += klineController(m);
    url += "/get?param=";
    url += (symbol ? symbol : "");
    url += ",";
    url += (period ? period : "day");
    url += ",,";                          // start 段恒空
    url += (endDate ? endDate : "");
    url += ",";
    url += std::to_string(count);
    url += ",";
    url += (fq ? fq : "qfq");
    return url;
}

// ---------------------------------------------------------------------------
// 搜索（smartbox.gtimg.cn）
// ---------------------------------------------------------------------------

// 项目内 symbol → smartbox 可识别的查询：
//   sh/sz/hk + 纯数字   → 剥 prefix（"sh588080" → "588080"）
//   us + ticker.suffix  → 剥 prefix + 剥 .OQ/.N 后缀（"usAAPL.OQ" → "AAPL"）
//   其它（拼音/中文名/纯代码）原样返回，交给 smartbox 模糊匹配
inline std::string normalizeSearchQuery(const char* utf8Query) {
    if (!utf8Query) return {};
    std::string q(utf8Query);
    if (q.size() < 3) return q;
    char p0 = q[0], p1 = q[1];
    if (p0 >= 'A' && p0 <= 'Z') p0 = static_cast<char>(p0 - 'A' + 'a');
    if (p1 >= 'A' && p1 <= 'Z') p1 = static_cast<char>(p1 - 'A' + 'a');
    std::string rest = q.substr(2);
    if ((p0 == 's' && (p1 == 'h' || p1 == 'z')) || (p0 == 'h' && p1 == 'k')) {
        bool allDigits = !rest.empty();
        for (char c : rest) {
            if (c < '0' || c > '9') { allDigits = false; break; }
        }
        if (allDigits) return rest;
    }
    if (p0 == 'u' && p1 == 's') {
        size_t dot = rest.find('.');
        if (dot != std::string::npos && dot > 0) return rest.substr(0, dot);
    }
    return q;
}

// UTF-8 → percent-encoded（保留 unreserved 字符）。
inline std::string urlEncodeUtf8(const char* s) {
    if (!s) return {};
    std::string out;
    out.reserve(std::strlen(s) * 3);
    for (const char* p = s; *p; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

inline std::string searchUrl(const char* utf8Query) {
    std::string q = normalizeSearchQuery(utf8Query);
    return "https://smartbox.gtimg.cn/s3/?t=all&q=" + urlEncodeUtf8(q.c_str());
}

// ---------------------------------------------------------------------------
// 展示用 symbol 化简 — 剥市场前缀 + 美股 .N/.OQ 后缀。
// ---------------------------------------------------------------------------

inline std::string stockDisplayCode(const char* symbol) {
    if (!symbol || !*symbol) return {};
    std::string s(symbol);
    if (s.size() >= 3) {
        char c0 = s[0], c1 = s[1];
        if (c0 >= 'A' && c0 <= 'Z') c0 = static_cast<char>(c0 - 'A' + 'a');
        if (c1 >= 'A' && c1 <= 'Z') c1 = static_cast<char>(c1 - 'A' + 'a');
        bool isPrefix =
            (c0 == 's' && (c1 == 'h' || c1 == 'z')) ||
            (c0 == 'b' &&  c1 == 'j')              ||
            (c0 == 'h' &&  c1 == 'k')              ||
            (c0 == 'u' &&  c1 == 's');
        if (isPrefix) s = s.substr(2);
    }
    size_t dot = s.find('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    return s;
}

}  // namespace tencent_endpoints

#endif  // API_TENCENT_ENDPOINTS_H
