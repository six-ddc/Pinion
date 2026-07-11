// stock_settings_view.h
// 自选股 Web 配置入口视图（720x720）：显示设备 IP + 启停配置服务器。
// 无 WiFi / 4G 时降级提示且不起服务。

#ifndef STOCK_SETTINGS_VIEW_H
#define STOCK_SETTINGS_VIEW_H

#include "lvgl.h"

namespace stock_settings_view {

struct Callbacks {
    void (*on_back)() = nullptr;
};

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb);

// 进入：取 IP、起 httpd（若有 WiFi）。离开：停 httpd。
void Show();
void Hide();

void Reset();

}  // namespace stock_settings_view

#endif  // STOCK_SETTINGS_VIEW_H
