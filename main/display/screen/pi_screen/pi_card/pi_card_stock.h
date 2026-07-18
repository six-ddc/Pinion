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
//   - 周期切换走脚部分段按钮（分时|五日|日K|周K，bump session 作废在途结果）；
//     图面按住/拖动显示十字线 + 该点数值气泡（时间/价格/涨跌幅），二者互不冲突。
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

// ---- Phase4：stock.<symbol>.<field> 动态数据绑定 ----
// 向 DataHub 注册 "stock." 动态路径 provider（pi_card::Init 调，幂等）。任意 label 可
// bind "stock.sh600519.price" 这类路径：首个绑定按 symbol 建无控件的报价订阅（复用本
// 模块 timer/worker/policy，盘中 5s），结果推回 DataHub subject 经 observer 自动刷新；
// 卡片删除释放路径 → 该 symbol 无绑定时退订。同时订阅 symbol 数有上限（超限路径显
// "超限"）。字段：price|chg|pct|open|high|low|last_close|avg_price|amplitude|turnover|
// volume|amount|pe|pb|float_cap|market_cap|time（推送侧已格式化，String 只读）。
void RegisterBindProvider();

// ui_render DESC 用的动态路径说明片段（常驻指针；字段清单与 provider 同源）。
const char* BindPathsDesc();

// 息屏门控（pi_card::SetScreenOff 分发，LVGL 线程）：off=true 停排一切新抓取（控件与
// bind 订阅；在途结果照常落地），off=false 立即补一轮调度。背光归零不影响
// lv_obj_is_visible，故必须由睡眠状态机显式告知，控件可见性判定拦不住息屏拉取。
void SetScreenOff(bool off);

}  // namespace pi_card_stock
