// manage_view.h
// 书籍管理入口页（on-device）：显示 Web 后台地址 http://<ip>，进页 Start / 离开 Stop
// book_config_server。手机连同一 WiFi 浏览器打开即可上传/删除 TXT。镜像 stock_settings_view。

#ifndef MANAGE_VIEW_H
#define MANAGE_VIEW_H

#include "lvgl.h"

namespace manage_view {

struct Callbacks {
    void (*on_back)() = nullptr;
};

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb, uint8_t theme_idx);

// 呼出：启动服务并显示地址（无 WiFi 显示提示，不启动）。book_count 显示当前书目数。
void Show(int book_count);
// 隐藏：停止服务。
void Hide();

void SetThemeIdx(uint8_t idx);
void Reset();

}  // namespace manage_view

#endif  // MANAGE_VIEW_H
