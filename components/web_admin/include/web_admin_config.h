// web_admin_config.h — 后台「配置」页的**可移植**核心（不含任何 httpd 类型）。
//
// 与 web_admin_fs 并列：设备端由 esp_http_server 薄壳（web_admin_httpd.cc）调用，
// sim 端由 POSIX socket 薄壳（sim/shim/src/web_admin_httpd_sim.cc）调用。
// 真正的存取与校验在 components/device_config，这里只做「HTTP 表单 ↔ 配置项」的
// 转换与状态出参。
//
// 重启不在这里：esp_restart 是平台相关的，由各端薄壳在 /api/config/apply 里做。

#pragma once

#include <cstddef>
#include <string>

namespace web_admin::config {

// POST /api/config 的 body 上限。llm_json(3500B) + radio_json(3960B) 可能同批提交，
// urlencoded 最坏膨胀约 3 倍（每字节 %XX），再留余量。
inline constexpr size_t kMaxFormBytes = 32768;

// GET /api/config 的 body：配置状态 + 密钥掩码，**不含明文密钥**。
std::string StatusJson();

// POST /api/config。body 为 application/x-www-form-urlencoded，字段全部可选：
//   llm_key / llm_base / llm_json / volc_app / volc_ak
// 缺字段 = 该项不动；字段存在且为空 = 清除该项。值首尾空白会被裁掉（粘贴常带
// 换行/空格）。任一字段校验不过（超长 / JSON 非法 / URL 协议不对）整个请求 400，
// 且**前面的字段已经写入**——网页因此逐项报错让用户改，而不是静默半成功。
// 返回 HTTP 状态码，msg 写 JSON body。
int Save(const std::string& form_body, std::string& msg);

}  // namespace web_admin::config
