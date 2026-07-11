// book_config_server.h
// 书籍/字体管理 Web 后台（esp_http_server，端口 80）。进「管理」视图时 Start、离开时 Stop。
// 浏览器可列出 / 上传 / 删除 /sdcard/books 下的 .txt/.epub 书籍与 .ttf/.otf 字体（列表带 type
// 字段区分）。上传走原始字节流式写盘（见 .cc）。
// handler 直接读写 SD，改动置 dirty；回书架时由 LVGL 线程 ConsumeDirty 决定是否重扫。
// 参照 stock_config_server。

#ifndef BOOK_CONFIG_SERVER_H
#define BOOK_CONFIG_SERVER_H

namespace book_config_server {

// 启动 HTTP 服务（幂等）。返回是否处于运行态。
bool Start();
void Stop();
bool IsRunning();

// 取并清除 dirty（书目是否被上传/删除改动过）。
bool ConsumeDirty();

}  // namespace book_config_server

#endif  // BOOK_CONFIG_SERVER_H
