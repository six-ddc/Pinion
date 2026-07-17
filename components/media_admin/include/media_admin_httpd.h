// media_admin_httpd.h — SD 音乐后台 HTTP 服务的生命周期门面。
//
// 两个后端实现同一接口：
//   设备端 main/media_admin/media_admin_httpd.cc（esp_http_server）
//   sim  端 sim/shim/src/media_admin_httpd_sim.cc（POSIX socket 薄壳）
// UI（pi_settings 文件管理页）只依赖本头，双端通用。

#pragma once

#include <string>

namespace media_admin::httpd {

// 起 HTTP 服务（端口设备 80 / sim 8080）。已在跑 → true（幂等）。失败 → false。
bool Start();

// 停服务（幂等，未起时 no-op）。
void Stop();

bool IsRunning();

// 供 UI 显示的访问地址（如 "http://192.168.1.36" / "http://127.0.0.1:8080"）；
// 未运行返回 ""。
std::string GetUrl();

}  // namespace media_admin::httpd
