// stock_list_view.h
// 自选股列表视图（720x720 方屏适配）。纯渲染：拥有顶栏 + 行卡片，暴露 Apply* 供
// 宿主（stock_screen）在 worker 回调里调用。抓取节奏/轮询由宿主驱动。
//
// 方屏布局：一行同时显示 左=名称+代码、中=迷你分时 spark、右=价格+涨跌幅。

#ifndef STOCK_LIST_VIEW_H
#define STOCK_LIST_VIEW_H

#include "lvgl.h"
#include "stock_models.h"

#include <cstddef>

namespace stock_list_view {

struct Callbacks {
    void (*on_row_click)(size_t idx) = nullptr;
    void (*on_gear)() = nullptr;
    void (*on_back)() = nullptr;
};

// 在 parent 下构建列表视图（顶栏 + 滚动列表容器），返回根容器。
lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb);

// 依据 stock_store 重建行卡片（首次显示 / 配置变更后）。
void Rebuild();

// 更新单行报价（价格/涨跌幅/颜色，价格变化触发闪烁）。
void ApplyQuote(size_t idx, const StockQuote& q);

// 更新单行迷你分时（symbol 定位行）。
void ApplySpark(const char* symbol, const ChartSeries& chart);

// 更新顶栏 4 个市场状态点。
void ApplyMarketStat(const MarketStatus& stat);

// 中央横幅（如「连接网络中…」）；空串隐藏。
void SetBanner(const char* text);

// 视图卸载：清空 widget 引用（宿主在 SCREEN_UNLOADED 调）。
void Reset();

}  // namespace stock_list_view

#endif  // STOCK_LIST_VIEW_H
