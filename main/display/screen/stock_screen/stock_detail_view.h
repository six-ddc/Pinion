// stock_detail_view.h
// 个股详情视图（720x720）：顶栏名称/现价 + 四模式 tab（分时/五日/日K/周K）+ 画布图表
// + 触摸十字光标 hover + 底部状态条 + K 线缩放档位。宿主 stock_screen 在进入详情时
// Show()、离开时 Hide()，并把 worker 回调路由到 ApplyQuote/ApplyChart/ApplyMarketStat。

#ifndef STOCK_DETAIL_VIEW_H
#define STOCK_DETAIL_VIEW_H

#include "lvgl.h"
#include "stock_models.h"

namespace stock_detail_view {

struct Callbacks {
    void (*on_back)() = nullptr;  // 返回列表
};

// 构建视图（默认 HIDDEN）。返回根容器。
lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb);

// 进入/离开详情。
void Show();
void Hide();

// 从 stock_store 当前选中股载入（名称/代码 + 清图 + 触发首拉）。
void LoadCurrentStock();

// 切下一支自选股（点名称/代码触发）。
void SwitchNextStock();

// 250ms pacing（宿主在 s_view==Detail 时调）。
void Tick(uint32_t now_ms);

// 网络就绪态（宿主每 tick 传入）：未就绪时图表区显示「连接网络中…」。
void SetNetworkReady(bool ready);

// worker 回调路由（持 LVGL 锁）。
void ApplyQuote(const StockQuote& q);
void ApplyChart(const ChartSeries& chart, bool is_range);
void ApplyMarketStat(const MarketStatus& stat);

// 卸载清理（释放画布缓冲、清引用）。
void Reset();

}  // namespace stock_detail_view

#endif  // STOCK_DETAIL_VIEW_H
