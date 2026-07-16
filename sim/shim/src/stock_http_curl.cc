// sim shim — stock_http 的主机实现：libcurl 同步 GET（curl 由 pi_posix PUBLIC 传递）。
#include "stock_http.h"

#include <curl/curl.h>

#include <cstring>

namespace stock_http {
namespace {

struct Sink {
    char* buf;
    size_t cap;  // 含 NUL 位
    size_t len;
};

size_t WriteCb(char* data, size_t size, size_t nmemb, void* user) {
    Sink* s = static_cast<Sink*>(user);
    size_t n = size * nmemb;
    size_t room = (s->cap > s->len + 1) ? s->cap - 1 - s->len : 0;
    if (n > room) n = room;  // 缓冲满即截断（与真机同口径），仍返回全量表示"已消费"
    if (n > 0) {
        std::memcpy(s->buf + s->len, data, n);
        s->len += n;
    }
    return size * nmemb;
}

}  // namespace

bool Fetch(const char* url, char* buf, size_t cap, size_t* out_len, std::string& out_err) {
    *out_len = 0;
    if (buf == nullptr || cap < 2) {
        out_err = "bad buffer";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        out_err = "curl init failed";
        return false;
    }
    Sink sink{buf, cap, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_REFERER, kReferer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        out_err = std::string("curl: ") + curl_easy_strerror(rc);
        return false;
    }
    if (status != 200) {
        out_err = "HTTP " + std::to_string(status);
        return false;
    }
    buf[sink.len] = '\0';
    *out_len = sink.len;
    return true;
}

}  // namespace stock_http
