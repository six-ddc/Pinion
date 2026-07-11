// stock_config_server.h
// 自选股 Web 配置服务器（esp_http_server，端口 80）。进设置视图时 Start、离开时
// Stop。handler 只读写 NVS（stock_store），置 dirty；回列表时由 LVGL 线程 Reload。

#ifndef STOCK_CONFIG_SERVER_H
#define STOCK_CONFIG_SERVER_H

namespace stock_config_server {

// 启动 HTTP 服务（幂等）。返回是否处于运行态。
bool Start();
void Stop();
bool IsRunning();

// 取并清除 dirty（配置是否被改动过）。
bool ConsumeDirty();

}  // namespace stock_config_server

#endif  // STOCK_CONFIG_SERVER_H
