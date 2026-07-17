// media_admin_httpd.cc — 设备端 esp_http_server 薄壳。
// 只负责：httpd 生命周期、URI 路由、请求参数抽取、body 流式 recv → 交给
// media_admin_core（可移植逻辑）。10 分钟无请求自动停服，省电防长期暴露。

#include "media_admin_httpd.h"

#include <atomic>
#include <cstring>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "media_admin_core.h"
#include "media_admin_web.h"
#include "metalio_hal/network.h"

namespace media_admin::httpd {
namespace {

constexpr char TAG[] = "media_admin";
constexpr int64_t kIdleStopUs = 10ll * 60 * 1000 * 1000;  // 10 分钟无请求自动停
constexpr int64_t kIdleCheckUs = 60ll * 1000 * 1000;      // 每分钟检查一次

httpd_handle_t s_server = nullptr;
esp_timer_handle_t s_idle_timer = nullptr;
std::atomic<int64_t> s_last_activity{0};

void Touch() { s_last_activity.store(esp_timer_get_time()); }

const char* StatusStr(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        case 507: return "507 Insufficient Storage";
        default: return "200 OK";
    }
}

esp_err_t SendJson(httpd_req_t* req, int code, const std::string& body) {
    httpd_resp_set_status(req, StatusStr(code));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body.c_str(), body.size());
}

// 读整段 query string。
std::string QueryStr(httpd_req_t* req) {
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1 || qlen > 2048) return "";
    std::string q(qlen, '\0');
    if (httpd_req_get_url_query_str(req, &q[0], qlen) != ESP_OK) return "";
    q.resize(std::strlen(q.c_str()));
    return q;
}

bool QueryVal(const std::string& q, const char* key, std::string& out) {
    char val[512];
    if (httpd_query_key_value(q.c_str(), key, val, sizeof(val)) != ESP_OK) return false;
    out = media_admin::UrlDecode(val);
    return true;
}

// 读 form body（路径类，2KB 足够）。
std::string ReadFormBody(httpd_req_t* req) {
    std::string body;
    size_t remaining = req->content_len;
    if (remaining > 2048) remaining = 2048;
    char buf[512];
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        body.append(buf, r);
        remaining -= r;
    }
    return body;
}

// ---- handlers ----
esp_err_t RootGet(httpd_req_t* req) {
    Touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, media_admin_web::Html());
}

esp_err_t ListGet(httpd_req_t* req) {
    Touch();
    std::string dir;
    QueryVal(QueryStr(req), "dir", dir);
    int status = 200;
    std::string json = media_admin::ListJson(dir, status);
    return SendJson(req, status, json);
}

esp_err_t SpaceGet(httpd_req_t* req) {
    Touch();
    return SendJson(req, 200, media_admin::SpaceJson());
}

esp_err_t UploadPost(httpd_req_t* req) {
    Touch();
    std::string q = QueryStr(req);
    std::string path, over;
    QueryVal(q, "path", path);
    QueryVal(q, "overwrite", over);
    bool overwrite = (over == "1");

    if (!media_admin::TryBeginUpload())
        return SendJson(req, 429, "{\"error\":\"another upload in progress\"}");

    // 流式 reader：从 httpd 拉取 body，最多 content_len 字节。
    //
    // 控制流：httpd_req_recv 在 recv_wait_timeout（见 Start() 里的 30s）内没等到
    // 数据会返回 HTTPD_SOCK_ERR_TIMEOUT（负值）——这只代表"这次没读到"，不是连接
    // 已断，弱 WiFi 一次瞬时 stall 很常见。之前的 bug：直接把超时重试那次 recv 的
    // 返回值当结果 return，既跳过了成功路径的 `remaining -= r` 记账，又会把连续
    // 两次超时的第二个负 sentinel 当"读到的字节数"吐给上层，导致已收全的上传被
    // 误判失败、.part 被删。
    //
    // 修复：整个 reader 是一个有限重试的 while 循环——超时 continue 重新 recv
    // （不 return），最多连续重试 kMaxTimeoutRetries 次；成功路径(r>0)统一在循环
    // 外扣减 remaining 并 return；r<=0 且非超时（客户端断开 / 其他错误）立即 -1。
    // 重试次数耗尽仍超时，视为连接确已死，返回 -1（Upload() 会 unlink .part）。
    uint64_t remaining = req->content_len;
    auto reader = [req, &remaining](char* buf, size_t max) -> int {
        if (remaining == 0) return 0;  // EOF
        size_t want = remaining < max ? static_cast<size_t>(remaining) : max;
        constexpr int kMaxTimeoutRetries = 5;  // 5 次 * 30s recv_wait_timeout ≈ 2.5min 弱网容忍
        for (int tries = 0; tries <= kMaxTimeoutRetries; tries++) {
            int r = httpd_req_recv(req, buf, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;  // 瞬时 stall，重试而非直接判失败
            if (r <= 0) return -1;  // 断开 / 其他错误
            remaining -= r;
            return r;
        }
        return -1;  // 连续超时耗尽重试预算，判连接已死
    };

    std::string msg;
    int status = media_admin::Upload(path, req->content_len, overwrite, reader, msg);
    media_admin::EndUpload();
    if (status != 200) ESP_LOGW(TAG, "upload %s -> %d", path.c_str(), status);
    return SendJson(req, status, msg);
}

esp_err_t MkdirPost(httpd_req_t* req) {
    Touch();
    std::string body = ReadFormBody(req);
    std::string path;
    if (!media_admin::FormField(body, "path", path))
        return SendJson(req, 400, "{\"error\":\"missing path\"}");
    std::string msg;
    int status = media_admin::Mkdir(path, msg);
    return SendJson(req, status, msg);
}

esp_err_t DeletePost(httpd_req_t* req) {
    Touch();
    std::string body = ReadFormBody(req);
    std::string path, rec;
    if (!media_admin::FormField(body, "path", path))
        return SendJson(req, 400, "{\"error\":\"missing path\"}");
    media_admin::FormField(body, "recursive", rec);
    std::string msg;
    int status = media_admin::Delete(path, rec == "1", msg);
    return SendJson(req, status, msg);
}

void Register(const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = fn;
    httpd_register_uri_handler(s_server, &u);
}

void IdleCheck(void*) {
    if (s_server == nullptr) return;
    if (esp_timer_get_time() - s_last_activity.load() > kIdleStopUs) {
        ESP_LOGI(TAG, "idle 10min, auto-stopping");
        Stop();
    }
}

}  // namespace

bool Start() {
    if (s_server != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;  // 大文件上传放宽
    config.send_wait_timeout = 30;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    Register("/", HTTP_GET, RootGet);
    Register("/api/list", HTTP_GET, ListGet);
    Register("/api/space", HTTP_GET, SpaceGet);
    Register("/api/upload", HTTP_POST, UploadPost);
    Register("/api/mkdir", HTTP_POST, MkdirPost);
    Register("/api/delete", HTTP_POST, DeletePost);

    Touch();
    if (s_idle_timer == nullptr) {
        esp_timer_create_args_t targs = {};
        targs.callback = IdleCheck;
        targs.name = "media_admin_idle";
        esp_timer_create(&targs, &s_idle_timer);
    }
    esp_timer_start_periodic(s_idle_timer, kIdleCheckUs);
    ESP_LOGI(TAG, "media admin server on :80");
    return true;
}

void Stop() {
    if (s_server == nullptr) return;
    if (s_idle_timer != nullptr) esp_timer_stop(s_idle_timer);
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "media admin server stopped");
}

bool IsRunning() { return s_server != nullptr; }

std::string GetUrl() {
    if (s_server == nullptr) return "";
    std::string ip = mhal::network::GetIpAddress();
    if (ip.empty()) return "";
    return "http://" + ip;
}

}  // namespace media_admin::httpd
