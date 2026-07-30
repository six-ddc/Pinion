// media_http_fetch_curl — 有界 HTTP GET 的主机实现（libcurl 同步）。语义与
// media_http_fetch_esp.cc 一致：不规范化 URL、abort 经 XferCb 中止 perform、
// 停滞 5s 判失败（对应真机 socket idle 超时）。
#include <curl/curl.h>

#include <atomic>
#include <string>

#include "media_http_fetch.h"

namespace media {
namespace {

struct FetchCtx {
    std::string* body = nullptr;
    size_t max_bytes = 0;
    const std::atomic<bool>* abort = nullptr;
    bool overflow = false;
};

size_t WriteCb(char* data, size_t sz, size_t nm, void* ud) {
    auto* ctx = static_cast<FetchCtx*>(ud);
    const size_t n = sz * nm;
    if (ctx->body->size() + n > ctx->max_bytes) {
        ctx->overflow = true;
        return 0;  // 中止传输
    }
    ctx->body->append(data, n);
    return n;
}

int XferCb(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<FetchCtx*>(ud);
    return ctx->abort->load() ? 1 : 0;  // 非零 = 中止 perform
}

}  // namespace

bool HttpGetToString(const char* url, size_t max_bytes, const std::atomic<bool>& abort, int* status,
                     std::string* body) {
    if (status != nullptr) *status = 0;
    if (body != nullptr) body->clear();
    if (url == nullptr || url[0] == '\0' || body == nullptr) return false;

    CURL* h = curl_easy_init();
    if (h == nullptr) return false;
    FetchCtx ctx{body, max_bytes, &abort, false};
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Pinion/1.0");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, XferCb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);  // <1B/s 持续 5s 视为停滞
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 5L);
    const CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    if (status != nullptr) *status = (int)code;
    curl_easy_cleanup(h);
    return rc == CURLE_OK && !ctx.overflow && !abort.load();
}

}  // namespace media
