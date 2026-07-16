#ifndef STOCK_TOOL_H
#define STOCK_TOOL_H

/* stock 工具 —— 腾讯财经行情查询（pi-c 工具桥，C 可包含，与 pi_card_tools.h 同契约）。
 *
 * execute 在 agent worker 线程调 pi_stock_tool_run（C++ 实现于 stock_tool.cc）做
 * **同步阻塞抓取**（每次 HTTP 6s 超时；模型本来就在等 tool 结果，TTS/ASR 在独立
 * 任务不受影响）。绝不碰 LVGL。返回串 malloc，调用方 free。 */

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

char *pi_stock_tool_run(const cJSON *args, bool *is_error);

#define PI_STOCK_TOOL_DESC                                                                          \
    "Live stock quotes: CN A-shares/HK/US equities, ETFs and indices (Tencent Finance; no "         \
    "crypto). Give query (Chinese name/pinyin/code, e.g. \"茅台\") to search and quote the top "     \
    "matches, OR symbols (e.g. [\"sh600519\",\"hk00700\",\"usAAPL.OQ\"]) to quote directly. "        \
    "Returns {quotes:[{sym,name,price,chg,pct,open,high,low,prev_close,vol,amount}]} — a "           \
    "snapshot at call time; vol in shares, amount in the listing currency. To SHOW a live chart "    \
    "card, follow with ui_render {root:{type:'stock_chart',symbol,name}}."

#define PI_STOCK_TOOL_SCHEMA                                                                        \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"stock "  \
    "name/pinyin/code to search\"},\"symbols\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}" \
    ",\"description\":\"known symbols, e.g. sh600519/hk00700/usAAPL.OQ\"}}}"

#ifdef __cplusplus
}
#endif
#endif /* STOCK_TOOL_H */
