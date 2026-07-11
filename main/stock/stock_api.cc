// stock_api.cc
// 腾讯财经接口实现。零认证、零 gzip。
// 关键实现点：
//   - HTTP：Board::GetNetwork()->CreateHttp() + 流式 Read 循环（禁 ReadAll，规避
//     HttpClient 内部 8KB 背压死锁；5 日分时 ~200KB）。
//   - JSON：用 cJSON；5 日分时改字符串扫描（避免 200KB 全量建树）。
//   - 工作缓冲：256KB 一块 PSRAM，单 in-flight worker 独占复用。

#include "stock_api.h"

#include "kline_parse.h"
#include "minute_parse.h"
#include "quote_parse.h"
#include "smartbox_parse.h"
#include "tencent_endpoints.h"

#include "board.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace stock_api {
namespace {

constexpr char TAG[] = "stock_api";

constexpr int kHttpTimeoutMs = 6000;
constexpr size_t kMaxBodyBytes = 256 * 1024;  // 5 日分时 ~200KB
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15) Gecko/20100101 Firefox/140.0";

using quote_parse::Market;
using quote_parse::marketOf;
using quote_parse::isAShare;

char* g_buf = nullptr;  // 256KB PSRAM 工作缓冲，惰性分配
// g_buf 被后台 worker 与配置服务器 search handler 两个线程共用，需互斥。
SemaphoreHandle_t g_mutex = nullptr;

// RAII 锁。首次调用发生在 worker 线程（早于配置服务器启动），惰性建锁无实际竞争。
struct ApiLock {
    ApiLock() {
        if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
        if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    }
    ~ApiLock() {
        if (g_mutex) xSemaphoreGive(g_mutex);
    }
};

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

char* EnsureBuffer() {
    if (g_buf == nullptr) {
        g_buf = static_cast<char*>(
            heap_caps_aligned_alloc(64, kMaxBodyBytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_buf == nullptr) {
            // PSRAM 不足时退内部 RAM（小响应仍可用；大 5 日分时可能失败）
            g_buf = static_cast<char*>(heap_caps_malloc(kMaxBodyBytes, MALLOC_CAP_8BIT));
        }
    }
    return g_buf;
}

// 同步 GET，流式 Read 到 g_buf 并 NUL 终止。out_len 不含终止符。
bool FetchBody(const std::string& url, size_t* out_len, std::string& out_err) {
    *out_len = 0;
    char* buf = EnsureBuffer();
    if (buf == nullptr) { out_err = "OOM"; return false; }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) { out_err = "Network not available"; return false; }
    auto http = network->CreateHttp(0);
    if (http == nullptr) { out_err = "HTTP create failed"; return false; }

    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("User-Agent", kUserAgent);
    http->SetHeader("Referer", "https://gu.qq.com/");
    http->SetHeader("Connection", "close");

    if (!http->Open("GET", url)) { out_err = "HTTP open failed"; return false; }
    const int status = http->GetStatusCode();
    if (status != 200) {
        out_err = "HTTP " + std::to_string(status);
        http->Close();
        return false;
    }

    size_t total = 0;
    while (total + 1 < kMaxBodyBytes) {
        const int n = http->Read(buf + total, kMaxBodyBytes - 1 - total);
        if (n < 0) { out_err = "HTTP read error"; http->Close(); return false; }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    http->Close();
    buf[total] = '\0';
    *out_len = total;
    return true;
}

// 报价 URL + cache buster。
std::string QuoteUrlForQuery(const std::string& q) {
    return tencent_endpoints::quoteUrl(q.c_str()) + "&_=" + std::to_string(NowMs());
}

// cJSON 取数（字段可能是 number 或 string）。
double JNum(const cJSON* it) {
    if (it == nullptr) return 0;
    if (cJSON_IsNumber(it)) return it->valuedouble;
    if (cJSON_IsString(it) && it->valuestring) return std::strtod(it->valuestring, nullptr);
    return 0;
}
const char* JStr(const cJSON* it) {
    if (it && cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return "";
}

// 把 ChartSeries 的 7 个 float 列暴露给 minute_parse 的列指针接口。
void ChartSeriesAsArrays(ChartSeries& s, float* (&arr)[7]) {
    arr[0] = s.points;
    arr[1] = s.opens;
    arr[2] = s.highs;
    arr[3] = s.lows;
    arr[4] = s.volumes;
    arr[5] = s.amounts;
    arr[6] = s.turnover_rates;
}

void Downsample(ChartSeries& s, size_t target_max) {
    if (s.count <= target_max) return;
    float* arrays[7];
    ChartSeriesAsArrays(s, arrays);
    minute_parse::downsampleParallelArrays(arrays, 7, s.timestamps_s, &s.count,
                                           ChartSeries::kMaxPoints, target_max);
}

// ---- 分时（当日）：cJSON ----------------------------------------------------
bool FetchMinuteToday(const char* symbol, ChartSeries& out, std::string& out_err) {
    Market m = marketOf(symbol);
    std::string url = tencent_endpoints::minuteUrl(symbol, m);
    ESP_LOGI(TAG, "GET %s", url.c_str());
    size_t len = 0;
    if (!FetchBody(url, &len, out_err)) return false;

    cJSON* root = cJSON_Parse(g_buf);
    if (root == nullptr) { out_err = "minute JSON error"; return false; }
    bool ok = false;
    do {
        cJSON* code = cJSON_GetObjectItem(root, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 0) { out_err = "minute code!=0"; break; }
        cJSON* data = cJSON_GetObjectItem(root, "data");
        cJSON* symObj = data ? cJSON_GetObjectItem(data, symbol) : nullptr;
        cJSON* d = symObj ? cJSON_GetObjectItem(symObj, "data") : nullptr;
        cJSON* rows = d ? cJSON_GetObjectItem(d, "data") : nullptr;
        const char* date = d ? JStr(cJSON_GetObjectItem(d, "date")) : "";
        if (!cJSON_IsArray(rows) || std::strlen(date) < 8) { out_err = "minute no rows"; break; }

        float last_close = 0;
        cJSON* qt = symObj ? cJSON_GetObjectItem(symObj, "qt") : nullptr;
        cJSON* qtSym = qt ? cJSON_GetObjectItem(qt, symbol) : nullptr;
        if (cJSON_IsArray(qtSym) && cJSON_GetArraySize(qtSym) > 4) {
            last_close = static_cast<float>(JNum(cJSON_GetArrayItem(qtSym, 4)));
        }

        size_t n = 0;
        cJSON* rv = nullptr;
        cJSON_ArrayForEach(rv, rows) {
            if (n >= ChartSeries::kMaxPoints) break;
            const char* line = JStr(rv);
            char hhmm[5];
            float price;
            if (!minute_parse::parseMinuteLine(line, hhmm, &price)) continue;
            out.points[n] = price;
            out.opens[n] = price;
            out.highs[n] = price;
            out.lows[n] = price;
            out.volumes[n] = 0;
            out.amounts[n] = 0;
            out.turnover_rates[n] = 0;
            out.timestamps_s[n] = minute_parse::hhmmToEpoch(date, hhmm);
            n++;
        }
        if (n == 0) { out_err = "minute empty"; break; }
        out.symbol = symbol;
        out.count = n;
        out.last_close = last_close;
        out.has_ref = (last_close > 0);
        out.fetched_at = NowMs();
        out.valid = true;
        ESP_LOGI(TAG, "minute parsed %u pts, last_close=%.2f", (unsigned)n, last_close);
        ok = true;
    } while (false);
    cJSON_Delete(root);
    return ok;
}

// ---- 5 日分时：字符串扫描（避免 200KB 全量 cJSON 建树）----------------------
// 结构：data.<sym>.data = [ {date, data:[ "HHMM p ...", ...], prec}, ... ]（今天在前）。
// 逐个 "date":" 定位每天，扫其 "data":[ 的字符串行，取 "prec":"。
// 收集顺序今天在前，最后整段 reverse 成"最旧→最新"，再降采样到 200。
bool Fetch5DayMinute(const char* symbol, ChartSeries& out, std::string& out_err) {
    Market m = marketOf(symbol);
    std::string url = tencent_endpoints::fiveDayUrl(symbol, m);
    ESP_LOGI(TAG, "GET %s", url.c_str());
    size_t len = 0;
    if (!FetchBody(url, &len, out_err)) return false;

    size_t n = 0;
    float last_close = 0;
    const char* cursor = g_buf;
    while (n < ChartSeries::kMaxPoints) {
        const char* dpos = std::strstr(cursor, "\"date\":\"");
        if (dpos == nullptr) break;
        dpos += 8;
        char date[9];
        int k = 0;
        while (k < 8 && dpos[k] && dpos[k] != '"') { date[k] = dpos[k]; k++; }
        date[k] = '\0';

        const char* darr = std::strstr(dpos, "\"data\":[");
        if (darr == nullptr) break;
        const char* q = darr + 8;
        while (n < ChartSeries::kMaxPoints) {
            while (*q == ' ' || *q == ',' || *q == '\n' || *q == '\r' || *q == '\t') q++;
            if (*q == ']' || *q == '\0') break;
            if (*q != '"') break;
            const char* line_start = q + 1;
            const char* line_end = std::strchr(line_start, '"');
            if (line_end == nullptr) { q = line_start; break; }
            char line[48];
            size_t llen = static_cast<size_t>(line_end - line_start);
            if (llen >= sizeof(line)) llen = sizeof(line) - 1;
            std::memcpy(line, line_start, llen);
            line[llen] = '\0';
            char hhmm[5];
            float price;
            if (minute_parse::parseMinuteLine(line, hhmm, &price)) {
                out.points[n] = price;
                out.opens[n] = price;
                out.highs[n] = price;
                out.lows[n] = price;
                out.volumes[n] = 0;
                out.amounts[n] = 0;
                out.turnover_rates[n] = 0;
                out.timestamps_s[n] = minute_parse::hhmmToEpoch(date, hhmm);
                n++;
            }
            q = line_end + 1;
        }
        // 本天 prec（在 data 数组之后）。持续更新 → 终值为最旧那天的非空 prec。
        const char* prec_pos = std::strstr(q, "\"prec\":\"");
        if (prec_pos != nullptr) {
            float pv = std::strtof(prec_pos + 8, nullptr);
            if (pv > 0) last_close = pv;
        }
        cursor = q;
    }
    if (n == 0) { out_err = "5d empty"; return false; }

    out.count = n;
    float* arrays[7];
    ChartSeriesAsArrays(out, arrays);
    minute_parse::reverseParallelArrays(arrays, 7, out.timestamps_s, n);

    if (!(last_close > 0)) last_close = out.points[0];
    out.symbol = symbol;
    out.count = n;
    out.last_close = last_close;
    out.has_ref = (last_close > 0);
    out.fetched_at = NowMs();
    out.valid = true;
    Downsample(out, 200);
    ESP_LOGI(TAG, "5d parsed %u pts, last_close=%.2f", (unsigned)out.count, last_close);
    return true;
}

// ---- 日 K / 周 K：cJSON ------------------------------------------------------
bool FetchKlineImpl(const char* symbol, ChartMode mode, const char* end_date,
                    int requested_count, ChartSeries& out, std::string& out_err) {
    Market m = marketOf(symbol);
    const char* period = (mode == CHART_KLINE_W) ? "week" : "day";
    int count = (requested_count > 0) ? requested_count
                                      : ((mode == CHART_KLINE_W) ? 26 : 22);
    const char* end_param = (end_date && *end_date) ? end_date : "";

    std::string url = tencent_endpoints::klineUrl(symbol, m, period, end_param, count);
    ESP_LOGI(TAG, "GET %s", url.c_str());
    size_t len = 0;
    if (!FetchBody(url, &len, out_err)) return false;

    const char* keys[4];
    kline_parse::fillCandidateKeys(period, keys);

    cJSON* root = cJSON_Parse(g_buf);
    if (root == nullptr) { out_err = "kline JSON error"; return false; }
    bool ok = false;
    do {
        cJSON* code = cJSON_GetObjectItem(root, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 0) { out_err = "kline code!=0"; break; }
        cJSON* data = cJSON_GetObjectItem(root, "data");
        cJSON* symObj = data ? cJSON_GetObjectItem(data, symbol) : nullptr;
        if (symObj == nullptr) { out_err = "kline no sym"; break; }

        cJSON* rows = nullptr;
        for (int i = 0; keys[i]; i++) {
            cJSON* a = cJSON_GetObjectItem(symObj, keys[i]);
            if (cJSON_IsArray(a) && cJSON_GetArraySize(a) > 0) { rows = a; break; }
        }
        if (rows == nullptr) { out_err = "kline no rows"; break; }

        size_t n = 0;
        bool a_share = isAShare(m);
        cJSON* row = nullptr;
        cJSON_ArrayForEach(row, rows) {
            if (n >= ChartSeries::kMaxPoints) break;
            if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) < 6) continue;
            int sz = cJSON_GetArraySize(row);
            const char* date = JStr(cJSON_GetArrayItem(row, 0));
            float open = static_cast<float>(JNum(cJSON_GetArrayItem(row, 1)));
            float close = static_cast<float>(JNum(cJSON_GetArrayItem(row, 2)));
            float high = static_cast<float>(JNum(cJSON_GetArrayItem(row, 3)));
            float low = static_cast<float>(JNum(cJSON_GetArrayItem(row, 4)));
            double raw_vol = JNum(cJSON_GetArrayItem(row, 5));
            float turnover = (sz > 7) ? static_cast<float>(JNum(cJSON_GetArrayItem(row, 7))) : 0;
            double raw_amt = (sz > 8) ? JNum(cJSON_GetArrayItem(row, 8)) : 0;
            auto r = kline_parse::makeRow(date, open, close, high, low, raw_vol, raw_amt,
                                          turnover, a_share, /*tenThousand*/ true);
            if (!r.valid) continue;
            out.points[n] = r.close;
            out.opens[n] = r.open;
            out.highs[n] = r.high;
            out.lows[n] = r.low;
            out.volumes[n] = r.volume;
            out.amounts[n] = r.amount;
            out.turnover_rates[n] = r.turnover_rate;
            out.timestamps_s[n] = r.timestamp_s;
            n++;
        }
        if (n == 0) { out_err = "kline empty"; break; }
        out.symbol = symbol;
        out.count = n;
        out.last_close = 0;
        out.has_ref = false;
        out.fetched_at = NowMs();
        out.valid = true;
        ESP_LOGI(TAG, "kline(%s) parsed %u pts", period, (unsigned)n);
        ok = true;
    } while (false);
    cJSON_Delete(root);
    return ok;
}

void AssignMarketByCode(MarketStatus& s, const char* code, bool is_open) {
    if (std::strcmp(code, "SH") == 0) s.sh_open = is_open;
    else if (std::strcmp(code, "SZ") == 0) s.sz_open = is_open;
    else if (std::strcmp(code, "HK") == 0) s.hk_open = is_open;
    else if (std::strcmp(code, "US") == 0) s.us_open = is_open;
}

}  // namespace

// ---- 批量报价（GBK 安全锚点，无 JSON）---------------------------------------
bool FetchQuoteBatch(const std::string* syms, size_t n, StockQuote* outs,
                     std::string& out_err) {
    ApiLock lk;
    out_err.clear();
    if (syms == nullptr || outs == nullptr || n == 0) { out_err = "Invalid params"; return false; }

    std::string q;
    q.reserve(n * 16);
    for (size_t i = 0; i < n; i++) {
        outs[i] = StockQuote{};
        const char* s = syms[i].c_str();
        if (s[0] == '\0' || syms[i].size() > 20) continue;
        if (marketOf(s) == Market::UNKNOWN) continue;
        if (!q.empty()) q += ",";
        q += tencent_endpoints::quoteSymbolOf(s);
    }
    if (q.empty()) { out_err = "Invalid symbol"; return false; }

    std::string url = QuoteUrlForQuery(q);
    ESP_LOGI(TAG, "GET %s", url.c_str());
    size_t len = 0;
    if (!FetchBody(url, &len, out_err)) return false;

    int body_len = static_cast<int>(len);
    int valid_count = 0;
    for (size_t i = 0; i < n; i++) {
        const char* sym = syms[i].c_str();
        if (sym[0] == '\0') continue;
        std::string req_sym = tencent_endpoints::quoteSymbolOf(sym);
        int bs = 0, be = 0;
        if (!quote_parse::findQuoteBlock(g_buf, 0, body_len, req_sym.c_str(), bs, be)) continue;
        quote_parse::QuoteFields f;
        if (!quote_parse::parseQuote(g_buf, bs, be, sym, f)) continue;
        outs[i].symbol = sym;
        outs[i].current = f.current;
        outs[i].last_close = f.last_close;
        outs[i].open = f.open;
        outs[i].chg = f.chg;
        outs[i].percent = f.percent;
        outs[i].high = f.high;
        outs[i].low = f.low;
        outs[i].amplitude = f.amplitude;
        outs[i].avg_price = f.avg_price;
        outs[i].turnover_rate = f.turnover_rate;
        outs[i].volume = f.volume;
        outs[i].amount = f.amount;
        outs[i].fetched_at = NowMs();
        outs[i].valid = f.valid;
        if (f.valid) valid_count++;
    }
    if (valid_count == 0) { out_err = "No quotes parsed"; return false; }
    return true;
}

bool FetchChart(const char* symbol, ChartMode mode, ChartSeries& out,
                std::string& out_err) {
    ApiLock lk;
    out_err.clear();
    if (symbol == nullptr || symbol[0] == '\0' || std::strlen(symbol) > 20) {
        out_err = "Invalid symbol";
        return false;
    }
    if (marketOf(symbol) == Market::UNKNOWN) { out_err = "Unknown market"; return false; }

    out.symbol = symbol;
    out.mode = mode;
    out.count = 0;
    out.has_ref = false;
    out.last_close = 0;
    out.valid = false;

    bool ok = false;
    switch (mode) {
        case CHART_MIN_1D:  ok = FetchMinuteToday(symbol, out, out_err); break;
        case CHART_MIN_5D:  ok = Fetch5DayMinute(symbol, out, out_err); break;
        case CHART_KLINE_D: ok = FetchKlineImpl(symbol, mode, "", 0, out, out_err); break;
        case CHART_KLINE_W: ok = FetchKlineImpl(symbol, mode, "", 0, out, out_err); break;
        default: out_err = "Invalid chart mode"; return false;
    }
    if (!ok && out_err.empty()) out_err = "K-line parse failed";
    return ok;
}

bool FetchKlineRange(const char* symbol, ChartMode mode, const char* end_date,
                     int count, ChartSeries& out, std::string& out_err) {
    ApiLock lk;
    out_err.clear();
    if (symbol == nullptr || symbol[0] == '\0' || std::strlen(symbol) > 20) {
        out_err = "Invalid symbol";
        return false;
    }
    if (marketOf(symbol) == Market::UNKNOWN) { out_err = "Unknown market"; return false; }
    if (mode != CHART_KLINE_D && mode != CHART_KLINE_W) { out_err = "Invalid mode"; return false; }
    if (count <= 0) { out_err = "Invalid count"; return false; }

    out.symbol = symbol;
    out.mode = mode;
    out.count = 0;
    out.has_ref = false;
    out.last_close = 0;
    out.valid = false;

    bool ok = FetchKlineImpl(symbol, mode, end_date, count, out, out_err);
    if (!ok && out_err.empty()) out_err = "K-line parse failed";
    return ok;
}

// ---- 全市场状态：v_marketStat="BJ时间|SH_close_..|US_open_..|..." -----------
bool FetchMarketStat(MarketStatus& out, std::string& out_err) {
    ApiLock lk;
    out_err.clear();
    out = MarketStatus{};

    std::string url = tencent_endpoints::marketStatUrl();
    ESP_LOGI(TAG, "GET %s", url.c_str());
    size_t len = 0;
    if (!FetchBody(url, &len, out_err)) return false;

    // 剥外层 v_marketStat="...";
    const char* first = std::strchr(g_buf, '"');
    if (first == nullptr) { out_err = "marketStat malformed"; return false; }
    const char* last = std::strrchr(g_buf, '"');
    if (last == nullptr || last <= first) { out_err = "marketStat malformed"; return false; }

    std::string inner(first + 1, last - first - 1);
    size_t seg_start = 0;
    bool first_seg = true;
    while (seg_start < inner.size()) {
        size_t seg_end = inner.find('|', seg_start);
        if (seg_end == std::string::npos) seg_end = inner.size();
        std::string seg = inner.substr(seg_start, seg_end - seg_start);
        seg_start = seg_end + 1;

        if (first_seg) {
            first_seg = false;
            if (seg.size() >= 19) {  // "2026-05-11 21:36:25"
                out.server_bj_year = std::atoi(seg.substr(0, 4).c_str());
                out.server_bj_mon = std::atoi(seg.substr(5, 2).c_str());
                out.server_bj_day = std::atoi(seg.substr(8, 2).c_str());
                out.server_bj_hour = std::atoi(seg.substr(11, 2).c_str());
                out.server_bj_min = std::atoi(seg.substr(14, 2).c_str());
                out.server_bj_sec = std::atoi(seg.substr(17, 2).c_str());
            }
            continue;
        }
        // "HK_close_已收盘" — 取前两个 '_' 之间为 state；中文 label 忽略（GBK）。
        size_t u1 = seg.find('_');
        if (u1 == std::string::npos || u1 == 0) continue;
        size_t u2 = seg.find('_', u1 + 1);
        if (u2 == std::string::npos || u2 <= u1 + 1) continue;
        std::string code = seg.substr(0, u1);
        std::string state = seg.substr(u1 + 1, u2 - u1 - 1);
        AssignMarketByCode(out, code.c_str(), state == "open");
    }

    out.fetched_at = NowMs();
    out.valid = (out.server_bj_year > 2000);
    if (!out.valid) out_err = "marketStat parse failed";
    return out.valid;
}

bool SearchStocks(const char* utf8_query, std::string& out_json, std::string& out_err) {
    ApiLock lk;
    out_err.clear();
    if (utf8_query == nullptr || utf8_query[0] == '\0') { out_json = "[]"; return true; }

    std::string url = tencent_endpoints::searchUrl(utf8_query);
    ESP_LOGI(TAG, "GET %s", url.c_str());
    size_t len = 0;
    if (!FetchBody(url, &len, out_err)) { out_json = "[]"; return false; }

    auto records = smartbox_parse::parseBody(g_buf, len);
    out_json = "[";
    for (size_t i = 0; i < records.size(); i++) {
        if (i > 0) out_json += ",";
        out_json += "{\"symbol\":\"";
        out_json += records[i].symbol;
        out_json += "\",\"name\":\"";
        out_json += records[i].name;  // 已是 \uXXXX 转义 ASCII，合法 JSON
        out_json += "\"}";
    }
    out_json += "]";
    return true;
}

std::string DisplayCode(const char* symbol) {
    return tencent_endpoints::stockDisplayCode(symbol);
}

void ReleaseBuffer() {
    if (g_buf != nullptr) {
        heap_caps_free(g_buf);
        g_buf = nullptr;
    }
}

}  // namespace stock_api
