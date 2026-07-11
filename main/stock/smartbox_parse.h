// smartbox_parse.h
// 腾讯 smartbox.gtimg.cn `/s3/?q=...&t=all` 响应解析。
// header-only、无外部依赖、可脱离硬件单测。
//
// 响应约定（实测）：
//   - Body 是纯 ASCII，中文用 \uXXXX 转义（JSON-style，可直接透传给前端 JSON.parse）
//   - 格式：v_hint="<rec1>^<rec2>^..."  每条 ~ 分隔字段：market~code~name~pinyin~type
//   - type 包含：GP / GP-A / GP-B / KJ / ZS / QZ / ETF / LOF / ...
//   - 我们只过滤股票 + ETF/LOF + 指数 (ZS)，其它丢弃
//   - **美股 code 是小写**（aapl.oq），但 qt.gtimg.cn 报价接口要求大写 → 这里转大写
//
// 接口风格：本 header 把"body → record list"做成纯函数，调用方负责拼 JSON 或
// 其它输出格式。

#ifndef API_SMARTBOX_PARSE_H
#define API_SMARTBOX_PARSE_H

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace smartbox_parse {

// 自选股能用的类型：股票 + 交易所基金（ETF/LOF）+ 指数（ZS）。
// 排除：KJ（OTC 基金，市场是 jj 不是 sh/sz，被市场过滤拦掉）/ QZ（权证）/ QDII 等
inline bool isWatchlistType(const char* type) {
    if (!type) return false;
    return std::strcmp(type, "GP")   == 0 ||
           std::strcmp(type, "GP-A") == 0 ||
           std::strcmp(type, "GP-B") == 0 ||
           std::strcmp(type, "ETF")  == 0 ||
           std::strcmp(type, "LOF")  == 0 ||
           std::strcmp(type, "ZS")   == 0;
}

// market 段是否是我们消费的四个市场之一（其它如 jj/wh 直接丢）。
inline bool isWatchlistMarket(const std::string& market) {
    return market == "us" || market == "sh" || market == "sz" || market == "hk";
}

struct Record {
    std::string symbol;  // 如 "usAAPL.OQ" / "sh600519" / "hk00700"
    std::string name;    // ASCII（含 \uXXXX 转义或纯 ASCII），调用方可直接拼到 JSON 里
};

// 把 smartbox 完整响应 body parse 成 record vector。
// 自动剥外层 v_hint="..."；handles "N"（no match）→ 空 vector。
// 不消费的 record（type 不命中 / market 未知）直接丢弃。
inline std::vector<Record> parseBody(const char* body, size_t len) {
    std::vector<Record> out;
    if (!body || len == 0) return out;

    // 剥外层引号
    size_t qStart = std::string::npos, qEnd = std::string::npos;
    for (size_t i = 0; i < len; i++) {
        if (body[i] == '"') { qStart = i; break; }
    }
    for (size_t i = len; i-- > 0; ) {
        if (body[i] == '"') { qEnd = i; break; }
    }
    if (qStart == std::string::npos || qEnd <= qStart) return out;

    std::string inner(body + qStart + 1, qEnd - qStart - 1);
    if (inner == "N") return out;  // smartbox "no match" 标记

    size_t recStart = 0;
    while (recStart < inner.size()) {
        size_t recEnd = inner.find('^', recStart);
        if (recEnd == std::string::npos) recEnd = inner.size();
        std::string rec = inner.substr(recStart, recEnd - recStart);
        recStart = recEnd + 1;
        if (rec.empty()) continue;

        // 5 个 ~ 分隔字段：market ~ code ~ name ~ pinyin ~ type
        size_t p1 = rec.find('~');
        if (p1 == std::string::npos) continue;
        size_t p2 = rec.find('~', p1 + 1);
        if (p2 == std::string::npos) continue;
        size_t p3 = rec.find('~', p2 + 1);
        if (p3 == std::string::npos) continue;
        size_t p4 = rec.find('~', p3 + 1);
        if (p4 == std::string::npos) continue;

        std::string market = rec.substr(0, p1);
        std::string code   = rec.substr(p1 + 1, p2 - p1 - 1);
        std::string name   = rec.substr(p2 + 1, p3 - p2 - 1);
        std::string type   = rec.substr(p4 + 1);

        if (!isWatchlistType(type.c_str())) continue;
        if (!isWatchlistMarket(market)) continue;

        // 美股 code 转大写（aapl.oq → AAPL.OQ；接口要求大写）
        if (market == "us") {
            for (char& c : code) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
        }

        Record r;
        r.symbol = market + code;
        r.name   = name;
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace smartbox_parse

#endif  // API_SMARTBOX_PARSE_H
