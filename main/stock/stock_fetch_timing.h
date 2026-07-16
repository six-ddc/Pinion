// config/fetch_timing.h
// 跨模块共享的 HTTP fetch 节奏参数。Scene 私有 tunable（如 List 批量报价节奏、
// Usage 额度刷新间隔）应放在对应 scene 自己的 .cpp 顶部 constexpr，不要往这里塞。

#ifndef CONFIG_FETCH_TIMING_H
#define CONFIG_FETCH_TIMING_H

// 详情页实时报价拉取间隔（毫秒）。盘中走 1s；非盘中由 MarketSchedule 拉长到 idle 间隔。
#define FETCH_INTERVAL_MS     1000UL

// 首次或失败后的快速重试间隔，避免一直停在“等待数据”。
#define FETCH_RETRY_MS        1000UL

// 行情图拉取间隔（毫秒），按 chart mode 区分：
//   分时 / 5 日   —— 10s（盘中跟最新分钟/最近 5 日）
//   日 K / 周 K  —— 30s（K 线粒度大，刷新没必要太频）
#define CHART_FETCH_INTERVAL_MIN_MS    10000UL
#define CHART_FETCH_INTERVAL_KLINE_MS  30000UL
#define CHART_FETCH_RETRY_MS           10000UL

#endif  // CONFIG_FETCH_TIMING_H
