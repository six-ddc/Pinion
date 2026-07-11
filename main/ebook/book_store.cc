// book_store.cc — 书架扫描 + 进度/设置持久化。

#include "book_store.h"

#include "SdCardManager.hpp"
#include "settings.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

namespace book_store {

namespace {

constexpr char TAG[] = "book_store";
constexpr const char* kNvsNs = "ebook";
constexpr size_t kNameMax = 256;

uint32_t Fnv1a(const char* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}

// 目录不存在则逐级创建（books + .metalio）。
void EnsureDirs() {
    struct stat st;
    if (stat(kBooksDir, &st) != 0) {
        if (mkdir(kBooksDir, 0777) != 0) {
            ESP_LOGW(TAG, "mkdir %s failed", kBooksDir);
            return;
        }
    }
    if (stat(kSidecarDir, &st) != 0) {
        mkdir(kSidecarDir, 0777);  // 失败不致命，缓存只是加速
    }
}

bool SuffixIs(const char* name, size_t n, const char* ext) {
    size_t el = strlen(ext);
    if (n <= el) return false;
    return strcasecmp(name + n - el, ext) == 0;
}

// 识别书籍文件：返回 true 并填 *format 与扩展名长度（含点）。
bool MatchBookSuffix(const char* name, BookFormat* format, size_t* ext_len) {
    size_t n = strlen(name);
    if (SuffixIs(name, n, ".txt")) {
        *format = BookFormat::kTxt;
        *ext_len = 4;
        return true;
    }
    if (SuffixIs(name, n, ".epub")) {
        *format = BookFormat::kEpub;
        *ext_len = 5;
        return true;
    }
    return false;
}

// NVS 进度 key："b" + 8 位十六进制（9 字符，满足 NVS ≤15 限制）；"p" 前缀存百分比。
void ProgressKey(char prefix, uint32_t hash, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%c%08lx", prefix, static_cast<unsigned long>(hash));
}

}  // namespace

uint32_t BookKeyHash(const std::string& name, uint32_t size) {
    char tail[24];
    int m = snprintf(tail, sizeof(tail), ":%lu", static_cast<unsigned long>(size));
    uint32_t h = Fnv1a(name.data(), name.size());
    // 继续把 size 尾巴混进同一条 FNV 链
    for (int i = 0; i < m; i++) {
        h ^= static_cast<uint8_t>(tail[i]);
        h *= 16777619u;
    }
    return h;
}

std::vector<BookInfo> ScanBooks() {
    std::vector<BookInfo> books;
    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGW(TAG, "SD not mounted");
        return books;
    }
    EnsureDirs();

    // 先收集文件名再统一 stat：避免在 readdir 循环内 stat 破坏 FATFS 目录句柄。
    DIR* dir = opendir(kBooksDir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "opendir %s failed", kBooksDir);
        return books;
    }
    std::vector<std::string> names;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;  // 跳过 . / .. / .metalio
        BookFormat fmt;
        size_t ext_len;
        if (!MatchBookSuffix(ent->d_name, &fmt, &ext_len)) continue;
        names.emplace_back(ent->d_name);
    }
    closedir(dir);

    char path[kNameMax + 32];
    for (const auto& fname : names) {
        snprintf(path, sizeof(path), "%s/%s", kBooksDir, fname.c_str());
        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        BookFormat fmt;
        size_t ext_len;
        if (!MatchBookSuffix(fname.c_str(), &fmt, &ext_len)) continue;

        BookInfo b;
        b.name = fname.substr(0, fname.size() - ext_len);
        b.path = path;
        b.size = static_cast<uint32_t>(st.st_size);
        b.hash = BookKeyHash(b.name, b.size);
        b.format = fmt;
        b.progress_pct = LoadProgressPct(b.hash, fmt, b.size);
        books.push_back(std::move(b));
    }

    std::sort(books.begin(), books.end(),
              [](const BookInfo& a, const BookInfo& c) { return a.name < c.name; });
    ESP_LOGI(TAG, "scanned %d books", static_cast<int>(books.size()));
    return books;
}

uint32_t LoadProgress(uint32_t hash) {
    char key[16];
    ProgressKey('b', hash, key, sizeof(key));
    Settings s(kNvsNs, false);
    return static_cast<uint32_t>(s.GetInt(key, 0));
}

void SaveProgress(uint32_t hash, uint32_t off, int pct) {
    char key[16];
    Settings s(kNvsNs, true);
    ProgressKey('b', hash, key, sizeof(key));
    s.SetInt(key, static_cast<int32_t>(off));
    ProgressKey('p', hash, key, sizeof(key));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s.SetInt(key, pct);
}

int LoadProgressPct(uint32_t hash, BookFormat format, uint32_t file_size) {
    char key[16];
    Settings s(kNvsNs, false);
    ProgressKey('p', hash, key, sizeof(key));
    int pct = s.GetInt(key, -1);
    if (pct >= 0) return pct > 100 ? 100 : pct;
    // 老数据（只有偏移）：TXT 可用 off/文件大小 估算；EPUB 偏移是虚拟域，无法换算。
    ProgressKey('b', hash, key, sizeof(key));
    uint32_t off = static_cast<uint32_t>(s.GetInt(key, 0));
    if (off == 0) return -1;
    if (format == BookFormat::kTxt && file_size > 0) {
        uint64_t p = static_cast<uint64_t>(off) * 100 / file_size;
        return static_cast<int>(p > 100 ? 100 : p);
    }
    return 0;  // EPUB 读过但无 pct：显示 0%，下次落盘即修正
}

ReaderSettings LoadSettings() {
    Settings s(kNvsNs, false);
    ReaderSettings r;
    r.font_idx = static_cast<uint8_t>(s.GetInt("fs", 1));
    r.theme_idx = static_cast<uint8_t>(s.GetInt("th", 1));
    r.line_space_idx = static_cast<uint8_t>(s.GetInt("ls", 1));
    r.margin_idx = static_cast<uint8_t>(s.GetInt("mg", 1));
    std::string face = s.GetString("ff", "");
    strlcpy(r.font_face, face.c_str(), sizeof(r.font_face));
    return r;
}

void SaveSettings(const ReaderSettings& r) {
    Settings s(kNvsNs, true);
    s.SetInt("fs", r.font_idx);
    s.SetInt("th", r.theme_idx);
    s.SetInt("ls", r.line_space_idx);
    s.SetInt("mg", r.margin_idx);
    s.SetString("ff", r.font_face);
}

std::string LoadLastBook() {
    Settings s(kNvsNs, false);
    return s.GetString("last", "");
}

void SaveLastBook(const std::string& path) {
    Settings s(kNvsNs, true);
    s.SetString("last", path);
}

}  // namespace book_store
