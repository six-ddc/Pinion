// media_http_fetch_esp — 有界 HTTP GET 的真机实现（esp_http_client 同步分块读）。
// 超时纪律同 media_http_esp.cc：只设 socket idle 超时 2.5s，块间轮询 abort，
// 中止响应上界一个超时。3xx 不自动跟随（CNR 网关不重定向；master→variant 的
// 跳转是 m3u8 文本层的事，与 HTTP 重定向无关）。
#include <atomic>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "media_http_fetch.h"

namespace media {
namespace {
const char* TAG = "media_fetch";
constexpr int kSocketTimeoutMs = 2500;
}  // namespace

bool HttpGetToString(const char* url, size_t max_bytes, const std::atomic<bool>& abort, int* status,
                     std::string* body) {
    if (status != nullptr) *status = 0;
    if (body != nullptr) body->clear();
    if (url == nullptr || url[0] == '\0' || body == nullptr) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kSocketTimeoutMs;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "User-Agent", "MetalioClaw/1.0");

    bool ok = false;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        if (esp_http_client_fetch_headers(client) < 0) break;
        if (status != nullptr) *status = esp_http_client_get_status_code(client);
        char buf[2048];
        for (;;) {
            if (abort.load()) break;
            int n = esp_http_client_read(client, buf, sizeof(buf));
            if (n > 0) {
                if (body->size() + (size_t)n > max_bytes) {
                    ESP_LOGW(TAG, "body exceeds %uB cap: %s", (unsigned)max_bytes, url);
                    break;
                }
                body->append(buf, (size_t)n);
                continue;
            }
            if (n == 0) ok = true;  // body 读完
            break;                  // n<0: socket 错误/idle 超时
        }
    } while (false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok && !abort.load();
}

}  // namespace media
