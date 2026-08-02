// pi_media_library.cc — 见头文件。

#include "pi_media_library.h"

#include <dirent.h>
#include <strings.h>  // strcasecmp
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "esp_log.h"

#include "metalio_hal/storage.h"

namespace pi_media_library {
namespace {

constexpr char TAG[] = "pi_media_lib";

// 递归深度钳（FAT 无软链，纯防御病态深树）。
constexpr int kMaxDepth = 8;

// meta 缓存：path → 已读出的 ID3 信息 + 读取时的文件大小（有效性判据）。
struct CachedMeta {
    std::string title;
    std::string subtitle;
    int duration_s = 0;
    long size = -1;
};
std::mutex s_cache_mu;
std::unordered_map<std::string, CachedMeta> s_cache;

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool HasMp3Ext(const char* name) {
    size_t n = std::strlen(name);
    if (n < 4) return false;
    const char* e = name + n - 4;
    return e[0] == '.' && LowerAscii(e[1]) == 'm' && LowerAscii(e[2]) == 'p' && e[3] == '3';
}

// 扫描中间产物：排序键（目录、文件名）与 stat 出的大小（缓存判据，省二次 stat）。
struct RawTrack {
    std::string dir;
    std::string name;
    long size;
};

void ScanDir(const std::string& dir, int depth, std::vector<RawTrack>& out) {
    if (depth > kMaxDepth || out.size() >= kLibraryMax) return;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;  // 目录不存在/无权限：跳过
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && out.size() < kLibraryMax) {
        const char* name = ent->d_name;
        if (name[0] == '.') continue;  // 跳过 "." ".." 及隐藏项
        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ScanDir(full, depth + 1, out);
        } else if (S_ISREG(st.st_mode) && HasMp3Ext(name)) {
            out.push_back({dir, name, static_cast<long>(st.st_size)});
        }
    }
    closedir(d);
}

// 早退版：找到第一个 .mp3 即返回 true。
bool AnyIn(const std::string& dir, int depth) {
    if (depth > kMaxDepth) return false;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return false;
    struct dirent* ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        if (name[0] == '.') continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            found = AnyIn(full, depth + 1);
        } else if (S_ISREG(st.st_mode) && HasMp3Ext(name)) {
            found = true;
        }
    }
    closedir(d);
    return found;
}

// 缓存查询：命中且 size 未变则回填 item 并置 meta_filled。
bool CacheApply(media::MediaItem& item, long size) {
    std::lock_guard<std::mutex> lk(s_cache_mu);
    auto it = s_cache.find(item.path_or_url);
    if (it == s_cache.end() || it->second.size != size) return false;
    if (!it->second.title.empty()) item.title = it->second.title;
    if (!it->second.subtitle.empty()) item.subtitle = it->second.subtitle;
    if (it->second.duration_s > 0) item.duration_s = it->second.duration_s;
    item.meta_filled = true;
    return true;
}

}  // namespace

std::vector<media::MediaItem> ScanLibrary() {
    const std::string root = mhal::storage::GetMountPoint();
    std::vector<RawTrack> raw;
    ScanDir(root + "/Music", 0, raw);
    ScanDir(root + "/Podcasts", 0, raw);
    if (raw.size() >= kLibraryMax) {
        ESP_LOGW(TAG, "library truncated at %u tracks (kLibraryMax)", (unsigned)kLibraryMax);
    }
    // 目录成组：父目录全路径优先、同目录内按文件名（strcasecmp 折叠 ASCII 大小写，
    // 中文按 UTF-8 码点序——非拼音，但稳定且目录成组即满足连播语义）。
    std::sort(raw.begin(), raw.end(), [](const RawTrack& a, const RawTrack& b) {
        int c = strcasecmp(a.dir.c_str(), b.dir.c_str());
        if (c != 0) return c < 0;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    std::vector<media::MediaItem> items;
    items.reserve(raw.size());
    for (const RawTrack& t : raw) {
        media::MediaItem m;
        m.path_or_url = t.dir + "/" + t.name;
        m.title = BaseNoExt(t.name);
        m.subtitle = ParentDirName(m.path_or_url);
        m.is_stream = false;
        CacheApply(m, t.size);  // 搜过的歌直接带真名；其余文件名兜底、播放时惰性补
        items.push_back(std::move(m));
    }
    return items;
}

int IndexOfPath(const std::vector<media::MediaItem>& items, const std::string& path) {
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].path_or_url == path) return static_cast<int>(i);
    }
    return -1;
}

bool HasAnyTrack() {
    const std::string root = mhal::storage::GetMountPoint();
    return AnyIn(root + "/Music", 0) || AnyIn(root + "/Podcasts", 0);
}

void ApplyId3(media::MediaItem& item) {
    if (item.meta_filled || item.is_stream) return;
    struct stat st;
    long size = (::stat(item.path_or_url.c_str(), &st) == 0) ? static_cast<long>(st.st_size) : -1;
    if (CacheApply(item, size)) return;
    media::FillItemMetaFromId3(item);
    std::lock_guard<std::mutex> lk(s_cache_mu);
    s_cache[item.path_or_url] = {item.title, item.subtitle, item.duration_s, size};
}

std::string BaseNoExt(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

std::string ParentDirName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return {};
    size_t p = path.find_last_of('/', slash - 1);
    return (p == std::string::npos) ? path.substr(0, slash) : path.substr(p + 1, slash - p - 1);
}

}  // namespace pi_media_library
