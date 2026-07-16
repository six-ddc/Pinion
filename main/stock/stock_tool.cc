// stock_tool.cc
// stock 工具实现：query 搜索（smartbox）→ 批量报价，或 symbols 直接批量报价。
// agent worker 线程同步执行；返回紧凑 JSON（malloc，调用方 free）。

#include "stock_tool.h"

#include "stock_api.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxSymbols = 8;      // 一次报价上限（腾讯批量接口上限 16，取保守值）
constexpr size_t kMaxSearchQuotes = 5; // query 模式取前 N 条搜索结果报价

char* DupString(const std::string& s) {
    char* out = static_cast<char*>(malloc(s.size() + 1));
    if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

char* Fail(bool* is_error, const std::string& msg) {
    *is_error = true;
    return DupString(msg);
}

// 保留 2 位小数的紧凑数字（cJSON 的 %1.15g 会放大 float 精度噪声，如 1705.00 → 1705.0000610…）。
double Round2(double v) { return static_cast<double>(static_cast<long long>(v * 100 + (v >= 0 ? 0.5 : -0.5))) / 100.0; }

void AddQuote(cJSON* arr, const StockQuote& q) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "sym", q.symbol.c_str());
    if (!q.name.empty()) cJSON_AddStringToObject(o, "name", q.name.c_str());
    cJSON_AddNumberToObject(o, "price", Round2(q.current));
    cJSON_AddNumberToObject(o, "chg", Round2(q.chg));
    cJSON_AddNumberToObject(o, "pct", Round2(q.percent));
    cJSON_AddNumberToObject(o, "open", Round2(q.open));
    cJSON_AddNumberToObject(o, "high", Round2(q.high));
    cJSON_AddNumberToObject(o, "low", Round2(q.low));
    cJSON_AddNumberToObject(o, "prev_close", Round2(q.last_close));
    cJSON_AddNumberToObject(o, "vol", static_cast<double>(static_cast<long long>(q.volume)));
    cJSON_AddNumberToObject(o, "amount", static_cast<double>(static_cast<long long>(q.amount)));
    cJSON_AddItemToArray(arr, o);
}

}  // namespace

extern "C" char* pi_stock_tool_run(const cJSON* args, bool* is_error) {
    *is_error = false;
    std::vector<std::string> syms;
    std::vector<std::string> names;  // 与 syms 对齐；query 模式来自 smartbox，symbols 模式为空
    std::string err;

    const cJSON* jquery = cJSON_GetObjectItemCaseSensitive(args, "query");
    const cJSON* jsymbols = cJSON_GetObjectItemCaseSensitive(args, "symbols");

    if (cJSON_IsString(jquery) && jquery->valuestring != nullptr && jquery->valuestring[0] != '\0') {
        std::string found_json;
        if (!stock_api::SearchStocks(jquery->valuestring, found_json, err)) {
            return Fail(is_error, "search failed: " + err);
        }
        cJSON* recs = cJSON_Parse(found_json.c_str());
        if (recs != nullptr) {
            const cJSON* rec = nullptr;
            cJSON_ArrayForEach(rec, recs) {
                if (syms.size() >= kMaxSearchQuotes) break;
                const cJSON* jsym = cJSON_GetObjectItemCaseSensitive(rec, "symbol");
                const cJSON* jname = cJSON_GetObjectItemCaseSensitive(rec, "name");
                if (!cJSON_IsString(jsym) || jsym->valuestring[0] == '\0') continue;
                syms.emplace_back(jsym->valuestring);
                names.emplace_back(cJSON_IsString(jname) ? jname->valuestring : "");
            }
            cJSON_Delete(recs);
        }
        if (syms.empty()) return DupString("{\"quotes\":[],\"note\":\"no stock matched the query\"}");
    } else if (cJSON_IsArray(jsymbols)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, jsymbols) {
            if (syms.size() >= kMaxSymbols) break;
            if (cJSON_IsString(it) && it->valuestring[0] != '\0') syms.emplace_back(it->valuestring);
        }
        names.resize(syms.size());
        if (syms.empty()) return Fail(is_error, "symbols is empty");
    } else {
        return Fail(is_error, "give query (name/pinyin/code) or symbols (array)");
    }

    std::vector<StockQuote> quotes(syms.size());
    if (!stock_api::FetchQuoteBatch(syms.data(), syms.size(), quotes.data(), err)) {
        return Fail(is_error, "quote failed: " + err);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "quotes");
    for (size_t i = 0; i < quotes.size(); i++) {
        if (!quotes[i].valid) continue;
        if (quotes[i].name.empty() && i < names.size()) quotes[i].name = names[i];
        AddQuote(arr, quotes[i]);
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(root);
        return Fail(is_error, "no valid quote (check symbols, e.g. sh600519/hk00700/usAAPL.OQ)");
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) return Fail(is_error, "OOM");
    return printed;  // cJSON 用 malloc，满足调用方 free 契约
}
