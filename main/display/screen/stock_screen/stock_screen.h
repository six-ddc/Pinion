// stock_screen.h
// 股票行情 app（720x720）。单 screen 宿主三个视图（列表 / 详情图表 / Web 设置），
// 驱动后台 worker 抓取节奏并路由回调。

#ifndef STOCK_SCREEN_H
#define STOCK_SCREEN_H

#include "lvgl.h"
#include "screen_util.h"

class StockScreen {
public:
    // 构建并返回 720x720 screen（parent=NULL）。
    static lv_obj_t* Create();

    // 生命周期观察（LOAD/UNLOAD），由 home_screen 注入。
    static void LifecycleCallback(screen_lifecycle_event_t event);
};

#endif  // STOCK_SCREEN_H
