// book_config_server.cc — 见头文件。参照 stock_config_server，增加流式文件上传。

#include "book_config_server.h"

#include "book_store.h"
#include "ebook_font_charset.h"
#include "ebook_web_page.h"

#include "SdCardManager.hpp"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <dirent.h>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace book_config_server {
namespace {

constexpr char TAG[] = "book_cfg";
constexpr size_t kNameMax = 120;             // 文件名最大字节（UTF-8）
constexpr size_t kUploadBuf = 8192;          // 流式写盘分块
constexpr size_t kMaxUpload = 64u * 1024 * 1024;  // 单文件上限（软限）

httpd_handle_t s_server = nullptr;
std::atomic<bool> s_dirty{false};

// ---- 工具（多数搬自 stock_config_server）----
std::string UrlDecode(const std::string& s) {
    auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return 0;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size()) {
            out += static_cast<char>((hex(s[i + 1]) << 4) | hex(s[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

bool FormField(const std::string& body, const char* key, std::string& out) {
    std::string k(key);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        std::string tok = body.substr(pos, amp - pos);
        pos = amp + 1;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        if (tok.substr(0, eq) == k) {
            out = UrlDecode(tok.substr(eq + 1));
            return true;
        }
    }
    return false;
}

std::string ReadBody(httpd_req_t* req) {
    std::string body;
    size_t remaining = req->content_len;
    if (remaining > 4096) remaining = 4096;
    char buf[512];
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (r <= 0) break;
        body.append(buf, r);
        remaining -= r;
    }
    return body;
}

void JsonEscapeInto(std::string& out, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;  // 跳过控制字符
        out += c;
    }
}

esp_err_t SendText(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

bool Suffix4(const char* e, char a, char b, char c, char d) {
    return e[0] == '.' && (e[1] | 0x20) == a && (e[2] | 0x20) == b && (e[3] | 0x20) == c &&
           (e[4] | 0x20) == d;
}

// 书籍后缀 .txt / .epub（不区分大小写）。
bool HasBookSuffix(const char* name, size_t n) {
    if (n >= 4) {
        const char* e = name + n - 4;
        if (e[0] == '.' && (e[1] | 0x20) == 't' && (e[2] | 0x20) == 'x' && (e[3] | 0x20) == 't')
            return true;
    }
    if (n >= 5 && Suffix4(name + n - 5, 'e', 'p', 'u', 'b')) return true;
    return false;
}

// 字体后缀 .ttf / .otf（不区分大小写）。
bool HasFontSuffix(const char* name, size_t n) {
    if (n < 4) return false;
    const char* e = name + n - 4;
    return (e[0] == '.' && (e[1] | 0x20) == 't' && (e[2] | 0x20) == 't' && (e[3] | 0x20) == 'f') ||
           (e[0] == '.' && (e[1] | 0x20) == 'o' && (e[2] | 0x20) == 't' && (e[3] | 0x20) == 'f');
}

// 上传/删除对象：书籍(.txt/.epub) 或字体(.ttf/.otf) 同放 /sdcard/books。
bool HasUploadSuffix(const char* name, size_t n) {
    return HasBookSuffix(name, n) || HasFontSuffix(name, n);
}

// 校验并规整文件名：非空、≤kNameMax、书籍/字体后缀、无路径穿越/控制字符。合法返回原名，否则空。
std::string SanitizeBookName(const std::string& raw) {
    if (raw.empty() || raw.size() > kNameMax) return "";
    if (raw.find('/') != std::string::npos || raw.find('\\') != std::string::npos) return "";
    if (raw.find("..") != std::string::npos) return "";
    for (char c : raw) {
        if (static_cast<unsigned char>(c) < 0x20) return "";
    }
    if (!HasUploadSuffix(raw.c_str(), raw.size())) return "";
    return raw;
}

void BuildPath(const char* name, char* out, size_t n) {
    snprintf(out, n, "%s/%s", book_store::kBooksDir, name);
}

void EnsureBooksDir() {
    struct stat st;
    if (stat(book_store::kBooksDir, &st) != 0) mkdir(book_store::kBooksDir, 0777);
}

// ---- handlers ----
esp_err_t RootGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, ebook_web_page::Html());
}

// 内置 puhui 字符集（delta-LEB128-base64）。Web 上传页据此在浏览器端裁剪用户字体，
// 使裁后覆盖与设备内置字体一致。同源请求，离线也能取。
esp_err_t FontCharsetGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, ebook_font_charset::PuhuiB64());
}

esp_err_t ListGet(httpd_req_t* req) {
    std::string json = "[";
    if (SdCardManager::GetInstance().IsMounted()) {
        EnsureBooksDir();
        // 安全 readdir：先收集名，再统一 stat（避免 readdir 循环内 stat 破坏句柄）。
        std::vector<std::string> names;
        DIR* dir = opendir(book_store::kBooksDir);
        if (dir != nullptr) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                if (!HasUploadSuffix(ent->d_name, strlen(ent->d_name))) continue;
                names.emplace_back(ent->d_name);
            }
            closedir(dir);
        }
        char path[300];
        bool first = true;
        for (const auto& nm : names) {
            BuildPath(nm.c_str(), path, sizeof(path));
            struct stat st;
            uint32_t size = 0;
            if (stat(path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) continue;
                size = static_cast<uint32_t>(st.st_size);
            }
            const char* type = HasFontSuffix(nm.c_str(), nm.size()) ? "font" : "book";
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"";
            JsonEscapeInto(json, nm);
            json += "\",\"size\":";
            json += std::to_string(size);
            json += ",\"type\":\"";
            json += type;
            json += "\"}";
        }
    }
    json += "]";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, json.c_str());
}

esp_err_t UploadPost(httpd_req_t* req) {
    // 文件名来自 query ?name=（中文名 URL 编码后可达 ~360 字符，缓冲放宽）
    std::string name;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen < 1024) {
        std::string qbuf(qlen, '\0');
        if (httpd_req_get_url_query_str(req, &qbuf[0], qlen) == ESP_OK) {
            char val[512];
            if (httpd_query_key_value(qbuf.c_str(), "name", val, sizeof(val)) == ESP_OK)
                name = UrlDecode(val);
        }
    }
    std::string safe = SanitizeBookName(name);
    if (safe.empty()) return SendText(req, "400 Bad Request", "文件名非法（需 .txt / .epub）");
    if (!SdCardManager::GetInstance().IsMounted())
        return SendText(req, "503 Service Unavailable", "SD 卡未挂载");
    if (req->content_len > kMaxUpload) return SendText(req, "413 Payload Too Large", "文件过大");
    EnsureBooksDir();

    char path[300];
    BuildPath(safe.c_str(), path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return SendText(req, "500 Internal Server Error", "无法创建文件");

    char* buf = static_cast<char*>(heap_caps_malloc(kUploadBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) buf = static_cast<char*>(malloc(kUploadBuf));
    if (buf == nullptr) {
        fclose(f);
        unlink(path);
        return SendText(req, "500 Internal Server Error", "内存不足");
    }

    bool err = false;
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < kUploadBuf ? remaining : kUploadBuf;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            err = true;
            break;
        }
        if (fwrite(buf, 1, r, f) != static_cast<size_t>(r)) {  // 磁盘满/写错误
            err = true;
            break;
        }
        remaining -= r;
    }
    free(buf);
    fclose(f);
    if (err) {
        unlink(path);  // 删除半成品
        ESP_LOGW(TAG, "upload failed: %s", safe.c_str());
        return SendText(req, "500 Internal Server Error", "写入失败（磁盘满或连接中断）");
    }
    s_dirty.store(true);
    ESP_LOGI(TAG, "uploaded: %s (%u bytes)", safe.c_str(), (unsigned)req->content_len);
    return SendText(req, "200 OK", "ok");
}

esp_err_t DeletePost(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string name;
    if (!FormField(body, "name", name)) return SendText(req, "400 Bad Request", "缺少参数");
    std::string safe = SanitizeBookName(name);
    if (safe.empty()) return SendText(req, "400 Bad Request", "文件名非法");
    if (!SdCardManager::GetInstance().IsMounted())
        return SendText(req, "503 Service Unavailable", "SD 卡未挂载");
    char path[300];
    BuildPath(safe.c_str(), path, sizeof(path));
    if (unlink(path) != 0) return SendText(req, "404 Not Found", "文件不存在");
    // 对应的 sidecar 索引缓存（.metalio/<hash>.idx）会成为孤儿，但按 file_size+name_hash
    // 校验，下次同名新书打开时不匹配即自动重建，无害，故不主动清理。
    s_dirty.store(true);
    ESP_LOGI(TAG, "deleted: %s", safe.c_str());
    return SendText(req, "200 OK", "ok");
}

void Register(const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = fn;
    httpd_register_uri_handler(s_server, &u);
}

}  // namespace

bool Start() {
    if (s_server != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;  // 大文件上传放宽超时
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    Register("/", HTTP_GET, RootGet);
    Register("/api/font-charset", HTTP_GET, FontCharsetGet);
    Register("/api/list", HTTP_GET, ListGet);
    Register("/api/upload", HTTP_POST, UploadPost);
    Register("/api/delete", HTTP_POST, DeletePost);
    ESP_LOGI(TAG, "book config server listening on :80");
    return true;
}

void Stop() {
    if (s_server == nullptr) return;
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "book config server stopped");
}

bool IsRunning() { return s_server != nullptr; }

bool ConsumeDirty() { return s_dirty.exchange(false); }

}  // namespace book_config_server
