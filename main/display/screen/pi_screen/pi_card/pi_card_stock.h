#pragma once

// ---------------------------------------------------------------------------
// pi_card_stock —— pi_card 的 stock_chart 叶子控件（AI 声明 symbol，数据设备侧直取）
//
// LLM 只下发 {type:'stock_chart', symbol, name?, mode?, w?, h?}——行情序列绝不
// 经过 LLM。控件自带头部（名称/代码/现价/涨跌幅）+ canvas 行情图 + 脚部（模式/
// 更新时间），由模块级 1s lv_timer 驱动：
//   - 排空 stock_fetch_worker 结果队列，按控件 session 匹配应用（不匹配即释放）；
//   - 按 StockFetchScheduler 自适应节奏入队报价/图表抓取（盘中密、闭市疏，
//     market_hours.h 本地推算盘中）；控件不可见（滚出视口/屏卸载）跳过；
//   - 点击图表循环切换 分时→五日→日K→周K（bump session 作废在途结果）。
// 生命周期：LV_EVENT_DELETE 出注册表 + 释放 canvas 缓冲；全部控件删光后 timer
// 暂停。主题切换经 pi_theme::AddListener 全量重绘。
// ---------------------------------------------------------------------------

#include <string>

#include "cJSON.h"
#include "lvgl.h"

namespace pi_card_stock {

// 校验（agent worker 线程，不碰 LVGL）：symbol 必填且为腾讯格式
// （sh/sz 6 位数字 | hk 5 位数字 | us TICKER[.N|.OQ]）；mode ∈ min|5d|day|week；
// 全屏同时存活 ≤3 个 stock_chart。失败写 err 返回 false。
bool ValidateNode(const cJSON* node, std::string& err);

// 渲染（LVGL 线程）：在 parent 下建整套控件树并登记进模块注册表。
// 返回控件根（column）；分配失败返回 nullptr（Validate 已过的 spec 不会走到）。
lv_obj_t* Create(lv_obj_t* parent, const cJSON* node);

}  // namespace pi_card_stock
