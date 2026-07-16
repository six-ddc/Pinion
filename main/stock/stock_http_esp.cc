// stock_http_esp.cc — stock_http 的真机实现：esp_http_client 流式 GET。
// 序列 open → fetch_headers → read 循环，与 pi-c transport_esp_http.c 同范式；
// 报价接口是明文 http://（qt.gtimg.cn），crt_bundle 仅对 https 生效。
#include "stock_http.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

namespace stock_http {

bool Fetch(const char* url, char* buf, size_t cap, size_t* out_len, std::string& out_err) {
    *out_len = 0;
    if (buf == nullptr || cap < 2) {
        out_err = "bad buffer";
        return false;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kTimeoutMs;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        out_err = "HTTP init failed";
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", kUserAgent);
    esp_http_client_set_header(client, "Referer", kReferer);
    esp_http_client_set_header(client, "Connection", "close");

    bool ok = false;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        out_err = "HTTP open failed";
    } else if (esp_http_client_fetch_headers(client) < 0) {
        out_err = "HTTP headers failed";
    } else {
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            out_err = "HTTP " + std::to_string(status);
        } else {
            size_t total = 0;
            for (;;) {
                if (total + 1 >= cap) break;  // 缓冲满即止（与 Claw4 同口径，截断不算错）
                const int n = esp_http_client_read(client, buf + total, (int)(cap - 1 - total));
                if (n == 0) break;
                if (n < 0) {
                    out_err = "HTTP read error";
                    esp_http_client_cleanup(client);
                    return false;
                }
                total += (size_t)n;
            }
            buf[total] = '\0';
            *out_len = total;
            ok = true;
        }
    }
    esp_http_client_cleanup(client);
    return ok;
}

}  // namespace stock_http
