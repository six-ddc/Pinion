// stock_http.h — 股票数据层的最小 HTTP 抽象（一次性同步 GET）。
// 双端各一实现，接口/超时/伪装头保持一致：
//   真机: main/stock/stock_http_esp.cc      (esp_http_client + esp_crt_bundle)
//   sim : sim/shim/src/stock_http_curl.cc   (libcurl)
#pragma once

#include <cstddef>
#include <string>

namespace stock_http {

// 实现方共享的常量（腾讯财经需要浏览器化的 UA/Referer）。
inline constexpr int kTimeoutMs = 6000;
inline constexpr char kUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15) Gecko/20100101 Firefox/140.0";
inline constexpr char kReferer[] = "https://gu.qq.com/";

// 同步 GET：流式读进 buf（容量 cap，含 NUL 位），NUL 终止；*out_len 不含终止符。
// 非 200 / 超时 / 读错误返回 false 并填 out_err（短英文，直接可回给 LLM）。
bool Fetch(const char* url, char* buf, size_t cap, size_t* out_len, std::string& out_err);

}  // namespace stock_http
