// media_http_fetch — 有界 HTTP GET 原语（HLS 的 playlist/分片拉取用），分端实现：
//   真机: media_http_fetch_esp.cc   (esp_http_client，同 media_http_esp 的超时/证书包纪律)
//   sim : sim/shim/src/media_http_fetch_curl.cc (libcurl)
// 与 media_http_stream（无限 MP3 流）不同，这里的请求都是有界的：playlist 数 KB、
// 分片数百 KB，整体入内存（设备端大分配自动落 PSRAM），无需流式拉。
//
// 约定：
//   - url 原样使用，**不做任何规范化/重编码**——HLS 防盗链 token 挂在 query 上，
//     字节必须原封（satellitepull 实测红线）。
//   - abort 由调用方持有（跨线程置位）；实现分块读取并在块间轮询，中止响应上界
//     约一个 socket 超时（~2.5s）。
//   - 返回 true = 传输完成（含 4xx/5xx——调用方看 *status 决定）；false = 网络错误 /
//     被 abort / body 超过 max_bytes。
#pragma once

#include <atomic>
#include <cstddef>
#include <string>

namespace media {

bool HttpGetToString(const char* url, size_t max_bytes, const std::atomic<bool>& abort, int* status,
                     std::string* body);

}  // namespace media
