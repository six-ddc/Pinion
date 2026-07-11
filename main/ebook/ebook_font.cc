// ebook_font.cc — 见头文件说明。

#include "ebook_font.h"

#include "book_store.h"
#include "font_mem_vfs.h"

#include "sdkconfig.h"

#include "SdCardManager.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#if defined(CONFIG_LV_USE_FREETYPE) && defined(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE)
#define EBOOK_FONT_FT 1
#include "esp_lv_adapter.h"
#else
#define EBOOK_FONT_FT 0
#endif

namespace ebook_font {

namespace {

constexpr char TAG[] = "ebook_font";

bool SuffixIs(const char* name, size_t n, const char* ext) {
    size_t el = strlen(ext);
    if (n <= el) return false;
    return strcasecmp(name + n - el, ext) == 0;
}

bool IsFontName(const char* name, size_t* ext_len) {
    size_t n = strlen(name);
    if (SuffixIs(name, n, ".ttf")) {
        *ext_len = 4;
        return true;
    }
    if (SuffixIs(name, n, ".otf")) {
        *ext_len = 4;
        return true;
    }
    return false;
}

#if EBOOK_FONT_FT

// 读入 PSRAM 的字体文件预算：要求最大连续空闲 ≥ 文件大小 + 该余量，否则降级 SD 直连。
// 余量兜底"换字体瞬间新旧 blob 并存 + 字形位图缓存 + 其它 app"。
constexpr size_t kBudgetReserve = 4 * 1024 * 1024;
constexpr size_t kReadChunk = 64 * 1024;

// PSRAM 内存字体块（一个 SD 字体文件读进内存的一份拷贝）。body/head 两个尺寸共用同一
// FT_Face（lv_freetype 按 pathname 匹配 cache_node），故一个 blob 服务两个字体实例。
struct Blob {
    char src_file[64] = {0};  // SD 文件名（用于同文件复用判定）
    char vfs_name[16] = {0};  // 代次唯一 "gN.ttf"（规避 lv_freetype 同名复用旧 face）
    uint8_t* data = nullptr;  // PSRAM
    size_t size = 0;
    int refs = 0;  // 引用它的 adapter 字体句柄数（body+head 各 +1，随 grave 释放递减）
};
std::vector<Blob*> s_blobs;  // 在世 blob（现役 + 退休中），同时 ≤2
uint32_t s_blob_counter = 0;

// 已激活的 FT 字体（正文 + 标题）。
esp_lv_adapter_ft_font_handle_t s_body_h = nullptr;
esp_lv_adapter_ft_font_handle_t s_head_h = nullptr;
const lv_font_t* s_body = nullptr;
const lv_font_t* s_head = nullptr;
bool s_active = false;
Blob* s_cur_blob = nullptr;  // 当前字体所用 blob；nullptr = 降级 SD 直连（或未激活）
bool s_degraded = false;     // 当前字体走 SD 直连（blob 装不下）
bool s_want_upgrade = false;  // 降级仅因新旧 blob 并存挤占 → 待 graveyard 排空后可重试升级
char s_cur_file[64] = {0};
int s_cur_body_px = 0;
int s_cur_head_px = 0;

uint32_t s_fence_counter = 0;

// 退休字体（等 worker fence 到达再 deinit）。blob 为其所用 PSRAM 块（nullptr = 降级 SD）。
struct Grave {
    esp_lv_adapter_ft_font_handle_t body;
    esp_lv_adapter_ft_font_handle_t head;
    Blob* blob;
    uint32_t fence;
};
std::vector<Grave> s_graveyard;

struct LockGuard {
    bool ok;
    LockGuard() { ok = (esp_lv_adapter_lock(-1) == ESP_OK); }
    ~LockGuard() {
        if (ok) esp_lv_adapter_unlock();
    }
};

Blob* FindBlobBySrc(const char* filename) {
    for (Blob* b : s_blobs) {
        if (strcmp(b->src_file, filename) == 0) return b;
    }
    return nullptr;
}

void DestroyBlob(Blob* b) {
    font_mem_vfs::Remove(b->vfs_name);
    if (b->data != nullptr) heap_caps_free(b->data);
    s_blobs.erase(std::remove(s_blobs.begin(), s_blobs.end(), b), s_blobs.end());
    delete b;
}

// 把 SD 字体整读进 PSRAM 并登记到 /fmem。预算不足 / 分配失败 / 读失败 → nullptr（调用方降级）。
// 成功返回的 blob 已入 s_blobs 且 refs==0（尚未被字体引用）。
Blob* LoadBlob(const char* filename) {
    font_mem_vfs::Register();  // 幂等
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", book_store::kBooksDir, filename);
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0) return nullptr;
    size_t size = static_cast<size_t>(st.st_size);

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largest < size + kBudgetReserve) {
        ESP_LOGW(TAG, "font %s (%u KB) too big for PSRAM (largest free %u KB) -> SD fallback",
                 filename, static_cast<unsigned>(size / 1024),
                 static_cast<unsigned>(largest / 1024));
        return nullptr;
    }
    uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (data == nullptr) {
        ESP_LOGW(TAG, "PSRAM malloc %u KB failed -> SD fallback", static_cast<unsigned>(size / 1024));
        return nullptr;
    }
    int64_t t0 = esp_timer_get_time();
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        heap_caps_free(data);
        return nullptr;
    }
    size_t got = 0;
    while (got < size) {
        size_t want = size - got;
        if (want > kReadChunk) want = kReadChunk;
        size_t n = fread(data + got, 1, want, fp);
        if (n == 0) break;
        got += n;
    }
    fclose(fp);
    if (got != size) {
        ESP_LOGW(TAG, "read %s short (%u/%u) -> SD fallback", filename, static_cast<unsigned>(got),
                 static_cast<unsigned>(size));
        heap_caps_free(data);
        return nullptr;
    }

    Blob* b = new Blob();
    strlcpy(b->src_file, filename, sizeof(b->src_file));
    snprintf(b->vfs_name, sizeof(b->vfs_name), "g%u.ttf", static_cast<unsigned>(++s_blob_counter));
    b->data = data;
    b->size = size;
    b->refs = 0;
    if (!font_mem_vfs::Add(b->vfs_name, b->data, b->size)) {
        heap_caps_free(data);
        delete b;
        return nullptr;
    }
    s_blobs.push_back(b);
    ESP_LOGI(TAG, "loaded %s -> %s (%u KB) into PSRAM in %lld ms; SPIRAM free %u KB", filename,
             b->vfs_name, static_cast<unsigned>(size / 1024),
             (esp_timer_get_time() - t0) / 1000,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return b;
}

// 创建单个 FT 字体实例（常规体，bitmap 模式）。失败返回 nullptr。
const lv_font_t* CreateOne(const char* path, int px, esp_lv_adapter_ft_font_handle_t* out_h) {
    esp_lv_adapter_ft_font_config_t cfg =
        ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(path, static_cast<uint16_t>(px),
                                           ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    esp_lv_adapter_ft_font_handle_t h = nullptr;
    if (esp_lv_adapter_ft_font_init(&cfg, &h) != ESP_OK || h == nullptr) return nullptr;
    const lv_font_t* f = esp_lv_adapter_ft_font_get(h);
    if (f == nullptr) {
        esp_lv_adapter_ft_font_deinit(h);
        return nullptr;
    }
    *out_h = h;
    return f;
}

// 真正激活（已在锁内、已确认要变更）。成功置 s_active=true。
bool DoActivate(const char* filename, int body_px, int head_px, const lv_font_t* body_fb,
                const lv_font_t* head_fb) {
    // 选路径：优先复用同文件在世 blob（换字号零重开销）；否则新读 blob；再否则降级 SD 直连。
    Blob* reuse = FindBlobBySrc(filename);
    Blob* fresh = nullptr;   // 本次新建的 blob（失败需回滚）
    bool degraded = false;
    bool other_alive = !s_blobs.empty();  // 是否已有别的 blob（判断降级是否因并存挤占）
    char path[128];
    Blob* blob = reuse;
    if (blob != nullptr) {
        font_mem_vfs::PathFor(blob->vfs_name, path, sizeof(path));
    } else {
        fresh = LoadBlob(filename);
        blob = fresh;
        if (blob != nullptr) {
            font_mem_vfs::PathFor(blob->vfs_name, path, sizeof(path));
        } else {
            snprintf(path, sizeof(path), "%s/%s", book_store::kBooksDir, filename);  // SD 直连
            degraded = true;
        }
    }

    esp_lv_adapter_ft_font_handle_t bh = nullptr, hh = nullptr;
    const lv_font_t* body = CreateOne(path, body_px, &bh);
    if (body == nullptr) {
        ESP_LOGW(TAG, "activate body failed: %s", path);
        if (fresh != nullptr && fresh->refs == 0) DestroyBlob(fresh);
        return false;
    }
    const lv_font_t* head = CreateOne(path, head_px, &hh);
    if (head == nullptr) {
        esp_lv_adapter_ft_font_deinit(bh);
        ESP_LOGW(TAG, "activate heading failed: %s", path);
        if (fresh != nullptr && fresh->refs == 0) DestroyBlob(fresh);
        return false;
    }
    // 缺字兜底：FT 字体没有的字形自动落到对应内置点阵字体（测宽/渲染均跟随 fallback 链）。
    const_cast<lv_font_t*>(body)->fallback = body_fb;
    const_cast<lv_font_t*>(head)->fallback = head_fb;

    if (blob != nullptr) blob->refs += 2;  // body + head 各引用 blob 一次

    s_body_h = bh;
    s_head_h = hh;
    s_body = body;
    s_head = head;
    s_active = true;
    s_cur_blob = blob;
    s_degraded = degraded;
    s_want_upgrade = degraded && other_alive;  // 仅并存挤占导致的降级值得重试
    strlcpy(s_cur_file, filename, sizeof(s_cur_file));
    s_cur_body_px = body_px;
    s_cur_head_px = head_px;
    ESP_LOGI(TAG, "activated %s @ %d/%d (%s)", filename, body_px, head_px,
             degraded ? "SD fallback" : "PSRAM");
    return true;
}

// 把当前激活字体挪进 graveyard，返回新 fence id。调用前须持锁且 s_active。
uint32_t RetireCurrentLocked() {
    uint32_t fence = ++s_fence_counter;
    s_graveyard.push_back({s_body_h, s_head_h, s_cur_blob, fence});
    s_body_h = s_head_h = nullptr;
    s_body = s_head = nullptr;
    s_active = false;
    s_cur_blob = nullptr;
    s_degraded = false;
    s_want_upgrade = false;
    s_cur_file[0] = '\0';
    s_cur_body_px = s_cur_head_px = 0;
    return fence;
}
#endif  // EBOOK_FONT_FT

}  // namespace

void ScanFonts(std::vector<FontFile>& out) {
    out.clear();
    if (!SdCardManager::GetInstance().IsMounted()) return;
    DIR* dir = opendir(book_store::kBooksDir);
    if (dir == nullptr) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        size_t ext_len;
        if (!IsFontName(ent->d_name, &ext_len)) continue;
        FontFile f;
        f.filename = ent->d_name;
        f.display = f.filename.substr(0, f.filename.size() - ext_len);
        out.push_back(std::move(f));
    }
    closedir(dir);
    std::sort(out.begin(), out.end(),
              [](const FontFile& a, const FontFile& b) { return a.display < b.display; });
}

bool Exists(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') return false;
    size_t ext_len;
    if (!IsFontName(filename, &ext_len)) return false;
    if (!SdCardManager::GetInstance().IsMounted()) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", book_store::kBooksDir, filename);
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

#if EBOOK_FONT_FT

uint32_t Configure(const char* filename, int body_px, int head_px, const lv_font_t* body_fallback,
                   const lv_font_t* head_fallback) {
    bool want = (filename != nullptr && filename[0] != '\0');
    LockGuard g;
    if (!g.ok) return 0;

    // 已是目标配置且未降级：无操作。降级态下即便配置相同也允许重激活（升级到 PSRAM）。
    if (want && s_active && !s_degraded && strcmp(s_cur_file, filename) == 0 &&
        s_cur_body_px == body_px && s_cur_head_px == head_px) {
        return 0;
    }
    if (!want && !s_active) return 0;

    uint32_t fence = 0;
    if (s_active) fence = RetireCurrentLocked();
    if (want) DoActivate(filename, body_px, head_px, body_fallback, head_fallback);
    return fence;
}

bool Active() { return s_active; }
const lv_font_t* BodyFont() { return s_body; }
const lv_font_t* HeadingFont() { return s_head; }

// 降级态、且并无其它 blob 与 graveyard 占用 PSRAM（预算恢复到最大）→ 值得重试升级为 PSRAM 字体。
bool ShouldUpgrade() {
    return s_active && s_degraded && s_want_upgrade && s_graveyard.empty();
}

void OnWorkerFence(uint32_t fence_id) {
    LockGuard g;
    if (!g.ok) return;
    for (auto it = s_graveyard.begin(); it != s_graveyard.end();) {
        if (it->fence <= fence_id) {
            // 先 deinit 字体句柄（最后一个引用该 pathname 时触发 FT_Done_Face → fclose 我们的 fd），
            // 再递减 blob refs、归 0 时销毁 blob —— 保证 blob 严格晚于 FT_Face 释放。
            if (it->body) esp_lv_adapter_ft_font_deinit(it->body);
            if (it->head) esp_lv_adapter_ft_font_deinit(it->head);
            Blob* b = it->blob;
            if (b != nullptr) {
                b->refs -= 2;
                if (b->refs <= 0) {
                    ESP_LOGI(TAG, "free blob %s; SPIRAM free %u KB", b->vfs_name,
                             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                    DestroyBlob(b);
                }
            }
            it = s_graveyard.erase(it);
        } else {
            ++it;
        }
    }
}

#else  // FreeType 未启用：纯内置字体降级。

uint32_t Configure(const char*, int, int, const lv_font_t*, const lv_font_t*) { return 0; }
bool Active() { return false; }
const lv_font_t* BodyFont() { return nullptr; }
const lv_font_t* HeadingFont() { return nullptr; }
bool ShouldUpgrade() { return false; }
void OnWorkerFence(uint32_t) {}

#endif  // EBOOK_FONT_FT

}  // namespace ebook_font
