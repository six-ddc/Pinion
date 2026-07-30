// web_admin_httpd.cc — 设备端 esp_http_server 薄壳。
// 只负责：httpd 生命周期、URI 路由、请求参数抽取、body 流式 recv → 交给可移植
// 逻辑（web_admin_fs 文件管理 / web_admin_config 设备配置）。10 分钟无请求自动
// 停服，省电防长期暴露。

#include "web_admin_httpd.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "web_admin_config.h"
#include "web_admin_fs.h"
#include "web_admin_web.h"
#include "metalio_hal/network.h"

namespace web_admin::httpd {
namespace {

constexpr char TAG[] = "web_admin";
constexpr int64_t kIdleStopUs = 10ll * 60 * 1000 * 1000;  // 10 分钟无请求自动停
constexpr int64_t kIdleCheckUs = 60ll * 1000 * 1000;      // 每分钟检查一次
constexpr int kFormRecvMaxTimeouts = 3;                   // mkdir/delete body 累计超时上限
constexpr int kUploadMaxTimeouts = 20;                    // 整个上传累计超时上限（跨 chunk）

// s_server 用 atomic：Stop（LVGL 线程）与 IdleCheck（esp_timer 线程）会并发读它，
// 置空/读取都走原子避免数据竞争；Start/Stop 的"检查-动作"复合序用 s_lifecycle_mtx
// 串行化（见 Start/Stop 注释）。
std::atomic<httpd_handle_t> s_server{nullptr};
esp_timer_handle_t s_idle_timer = nullptr;
esp_timer_handle_t s_reboot_timer = nullptr;  // /api/config/apply 的延迟重启
std::atomic<int64_t> s_last_activity{0};
std::mutex s_lifecycle_mtx;

// Touch 只做一次原子写，故意不进 s_lifecycle_mtx：它在 httpd 任务的每个 handler 里
// 被调，而 Stop() 持锁时会调 httpd_stop 等待 httpd 任务收尾——若 Touch 也抢这把锁，
// 在途 handler 的 Touch 会与 Stop 互等成死锁。s_last_activity 本身是 atomic，无需锁。
void Touch() { s_last_activity.store(esp_timer_get_time()); }

const char* StatusStr(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 409: return "409 Conflict";
        case 411: return "411 Length Required";
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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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
    out = web_admin::fs::UrlDecode(val);
    return true;
}

// 读 form body。路径类 2KB 足够；配置页要粘整份 models JSON，用 max 放宽。
std::string ReadFormBody(httpd_req_t* req, size_t max = 2048) {
    std::string body;
    size_t remaining = req->content_len;
    if (remaining > max) remaining = max;
    char buf[512];
    int timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= kFormRecvMaxTimeouts) break;  // 累计超时上限，放弃而非无限等
            continue;
        }
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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, web_admin_web::Html());
}

esp_err_t ListGet(httpd_req_t* req) {
    Touch();
    std::string dir;
    QueryVal(QueryStr(req), "dir", dir);
    int status = 200;
    std::string json = web_admin::fs::ListJson(dir, status);
    return SendJson(req, status, json);
}

esp_err_t SpaceGet(httpd_req_t* req) {
    Touch();
    return SendJson(req, 200, web_admin::fs::SpaceJson());
}

esp_err_t UploadPost(httpd_req_t* req) {
    Touch();
    // 无 Content-Length（含 chunked）→ 无法预检大小/空间，且会静默落 0 字节文件，直接拒。
    if (req->content_len == 0) return SendJson(req, 411, "{\"error\":\"length required\"}");

    std::string q = QueryStr(req);
    std::string path, over;
    QueryVal(q, "path", path);
    QueryVal(q, "overwrite", over);
    bool overwrite = (over == "1");

    if (!web_admin::fs::TryBeginUpload())
        return SendJson(req, 429, "{\"error\":\"another upload in progress\"}");

    // 流式 reader：从 httpd 拉取 body，最多 content_len 字节。
    //
    // 控制流：httpd_req_recv 在 recv_wait_timeout（见 Start() 里的 30s）内没等到
    // 数据会返回 HTTPD_SOCK_ERR_TIMEOUT（负值）——这只代表"这次没读到"，不是连接
    // 已断，弱 WiFi 一次瞬时 stall 很常见，因此超时时 continue 重试而非直接判失败；
    // 成功路径(r>0)统一在循环里扣减 remaining 并 return；r<=0 且非超时（客户端断开 /
    // 其他错误）立即 -1（Upload() 会 unlink .part）。
    //
    // 超时预算 timeout_budget 是**整个上传累计**（跨 chunk 不重置，捕获引用），耗尽即
    // 判连接已死返回 -1——避免"每 chunk 重置预算"被慢速滴送的客户端利用无限占用单
    // 线程 httpd 任务。上限 kUploadMaxTimeouts * 30s ≈ 10min 总容忍。
    uint64_t remaining = req->content_len;
    int timeout_budget = kUploadMaxTimeouts;
    auto reader = [req, &remaining, &timeout_budget](char* buf, size_t max) -> int {
        if (remaining == 0) return 0;  // EOF
        size_t want = remaining < max ? static_cast<size_t>(remaining) : max;
        for (;;) {
            int r = httpd_req_recv(req, buf, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {          // 瞬时 stall，重试而非直接判失败
                if (--timeout_budget <= 0) return -1;   // 累计预算耗尽，判连接已死
                continue;
            }
            if (r <= 0) return -1;  // 断开 / 其他错误
            remaining -= r;
            Touch();  // 单个大文件传输可能超过闲置阈值，按 chunk 刷新防止中途被自停
            return r;
        }
    };

    std::string msg;
    int status = web_admin::fs::Upload(path, req->content_len, overwrite, reader, msg);
    web_admin::fs::EndUpload();
    if (status != 200) ESP_LOGW(TAG, "upload %s -> %d", path.c_str(), status);
    return SendJson(req, status, msg);
}

esp_err_t MkdirPost(httpd_req_t* req) {
    Touch();
    std::string body = ReadFormBody(req);
    std::string path;
    if (!web_admin::fs::FormField(body, "path", path))
        return SendJson(req, 400, "{\"error\":\"missing path\"}");
    std::string msg;
    int status = web_admin::fs::Mkdir(path, msg);
    return SendJson(req, status, msg);
}

esp_err_t DeletePost(httpd_req_t* req) {
    Touch();
    std::string body = ReadFormBody(req);
    std::string path, rec;
    if (!web_admin::fs::FormField(body, "path", path))
        return SendJson(req, 400, "{\"error\":\"missing path\"}");
    web_admin::fs::FormField(body, "recursive", rec);
    std::string msg;
    int status = web_admin::fs::Delete(path, rec == "1", msg);
    return SendJson(req, status, msg);
}

// ---- 配置页（大模型 / 语音密钥）----
esp_err_t ConfigGet(httpd_req_t* req) {
    Touch();
    return SendJson(req, 200, web_admin::config::StatusJson());
}

esp_err_t ConfigPost(httpd_req_t* req) {
    Touch();
    std::string body = ReadFormBody(req, web_admin::config::kMaxFormBytes);
    std::string msg;
    int status = web_admin::config::Save(body, msg);
    if (status != 200) ESP_LOGW(TAG, "config save -> %d", status);
    return SendJson(req, status, msg);
}

void RebootNow(void*) { esp_restart(); }

// 保存后由网页显式调用：先把 200 发出去（让网页能提示"正在重启"），再延迟重启。
esp_err_t ApplyPost(httpd_req_t* req) {
    Touch();
    esp_err_t err = SendJson(req, 200, R"({"ok":true,"reboot_in_ms":1500})");
    if (s_reboot_timer == nullptr) {
        esp_timer_create_args_t targs = {};
        targs.callback = RebootNow;
        targs.name = "web_admin_reboot";
        esp_timer_create(&targs, &s_reboot_timer);
    }
    if (s_reboot_timer != nullptr) {
        ESP_LOGI(TAG, "config applied, rebooting in 1.5s");
        esp_timer_start_once(s_reboot_timer, 1500 * 1000);
    }
    return err;
}

void Register(const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = fn;
    httpd_register_uri_handler(s_server.load(), &u);
}

void IdleCheck(void*) {
    if (s_server.load() == nullptr) return;
    if (esp_timer_get_time() - s_last_activity.load() > kIdleStopUs) {
        ESP_LOGI(TAG, "idle 10min, auto-stopping");
        Stop();  // Stop 内部再持锁复检，与 LVGL 线程的并发 Stop 互斥
    }
}

}  // namespace

bool Start() {
    std::lock_guard<std::mutex> lk(s_lifecycle_mtx);  // 与 Stop 串行化，防 Start/Stop 交错
    if (s_server.load() != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 12;  // 文件管理 6 + 配置 3，留余量
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;  // 大文件上传放宽
    config.send_wait_timeout = 30;
    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    s_server.store(srv);
    web_admin::fs::SweepOrphans();  // 清上次崩溃/掉电遗留的 .part / .old 孤儿
    Register("/", HTTP_GET, RootGet);
    Register("/api/list", HTTP_GET, ListGet);
    Register("/api/space", HTTP_GET, SpaceGet);
    Register("/api/upload", HTTP_POST, UploadPost);
    Register("/api/mkdir", HTTP_POST, MkdirPost);
    Register("/api/delete", HTTP_POST, DeletePost);
    Register("/api/config", HTTP_GET, ConfigGet);
    Register("/api/config", HTTP_POST, ConfigPost);
    Register("/api/config/apply", HTTP_POST, ApplyPost);

    Touch();
    if (s_idle_timer == nullptr) {
        esp_timer_create_args_t targs = {};
        targs.callback = IdleCheck;
        targs.name = "web_admin_idle";
        esp_timer_create(&targs, &s_idle_timer);
    }
    esp_timer_start_periodic(s_idle_timer, kIdleCheckUs);
    ESP_LOGI(TAG, "web admin server on :80");
    return true;
}

void Stop() {
    // 锁内只做"检查并置空 s_server"这段短临界区：并发的第二个 Stop（或 IdleCheck 触发
    // 的 Stop）进来看到已是 nullptr 即早退，只有抢到非空的那一个继续 httpd_stop，从而
    // 消除双重 httpd_stop 竞态。httpd_stop 本身放到锁外执行——它可能阻塞（等 httpd 任务
    // 收尾在途 handler），不占 lifecycle 锁，避免拖住并发的 Start/Stop 与 esp_timer 任务。
    httpd_handle_t srv;
    {
        std::lock_guard<std::mutex> lk(s_lifecycle_mtx);
        srv = s_server.load();
        if (srv == nullptr) return;
        if (s_idle_timer != nullptr) esp_timer_stop(s_idle_timer);
        s_server.store(nullptr);
    }
    httpd_stop(srv);
    ESP_LOGI(TAG, "web admin server stopped");
}

bool IsRunning() { return s_server.load() != nullptr; }

std::string GetUrl() {
    if (s_server.load() == nullptr) return "";
    std::string ip = mhal::network::GetIpAddress();
    if (ip.empty()) return "";
    return "http://" + ip;
}

}  // namespace web_admin::httpd
