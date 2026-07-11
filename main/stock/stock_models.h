// stock_models.h
// 股票行情的数据模型。marketStat 只保留 open bool（画红/绿状态点），不解析 GBK
// 中文 label 以规避转码。零 LVGL 依赖，可被数据层与 UI 层共享。

#ifndef STOCK_MODELS_H
#define STOCK_MODELS_H

#include <cstddef>
#include <cstdint>
#include <string>

// 单股实时报价快照。name 不从腾讯 GBK 响应解析，由调用方（NVS 缓存）注入。
struct StockQuote {
    std::string symbol;
    std::string name;
    float current = 0;       // 现价
    float chg = 0;           // 涨跌额
    float percent = 0;       // 涨跌幅 %
    float open = 0;
    float high = 0;
    float low = 0;
    float last_close = 0;
    float amplitude = 0;     // 振幅 %
    float avg_price = 0;     // 均价（amount/volume）
    float turnover_rate = 0; // 换手率 %
    double volume = 0;       // 成交量（统一为"股"；A 股原始"手" ×100）
    double amount = 0;       // 成交额（货币原单位；A 股原始"万元" ×10000）
    uint32_t fetched_at = 0; // esp_timer 毫秒
    bool valid = false;
};

// 行情图模式：分时 / 5 日 / 日 K / 周 K。
enum ChartMode : uint8_t {
    CHART_MIN_1D  = 0,  // 当日分时
    CHART_MIN_5D  = 1,  // 5 日分时
    CHART_KLINE_D = 2,  // 日 K
    CHART_KLINE_W = 3,  // 周 K
    CHART_MODE_COUNT
};

// 行情图序列。points 为 close/current 价格；K 线额外填 opens/highs/lows；
// 分时模式这三者与 points 相同。kMaxPoints=400 足以装下 minute 1d 的 391 点。
struct ChartSeries {
    static constexpr size_t kMaxPoints = 400;
    std::string symbol;
    ChartMode mode = CHART_MIN_1D;
    float points[kMaxPoints];
    float opens[kMaxPoints];
    float highs[kMaxPoints];
    float lows[kMaxPoints];
    float volumes[kMaxPoints];
    float amounts[kMaxPoints];
    float turnover_rates[kMaxPoints];
    uint32_t timestamps_s[kMaxPoints];  // 秒级 epoch（北京时间近似）
    size_t count = 0;
    float last_close = 0;   // 分时模式有意义（昨收虚线）；K 线置 0
    bool has_ref = false;   // 是否画昨收虚线（仅分时 true）
    uint32_t fetched_at = 0;
    bool valid = false;
};

// 全市场交易状态（qt.gtimg.cn/q=marketStat，一行 ~200B）。
// server_bj_*: 响应首段的服务器北京时间，用作 beijing_clock 校时源。
// <market>_open: true=交易中；false=非交易中。中文 label 不解析（GBK）。
struct MarketStatus {
    bool sh_open = false;
    bool sz_open = false;
    bool hk_open = false;
    bool us_open = false;
    int server_bj_year = 0, server_bj_mon = 0, server_bj_day = 0;
    int server_bj_hour = 0, server_bj_min = 0, server_bj_sec = 0;
    uint32_t fetched_at = 0;
    bool valid = false;
};

#endif  // STOCK_MODELS_H
