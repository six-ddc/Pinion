// media_admin_core.cc — 见头文件。可移植核心，无 httpd 依赖。

#include "media_admin_core.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "metalio_hal/storage.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace media_admin {
namespace {

std::atomic<bool> s_upload_busy{false};

std::string Base() { return mhal::storage::GetMountPoint(); }

// 单段校验：非空、无内嵌 '/'、无 '\\'、非 "."/".."、无控制字符（<0x20）。UTF-8 中文放行。
bool SegOk(const std::string& seg) {
    if (seg.empty() || seg == "." || seg == "..") return false;
    for (char c : seg) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '\\' || c == '/') return false;
    }
    return true;
}

// 把 rel 按 '/' 切成段，逐段校验；首段须为根名。out_segs 收集各段。
bool SplitAndCheck(const std::string& rel, std::vector<std::string>& out_segs) {
    out_segs.clear();
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t slash = rel.find('/', pos);
        if (slash == std::string::npos) slash = rel.size();
        std::string seg = rel.substr(pos, slash - pos);
        if (!SegOk(seg)) return false;
        out_segs.push_back(seg);
        pos = slash + 1;
        if (slash == rel.size()) break;
    }
    if (out_segs.empty()) return false;
    if (out_segs[0] != kRoots[0] && out_segs[0] != kRoots[1]) return false;
    return true;
}

std::string JoinAbs(const std::vector<std::string>& segs) {
    std::string p = Base();
    for (const auto& s : segs) {
        p += '/';
        p += s;
    }
    return p;
}

// mkdir -p：逐级建 abs_dir（须已在 Base 下，调用方保证）。全部成功或已存在 → true。
bool MkdirP(const std::string& abs_dir) {
    std::string cur;
    size_t pos = 0;
    // 保留起始 '/'（绝对路径）。
    while (pos < abs_dir.size()) {
        size_t slash = abs_dir.find('/', pos + 1);
        if (slash == std::string::npos) slash = abs_dir.size();
        cur = abs_dir.substr(0, slash);
        if (!cur.empty() && cur != "/") {
            struct stat st;
            if (stat(cur.c_str(), &st) != 0) {
                if (mkdir(cur.c_str(), 0777) != 0) return false;
            } else if (!S_ISDIR(st.st_mode)) {
                return false;
            }
        }
        pos = slash;
    }
    return true;
}

void EnsureRoots() {
    for (const char* r : kRoots) {
        std::string p = Base() + "/" + r;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) mkdir(p.c_str(), 0777);
    }
}

int RmRecursive(const std::string& abs, int depth) {
    if (depth > kDeleteMaxDepth) return -1;
    DIR* dir = opendir(abs.c_str());
    if (dir == nullptr) return -1;
    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        std::string child = abs + "/" + ent->d_name;
        struct stat st;
        if (stat(child.c_str(), &st) != 0) {
            rc = -1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (RmRecursive(child, depth + 1) != 0) rc = -1;
        } else {
            if (unlink(child.c_str()) != 0) rc = -1;
        }
    }
    closedir(dir);
    if (rmdir(abs.c_str()) != 0) rc = -1;
    return rc;
}

void* ChunkAlloc(size_t n) {
#ifdef ESP_PLATFORM
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != nullptr) return p;
#endif
    return std::malloc(n);
}

}  // namespace

void JsonEscapeInto(std::string& out, const std::string& s) {
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", u);
            out += buf;
        } else {
            out += c;  // UTF-8 中文原样透传
        }
    }
}

bool HasMp3Suffix(const std::string& name) {
    if (name.size() < 4) return false;
    const char* e = name.c_str() + name.size() - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'm' && (e[2] | 0x20) == 'p' && e[3] == '3';
}

bool ResolvePath(const std::string& rel, std::string& out_abs) {
    std::vector<std::string> segs;
    if (!SplitAndCheck(rel, segs)) return false;
    out_abs = JoinAbs(segs);
    return true;
}

std::string ListJson(const std::string& dir, int& status) {
    if (!mhal::storage::IsSdMounted()) {
        status = 503;
        return "{\"error\":\"SD not mounted\"}";
    }

    struct Entry {
        std::string name;
        bool is_dir;
        uint64_t size;
        long mtime;
    };
    std::vector<Entry> entries;
    std::string dir_out;

    if (dir.empty()) {
        // 两个根：确保存在后列出。
        EnsureRoots();
        for (const char* r : kRoots) {
            std::string p = Base() + "/" + r;
            struct stat st;
            long mt = 0;
            if (stat(p.c_str(), &st) == 0) mt = static_cast<long>(st.st_mtime);
            entries.push_back({r, true, 0, mt});
        }
    } else {
        std::string abs;
        if (!ResolvePath(dir, abs)) {
            status = 400;
            return "{\"error\":\"bad path\"}";
        }
        DIR* d = opendir(abs.c_str());
        if (d == nullptr) {
            status = 404;
            return "{\"error\":\"not found\"}";
        }
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;  // 隐藏项 + . / ..
            std::string child = abs + "/" + ent->d_name;
            struct stat st;
            if (stat(child.c_str(), &st) != 0) continue;
            bool is_dir = S_ISDIR(st.st_mode);
            if (!is_dir && !HasMp3Suffix(ent->d_name)) continue;  // 只显示子目录与 .mp3
            entries.push_back({ent->d_name, is_dir,
                               is_dir ? 0 : static_cast<uint64_t>(st.st_size),
                               static_cast<long>(st.st_mtime)});
        }
        closedir(d);
        dir_out = dir;
    }

    // 目录在前、同类按名排序（大小写不敏感）。
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    std::string json = "{\"dir\":\"";
    JsonEscapeInto(json, dir_out);
    json += "\",\"entries\":[";
    bool first = true;
    for (const auto& e : entries) {
        if (!first) json += ',';
        first = false;
        json += "{\"name\":\"";
        JsonEscapeInto(json, e.name);
        json += "\",\"is_dir\":";
        json += e.is_dir ? "true" : "false";
        json += ",\"size\":";
        json += std::to_string(e.size);
        json += ",\"mtime\":";
        json += std::to_string(e.mtime);
        json += '}';
    }
    json += "]}";
    status = 200;
    return json;
}

std::string SpaceJson() {
    uint64_t total = 0, freeb = 0;
    bool ok = mhal::storage::GetSdFreeBytes(total, freeb);
    std::string json = "{\"mounted\":";
    json += (ok ? "true" : "false");
    json += ",\"total\":";
    json += std::to_string(total);
    json += ",\"free\":";
    json += std::to_string(freeb);
    json += '}';
    return json;
}

int Mkdir(const std::string& rel, std::string& msg) {
    if (!mhal::storage::IsSdMounted()) {
        msg = "{\"error\":\"SD not mounted\"}";
        return 503;
    }
    std::string abs;
    if (!ResolvePath(rel, abs)) {
        msg = "{\"error\":\"bad path\"}";
        return 400;
    }
    if (!MkdirP(abs)) {
        msg = "{\"error\":\"mkdir failed\"}";
        return 500;
    }
    msg = "{\"ok\":true}";
    return 200;
}

int Delete(const std::string& rel, bool recursive, std::string& msg) {
    if (!mhal::storage::IsSdMounted()) {
        msg = "{\"error\":\"SD not mounted\"}";
        return 503;
    }
    std::vector<std::string> segs;
    if (!SplitAndCheck(rel, segs)) {
        msg = "{\"error\":\"bad path\"}";
        return 400;
    }
    if (segs.size() < 2) {  // 禁止删两个根本身（Music / Podcasts），只准删根下面的内容
        msg = "{\"error\":\"cannot delete root\"}";
        return 403;
    }
    std::string abs = JoinAbs(segs);
    struct stat st;
    if (stat(abs.c_str(), &st) != 0) {
        msg = "{\"error\":\"not found\"}";
        return 404;
    }
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(abs.c_str()) == 0) {  // 空目录直接成功
            msg = "{\"ok\":true}";
            return 200;
        }
        if (!recursive) {
            msg = "{\"error\":\"directory not empty\",\"need_recursive\":true}";
            return 409;
        }
        if (RmRecursive(abs, 0) != 0) {
            msg = "{\"error\":\"recursive delete failed\"}";
            return 500;
        }
    } else {
        if (unlink(abs.c_str()) != 0) {
            msg = "{\"error\":\"delete failed\"}";
            return 500;
        }
    }
    msg = "{\"ok\":true}";
    return 200;
}

int Upload(const std::string& rel, uint64_t content_len, bool overwrite, const ReadFn& reader,
           std::string& msg) {
    if (!mhal::storage::IsSdMounted()) {
        msg = "{\"error\":\"SD not mounted\"}";
        return 503;
    }
    std::vector<std::string> segs;
    if (!SplitAndCheck(rel, segs) || segs.size() < 2) {
        msg = "{\"error\":\"bad path\"}";
        return 400;
    }
    const std::string& fname = segs.back();
    if (!HasMp3Suffix(fname)) {
        msg = "{\"error\":\"only .mp3 accepted\"}";
        return 400;
    }
    if (content_len > kMaxUploadBytes) {
        msg = "{\"error\":\"file too large\"}";
        return 413;
    }
    // 空间预检：需保留安全垫。
    uint64_t total = 0, freeb = 0;
    if (mhal::storage::GetSdFreeBytes(total, freeb)) {
        uint64_t budget = freeb > kSpaceSafetyBytes ? freeb - kSpaceSafetyBytes : 0;
        if (content_len > budget) {
            msg = "{\"error\":\"insufficient space\"}";
            return 507;
        }
    }

    std::string abs = JoinAbs(segs);
    // 父目录逐级建（目录上传的关键）。
    std::string parent = JoinAbs(std::vector<std::string>(segs.begin(), segs.end() - 1));
    if (!MkdirP(parent)) {
        msg = "{\"error\":\"mkdir failed\"}";
        return 500;
    }

    struct stat st;
    bool exists = stat(abs.c_str(), &st) == 0;
    if (exists && !overwrite) {
        msg = "{\"error\":\"exists\",\"conflict\":true}";
        return 409;
    }

    std::string part = abs + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (f == nullptr) {
        msg = "{\"error\":\"cannot create file\"}";
        return 500;
    }
    char* buf = static_cast<char*>(ChunkAlloc(kUploadChunk));
    if (buf == nullptr) {
        std::fclose(f);
        unlink(part.c_str());
        msg = "{\"error\":\"out of memory\"}";
        return 500;
    }

    bool io_err = false;      // 磁盘写错（满）
    bool recv_err = false;    // 客户端断开 / recv 错
    for (;;) {
        int r = reader(buf, kUploadChunk);
        if (r == 0) break;  // EOF
        if (r < 0) {
            recv_err = true;
            break;
        }
        if (std::fwrite(buf, 1, static_cast<size_t>(r), f) != static_cast<size_t>(r)) {
            io_err = true;
            break;
        }
    }
    std::free(buf);
    bool flush_ok = (std::fflush(f) == 0);
    std::fclose(f);

    if (io_err || recv_err || !flush_ok) {
        unlink(part.c_str());  // 清半成品
        if (recv_err) {
            msg = "{\"error\":\"connection interrupted\"}";
            return 400;
        }
        msg = "{\"error\":\"write failed (disk full?)\"}";
        return 507;
    }

    if (exists) unlink(abs.c_str());  // overwrite：先删旧文件再落位
    if (rename(part.c_str(), abs.c_str()) != 0) {
        unlink(part.c_str());
        msg = "{\"error\":\"rename failed\"}";
        return 500;
    }
    msg = "{\"ok\":true}";
    return 200;
}

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

bool TryBeginUpload() {
    bool expected = false;
    return s_upload_busy.compare_exchange_strong(expected, true);
}

void EndUpload() { s_upload_busy.store(false); }

}  // namespace media_admin
