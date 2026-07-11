// stock_config_server.cc — 见头文件。

#include "stock_config_server.h"

#include "stock_api.h"
#include "stock_store.h"
#include "stock_web_page.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <atomic>
#include <cstring>
#include <string>

namespace stock_config_server {
namespace {

constexpr char TAG[] = "stock_cfg";
constexpr size_t kMaxStocks = stock_store::kMaxStocks;

httpd_handle_t s_server = nullptr;
std::atomic<bool> s_dirty{false};

// ---- 工具 ----
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

// 从 application/x-www-form-urlencoded body 取字段（url-decoded）。
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

void Trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
        s.clear();
    } else {
        s = s.substr(a, b - a + 1);
    }
}

void JsonEscapeInto(std::string& out, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
}

esp_err_t SendText(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

// ---- handlers ----
esp_err_t RootGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, stock_web_page::Html());
}

esp_err_t ListGet(httpd_req_t* req) {
    stock_store::Entry entries[kMaxStocks];
    size_t n = stock_store::LoadFromNvs(entries, kMaxStocks);
    std::string json = "[";
    for (size_t i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"symbol\":\"";
        JsonEscapeInto(json, entries[i].symbol);
        json += "\",\"name\":\"";
        JsonEscapeInto(json, entries[i].name);
        json += "\"}";
    }
    json += "]";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, json.c_str());
}

esp_err_t SearchGet(httpd_req_t* req) {
    std::string q;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen < 256) {
        std::string qbuf(qlen, '\0');
        if (httpd_req_get_url_query_str(req, &qbuf[0], qlen) == ESP_OK) {
            char val[128];
            if (httpd_query_key_value(qbuf.c_str(), "q", val, sizeof(val)) == ESP_OK) {
                q = UrlDecode(val);
            }
        }
    }
    std::string json, err;
    stock_api::SearchStocks(q.c_str(), json, err);  // 失败时 json 仍是 "[]"
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, json.c_str());
}

esp_err_t AddPost(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string sym, name;
    if (!FormField(body, "symbol", sym) || !FormField(body, "name", name)) {
        return SendText(req, "400 Bad Request", "Missing parameters");
    }
    Trim(sym);
    Trim(name);
    if (sym.empty() || sym.size() > 24 || name.empty()) {
        return SendText(req, "400 Bad Request", "Invalid parameters");
    }
    stock_store::Entry e[kMaxStocks];
    size_t n = stock_store::LoadFromNvs(e, kMaxStocks);
    for (size_t i = 0; i < n; i++) {
        if (e[i].symbol == sym) return SendText(req, "409 Conflict", "Already exists");
    }
    if (n >= kMaxStocks) return SendText(req, "409 Conflict", "List is full (max 16)");
    e[n].symbol = sym;
    e[n].name = name;
    n++;
    stock_store::SaveToNvs(e, n);
    s_dirty.store(true);
    return SendText(req, "200 OK", "ok");
}

esp_err_t DeletePost(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string sym;
    if (!FormField(body, "symbol", sym)) {
        return SendText(req, "400 Bad Request", "Missing parameters");
    }
    stock_store::Entry e[kMaxStocks];
    size_t n = stock_store::LoadFromNvs(e, kMaxStocks);
    size_t out_n = 0;
    bool removed = false;
    for (size_t i = 0; i < n; i++) {
        if (e[i].symbol == sym) {
            removed = true;
            continue;
        }
        if (out_n != i) e[out_n] = e[i];
        out_n++;
    }
    if (!removed) return SendText(req, "404 Not Found", "Not found");
    stock_store::SaveToNvs(e, out_n);
    s_dirty.store(true);
    return SendText(req, "200 OK", "ok");
}

esp_err_t RenamePost(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string sym, name;
    if (!FormField(body, "symbol", sym) || !FormField(body, "name", name)) {
        return SendText(req, "400 Bad Request", "Missing parameters");
    }
    Trim(sym);
    Trim(name);
    if (sym.empty() || name.empty() || name.size() > 31) {
        return SendText(req, "400 Bad Request", "Invalid parameters");
    }
    if (name.find('\t') != std::string::npos || name.find('\n') != std::string::npos) {
        return SendText(req, "400 Bad Request", "Name cannot contain tabs or newlines");
    }
    stock_store::Entry e[kMaxStocks];
    size_t n = stock_store::LoadFromNvs(e, kMaxStocks);
    bool hit = false;
    for (size_t i = 0; i < n; i++) {
        if (e[i].symbol == sym) {
            e[i].name = name;
            hit = true;
            break;
        }
    }
    if (!hit) return SendText(req, "404 Not Found", "Not found");
    stock_store::SaveToNvs(e, n);
    s_dirty.store(true);
    return SendText(req, "200 OK", "ok");
}

esp_err_t ReorderPost(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string csv;
    if (!FormField(body, "symbols", csv)) {
        return SendText(req, "400 Bad Request", "Missing parameters");
    }
    stock_store::Entry cur[kMaxStocks];
    size_t n = stock_store::LoadFromNvs(cur, kMaxStocks);
    stock_store::Entry next[kMaxStocks];
    bool used[kMaxStocks] = {};
    size_t out_n = 0;
    size_t start = 0;
    while (start <= csv.size() && out_n < kMaxStocks) {
        size_t comma = csv.find(',', start);
        if (comma == std::string::npos) comma = csv.size();
        std::string sym = csv.substr(start, comma - start);
        Trim(sym);
        start = comma + 1;
        if (sym.empty()) continue;
        bool found = false;
        for (size_t i = 0; i < n; i++) {
            if (!used[i] && cur[i].symbol == sym) {
                next[out_n++] = cur[i];
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) return SendText(req, "400 Bad Request", "Contains unknown symbol");
    }
    if (out_n != n) return SendText(req, "400 Bad Request", "Count mismatch");
    stock_store::SaveToNvs(next, out_n);
    s_dirty.store(true);
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
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    Register("/", HTTP_GET, RootGet);
    Register("/api/list", HTTP_GET, ListGet);
    Register("/api/search", HTTP_GET, SearchGet);
    Register("/api/add", HTTP_POST, AddPost);
    Register("/api/delete", HTTP_POST, DeletePost);
    Register("/api/rename", HTTP_POST, RenamePost);
    Register("/api/reorder", HTTP_POST, ReorderPost);
    ESP_LOGI(TAG, "config server listening on :80");
    return true;
}

void Stop() {
    if (s_server == nullptr) return;
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "config server stopped");
}

bool IsRunning() { return s_server != nullptr; }

bool ConsumeDirty() { return s_dirty.exchange(false); }

}  // namespace stock_config_server
