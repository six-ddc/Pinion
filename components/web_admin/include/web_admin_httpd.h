// web_admin_httpd.h — 设备 Web 后台 HTTP 服务的生命周期门面。
//
// 一个服务两页：「配置」（大模型 / 语音密钥，写 NVS）与「文件」（SD 卡音乐管理）。
// 两个后端实现同一接口：
//   设备端 components/web_admin/web_admin_httpd.cc（esp_http_server，:80）
//   sim  端 sim/shim/src/web_admin_httpd_sim.cc（POSIX socket 薄壳，:8080）
// UI（pi_settings 后台页、pi_screen 未配置引导页）只依赖本头，双端通用。

#pragma once

#include <string>

namespace web_admin::httpd {

// 起 HTTP 服务（端口设备 80 / sim 8080）。已在跑 → true（幂等）。失败 → false。
bool Start();

// 停服务（幂等，未起时 no-op）。
void Stop();

bool IsRunning();

// 供 UI 显示的访问地址（如 "http://192.168.1.36" / "http://127.0.0.1:8080"）；
// 未运行返回 ""。
std::string GetUrl();

}  // namespace web_admin::httpd
