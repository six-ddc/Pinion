// web_admin_httpd_sim.cc — sim 端极简 POSIX socket HTTP 薄壳。
//
// 挂同一套可移植 core（web_admin_fs / web_admin_config）+ 同一份内嵌网页，监听
// 127.0.0.1:8080，实现与设备端 web_admin::httpd 相同的接口，供 UI 与浏览器复用。
// 只支撑本页面用到的 GET / POST（Content-Length body，含大文件流式上传）。
// 每连接一线程 → 复用 core 的 TryBeginUpload 互斥即可复现"并发第二个 upload→429"。

#include "web_admin_httpd.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <csignal>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "web_admin_config.h"
#include "web_admin_fs.h"
#include "web_admin_web.h"

namespace web_admin::httpd {
namespace {

constexpr int kPort = 8080;

std::atomic<bool> s_running{false};
int s_listen_fd = -1;
std::thread s_accept_thread;

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

void SendAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0) return;
        off += n;
    }
}

void SendResponse(int fd, int code, const char* ctype, const std::string& body) {
    char hdr[320];
    int hn = std::snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           StatusStr(code), ctype, body.size());
    if (hn < 0) return;
    if (hn > static_cast<int>(sizeof(hdr))) hn = sizeof(hdr);  // 防截断时越界读
    SendAll(fd, hdr, hn);
    SendAll(fd, body.data(), body.size());
}

// query "a=1&b=%E4..." 取字段并 URL 解码。
bool QueryVal(const std::string& q, const char* key, std::string& out) {
    std::string k(key);
    size_t pos = 0;
    while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.size();
        std::string tok = q.substr(pos, amp - pos);
        pos = amp + 1;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        if (tok.substr(0, eq) == k) {
            out = web_admin::fs::UrlDecode(tok.substr(eq + 1));
            return true;
        }
    }
    return false;
}

// 从 socket 拉 body 的 reader：先吐 header 之后残留的 leftover，再 recv。
struct BodyReader {
    int fd;
    std::string leftover;
    size_t off = 0;
    uint64_t remaining;  // content_len
};

int ReadBody(BodyReader* br, char* buf, size_t max) {
    if (br->remaining == 0) return 0;  // EOF
    size_t want = br->remaining < max ? static_cast<size_t>(br->remaining) : max;
    // 先消费 leftover
    if (br->off < br->leftover.size()) {
        size_t avail = br->leftover.size() - br->off;
        size_t take = avail < want ? avail : want;
        std::memcpy(buf, br->leftover.data() + br->off, take);
        br->off += take;
        br->remaining -= take;
        return static_cast<int>(take);
    }
    ssize_t n = ::recv(br->fd, buf, want, 0);
    if (n <= 0) return -1;  // 断开 / 错误
    br->remaining -= n;
    return static_cast<int>(n);
}

// 把 leftover 补满 content_len，作为完整 form body 返回（mkdir/delete/config 共用）。
std::string ReadFormBodyFull(int fd, const std::string& leftover, uint64_t content_len, char* tmp,
                             size_t tmp_size) {
    std::string body = leftover;
    while (body.size() < content_len) {
        ssize_t n = ::recv(fd, tmp, tmp_size, 0);
        if (n <= 0) break;
        body.append(tmp, n);
    }
    return body;
}

void HandleConn(int fd) {
    // 给连接设 recv 超时：慢速/停顿客户端不再占死本连接线程。sim 侧简化为"一次 recv
    // 超时即当断开中止"（device 壳则用累计超时预算重试）——两端都有界，sim 更严格；
    // localhost 正常传输不会空闲到 5s，不影响大文件上传。
    struct timeval tv {
        5, 0
    };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 读到 header 结束（\r\n\r\n）；把多读的部分留作 body leftover。
    std::string raw;
    char tmp[4096];
    size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            ::close(fd);
            return;
        }
        raw.append(tmp, n);
        hdr_end = raw.find("\r\n\r\n");
        if (raw.size() > 64 * 1024) break;  // header 过大，异常
    }
    if (hdr_end == std::string::npos) {
        ::close(fd);
        return;
    }
    std::string header = raw.substr(0, hdr_end);
    std::string leftover = raw.substr(hdr_end + 4);

    // 请求行：METHOD SP target SP HTTP/1.1
    size_t sp1 = header.find(' ');
    size_t sp2 = header.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        ::close(fd);
        return;
    }
    std::string method = header.substr(0, sp1);
    std::string target = header.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) {
        path = target.substr(0, qm);
        query = target.substr(qm + 1);
    }

    // Content-Length（大小写不敏感）
    uint64_t content_len = 0;
    {
        std::string lower = header;
        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
        size_t p = lower.find("content-length:");
        if (p != std::string::npos)
            content_len = std::strtoull(header.c_str() + p + 15, nullptr, 10);
    }

    // ---- 路由 ----
    if (method == "GET" && path == "/") {
        SendResponse(fd, 200, "text/html; charset=utf-8", web_admin_web::Html());
    } else if (method == "GET" && path == "/api/list") {
        std::string dir;
        QueryVal(query, "dir", dir);
        int status = 200;
        std::string json = web_admin::fs::ListJson(dir, status);
        SendResponse(fd, status, "application/json; charset=utf-8", json);
    } else if (method == "GET" && path == "/api/space") {
        SendResponse(fd, 200, "application/json; charset=utf-8", web_admin::fs::SpaceJson());
    } else if (method == "POST" && path == "/api/upload") {
        std::string upath, over;
        QueryVal(query, "path", upath);
        QueryVal(query, "overwrite", over);
        if (content_len == 0) {  // 无 Content-Length → 拒，勿静默落 0 字节文件（与 device 壳一致）
            SendResponse(fd, 411, "application/json; charset=utf-8", "{\"error\":\"length required\"}");
        } else if (!web_admin::fs::TryBeginUpload()) {
            SendResponse(fd, 429, "application/json; charset=utf-8",
                         "{\"error\":\"another upload in progress\"}");
        } else {
            BodyReader br{fd, leftover, 0, content_len};
            auto reader = [&br](char* buf, size_t max) -> int { return ReadBody(&br, buf, max); };
            std::string msg;
            int status = web_admin::fs::Upload(upath, content_len, over == "1", reader, msg);
            web_admin::fs::EndUpload();
            SendResponse(fd, status, "application/json; charset=utf-8", msg);
        }
    } else if (method == "GET" && path == "/api/config") {
        SendResponse(fd, 200, "application/json; charset=utf-8", web_admin::config::StatusJson());
    } else if (method == "POST" && path == "/api/config") {
        std::string body = ReadFormBodyFull(fd, leftover, content_len, tmp, sizeof(tmp));
        std::string msg;
        int status = web_admin::config::Save(body, msg);
        SendResponse(fd, status, "application/json; charset=utf-8", msg);
    } else if (method == "POST" && path == "/api/config/apply") {
        // sim 不重启（进程重启会关掉窗口）：只回 200 并提示，配置已在 NVS 里，
        // 手动重启 pi_sim 即可生效——与真机"延迟 1.5s esp_restart"的语义对齐。
        std::printf("[web_admin] config applied; restart pi_sim to take effect\n");
        SendResponse(fd, 200, "application/json; charset=utf-8",
                     "{\"ok\":true,\"reboot_in_ms\":0,\"sim\":true}");
    } else if (method == "POST" && (path == "/api/mkdir" || path == "/api/delete")) {
        std::string body = ReadFormBodyFull(fd, leftover, content_len, tmp, sizeof(tmp));
        std::string p;
        if (!web_admin::fs::FormField(body, "path", p)) {
            SendResponse(fd, 400, "application/json; charset=utf-8", "{\"error\":\"missing path\"}");
        } else if (path == "/api/mkdir") {
            std::string msg;
            int status = web_admin::fs::Mkdir(p, msg);
            SendResponse(fd, status, "application/json; charset=utf-8", msg);
        } else {
            std::string rec;
            web_admin::fs::FormField(body, "recursive", rec);
            std::string msg;
            int status = web_admin::fs::Delete(p, rec == "1", msg);
            SendResponse(fd, status, "application/json; charset=utf-8", msg);
        }
    } else {
        SendResponse(fd, 404, "application/json; charset=utf-8", "{\"error\":\"not found\"}");
    }
    ::close(fd);
}

void AcceptLoop() {
    while (s_running.load()) {
        int fd = ::accept(s_listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (!s_running.load()) break;
            continue;
        }
        std::thread(HandleConn, fd).detach();
    }
}

}  // namespace

bool Start() {
    if (s_running.load()) return true;
    // 客户端中途断开后向死 socket send 会触发 SIGPIPE，默认动作会杀掉整个进程；忽略之。
    std::signal(SIGPIPE, SIG_IGN);
    s_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) return false;
    int yes = 1;
    ::setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = htons(kPort);
    if (::bind(s_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s_listen_fd, 8) != 0) {
        ::close(s_listen_fd);
        s_listen_fd = -1;
        std::fprintf(stderr, "[sim][admin] bind/listen :%d failed\n", kPort);
        return false;
    }
    web_admin::fs::SweepOrphans();  // 清上次遗留的 .part / .old 孤儿（与 device 壳一致）
    s_running.store(true);
    s_accept_thread = std::thread(AcceptLoop);
    // 进程退出时把 accept 线程收干净：joinable 的 std::thread 走静态析构会
    // std::terminate（退出码 134），看起来跟真崩溃一模一样，会毒化 sim 的无人值守
    // 截图/自测。设备端无此问题（esp_restart 直接走）。
    static bool atexit_hooked = false;
    if (!atexit_hooked) {
        atexit_hooked = true;
        std::atexit([]() { Stop(); });
    }
    std::fprintf(stderr, "[sim][admin] web admin server on http://127.0.0.1:%d\n", kPort);
    return true;
}

void Stop() {
    if (!s_running.load()) return;
    s_running.store(false);
    if (s_listen_fd >= 0) {
        ::shutdown(s_listen_fd, SHUT_RDWR);
        ::close(s_listen_fd);
        s_listen_fd = -1;
    }
    if (s_accept_thread.joinable()) s_accept_thread.join();
    std::fprintf(stderr, "[sim][admin] web admin server stopped\n");
}

bool IsRunning() { return s_running.load(); }

std::string GetUrl() {
    if (!s_running.load()) return "";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "http://127.0.0.1:%d", kPort);
    return buf;
}

}  // namespace web_admin::httpd
