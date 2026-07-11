// stock_api.h
// 腾讯财经零认证行情接口的同步抓取。
// HTTP 走 Board::GetNetwork()->CreateHttp() 流式 Read；JSON 用 cJSON（5 日分时
// 因响应 ~200KB 改为字符串扫描，见 .cc）。所有函数同步阻塞，只能在后台 worker
// 线程调用，绝不可在 LVGL 线程调用。错误经 out_err 返回短英文。

#ifndef STOCK_API_H
#define STOCK_API_H

#include "stock_models.h"

#include <cstddef>
#include <string>

namespace stock_api {

// 一次 HTTP 批量拉取 N 支报价（qt.gtimg.cn/q=sym1,sym2,...）。返回 true 仅表示
// HTTP 成功且至少一支解析成功；每支成败看 outs[i].valid。n 上限 16。
bool FetchQuoteBatch(const std::string* syms, size_t n, StockQuote* outs,
                     std::string& out_err);

// 按 mode 拉取一次图表数据（分时/5日/日K/周K），整段替换 out。
bool FetchChart(const char* symbol, ChartMode mode, ChartSeries& out,
                std::string& out_err);

// 增量历史 K 线：拉 end_date（"YYYY-MM-DD"，空=最新）及之前最近 count 根。
// mode 必须是 CHART_KLINE_D / CHART_KLINE_W。
bool FetchKlineRange(const char* symbol, ChartMode mode, const char* end_date,
                     int count, ChartSeries& out, std::string& out_err);

// 全市场 open/close 状态（qt.gtimg.cn/q=marketStat）。顺带解析服务器北京时间。
bool FetchMarketStat(MarketStatus& out, std::string& out_err);

// 代理 smartbox 搜索，out_json 填 `[{"symbol":"..","name":".."},...]`。
bool SearchStocks(const char* utf8_query, std::string& out_json,
                  std::string& out_err);

// 展示用代码（剥市场前缀 + 美股后缀）。
std::string DisplayCode(const char* symbol);

// 释放内部 256KB PSRAM 工作缓冲（worker 关闭时调用，可选）。
void ReleaseBuffer();

}  // namespace stock_api

#endif  // STOCK_API_H
