// ebook_worker.cc — 见头文件说明。

#include "ebook_worker.h"

#include "book_source.h"
#include "book_store.h"
#include "epub_source.h"
#include "paginator.h"

#include "esp_lv_adapter.h"
#include "esp_log.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>  // xTaskCreatePinnedToCoreWithCaps（PSRAM 栈）
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace ebook_worker {
namespace {

constexpr char TAG[] = "ebook_worker";
constexpr UBaseType_t kQueueSize = 4;

enum CmdKind : uint8_t { CMD_OPEN = 0, CMD_LOAD = 1, CMD_COVER = 2, CMD_FENCE = 3 };

// 值传递命令（xQueueSend memcpy）。PageMetrics.font 指向内置 const 字体（永生只读安全）或
// 用户 FT 字体（由 ebook_font 的 fence/graveyard 保证：本命令处理期间字体必在世；对 FT 字体
// 测宽的线程安全由 lv_freetype 内部 face_lock 承担）。
struct Cmd {
    CmdKind kind;
    uint32_t session;
    // open
    BookReq book;
    // load
    int chapter_idx;
    PageMetrics metrics;
    TargetMode target_mode;
    uint32_t target_file_off;
    uint8_t tag;
    // fence
    uint32_t fence_id;
};

QueueHandle_t s_q = nullptr;
TaskHandle_t s_task = nullptr;
Callbacks s_cb;
std::atomic<uint32_t> s_session{0};

// worker 线程私有状态：当前打开的书 + 章节索引。
BookInfo s_book;
ChapterIndex s_index;
bool s_have_index = false;

bool LockAndValid(uint32_t cmd_session) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return false;
    if (cmd_session != s_session.load(std::memory_order_acquire)) {
        esp_lv_adapter_unlock();
        return false;
    }
    return true;
}

void ProcessOpen(const Cmd& cmd) {
    s_have_index = false;
    s_book.name = cmd.book.name;
    s_book.path = cmd.book.path;
    s_book.size = cmd.book.size;
    s_book.hash = cmd.book.hash;
    s_book.format = cmd.book.format;

    bool ok = book_source::OpenBook(s_book, s_index);
    s_have_index = ok;
    if (LockAndValid(cmd.session)) {
        if (s_cb.on_book_ready) s_cb.on_book_ready(ok ? &s_index : nullptr, ok);
        esp_lv_adapter_unlock();
    }
}

void ProcessLoad(const Cmd& cmd) {
    PaginatedChapter* pc = nullptr;
    bool ok = false;
    int target_page = 0;

    if (s_have_index && cmd.chapter_idx >= 0 &&
        cmd.chapter_idx < static_cast<int>(s_index.chapters.size())) {
        pc = new (std::nothrow) PaginatedChapter();
        if (pc != nullptr && book_source::LoadChapter(s_book, s_index, cmd.chapter_idx, *pc)) {
            // EPUB 插图：分页前解码并定显示尺寸（图片行高参与组页）。TXT images 为空，无副作用。
            epub_source::PrepareImages(s_book, *pc, cmd.metrics);
            paginator::Paginate(*pc, cmd.metrics);
            strlcpy(pc->title, s_index.chapters[cmd.chapter_idx].title, sizeof(pc->title));
            switch (cmd.target_mode) {
                case kTargetFirst: target_page = 0; break;
                case kTargetLast:
                    target_page = pc->pages.empty() ? 0 : static_cast<int>(pc->pages.size()) - 1;
                    break;
                default: target_page = pc->FindPageByFileOffset(cmd.target_file_off); break;
            }
            ok = true;
        }
    }

    if (LockAndValid(cmd.session)) {
        if (s_cb.on_chapter_ready) {
            s_cb.on_chapter_ready(ok ? pc : nullptr, target_page, ok, cmd.tag);
            pc = nullptr;  // 成功回调后所有权转移给 UI
        }
        esp_lv_adapter_unlock();
    }
    if (pc != nullptr) {  // 未回调（session 失效）或失败：worker 自行释放
        pc->FreeBuf();
        delete pc;
    }
}

// ---- 书架封面（.cov 缓存：LVGL RGB888(BGR) 原始像素 + 小头；w==0 为"无封面"标记）----

struct CovHeader {
    char magic[4];  // 'M','C','O','V'
    uint16_t ver;
    uint16_t w;
    uint16_t h;
    uint16_t reserved;
};
constexpr uint16_t kCovVersion = 1;

void CovPath(uint32_t hash, char* buf, size_t n) {
    snprintf(buf, n, "%s/%08lx.cov", book_store::kSidecarDir, static_cast<unsigned long>(hash));
}

// 读 .cov 缓存。返回 true 表示缓存命中（*px 可能为 null = 无封面标记）。
bool LoadCoverCache(uint32_t hash, uint8_t** px, uint16_t* w, uint16_t* h) {
    char path[64];
    CovPath(hash, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    CovHeader hd;
    if (fread(&hd, sizeof(hd), 1, f) != 1 || memcmp(hd.magic, "MCOV", 4) != 0 ||
        hd.ver != kCovVersion || hd.w > kCoverBoxW || hd.h > kCoverBoxH) {
        fclose(f);
        return false;
    }
    if (hd.w == 0 || hd.h == 0) {  // 无封面标记
        fclose(f);
        *px = nullptr;
        *w = *h = 0;
        return true;
    }
    size_t n = static_cast<size_t>(hd.w) * hd.h * 3;
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) buf = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_8BIT));
    if (buf == nullptr || fread(buf, 1, n, f) != n) {
        if (buf != nullptr) heap_caps_free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *px = buf;
    *w = hd.w;
    *h = hd.h;
    return true;
}

void SaveCoverCache(uint32_t hash, const uint8_t* px, uint16_t w, uint16_t h) {
    char path[64];
    CovPath(hash, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return;
    CovHeader hd = {};
    memcpy(hd.magic, "MCOV", 4);
    hd.ver = kCovVersion;
    hd.w = w;
    hd.h = h;
    fwrite(&hd, sizeof(hd), 1, f);
    if (px != nullptr && w > 0 && h > 0) fwrite(px, 1, static_cast<size_t>(w) * h * 3, f);
    fclose(f);
}

void ProcessCover(const Cmd& cmd) {
    uint8_t* px = nullptr;
    uint16_t w = 0, h = 0;
    if (!LoadCoverCache(cmd.book.hash, &px, &w, &h)) {
        if (cmd.book.format == BookFormat::kEpub) {
            BookInfo b;
            b.name = cmd.book.name;
            b.path = cmd.book.path;
            b.size = cmd.book.size;
            b.hash = cmd.book.hash;
            b.format = cmd.book.format;
            px = epub_source::ExtractCover(b, kCoverBoxW, kCoverBoxH, &w, &h);
            if (px == nullptr) w = h = 0;
        }
        SaveCoverCache(cmd.book.hash, px, w, h);  // 失败也写"无封面"标记，避免每次重解析
    }
    if (LockAndValid(cmd.session)) {
        if (s_cb.on_cover_ready) {
            s_cb.on_cover_ready(cmd.book.hash, px, w, h);
            px = nullptr;  // 所有权转移给 UI
        }
        esp_lv_adapter_unlock();
    }
    if (px != nullptr) heap_caps_free(px);
}

// FIFO 屏障：到达即回调（不校验 session —— 退休字体无论如何都要释放）。
void ProcessFence(const Cmd& cmd) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    if (s_cb.on_fence) s_cb.on_fence(cmd.fence_id);
    esp_lv_adapter_unlock();
}

void WorkerRun(void*) {
    Cmd cmd;
    for (;;) {
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.kind) {
            case CMD_OPEN: ProcessOpen(cmd); break;
            case CMD_LOAD: ProcessLoad(cmd); break;
            case CMD_COVER: ProcessCover(cmd); break;
            case CMD_FENCE: ProcessFence(cmd); break;
            default: break;
        }
    }
}

bool Enqueue(Cmd& cmd) {
    if (s_q == nullptr) return false;
    cmd.session = s_session.load(std::memory_order_acquire);
    return xQueueSend(s_q, &cmd, 0) == pdTRUE;
}

}  // namespace

void Begin(const Callbacks& cb) {
    s_cb = cb;
    BumpSession();
    if (s_q != nullptr) return;  // 幂等：worker 常驻
    s_q = xQueueCreate(kQueueSize, sizeof(Cmd));
    if (s_q == nullptr) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    // 栈放 PSRAM 并加大到 48KB：分页对用户 FreeType 字体测宽时，OTF/CFF 的 Adobe charstring
    // 解释器(cf2_*)要吃 ~18-28KB 栈，原 16KB 内部栈会溢出崩溃。放 PSRAM 不占内部 RAM
    // （CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y）。worker 常驻不删除，无需 WithCaps 清理。
    if (xTaskCreatePinnedToCoreWithCaps(WorkerRun, "ebook_wk", 49152, nullptr, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

uint32_t BumpSession() { return s_session.fetch_add(1, std::memory_order_acq_rel) + 1; }

bool EnqueueOpenBook(const BookReq& book) {
    Cmd cmd = {};
    cmd.kind = CMD_OPEN;
    cmd.book = book;
    return Enqueue(cmd);
}

bool EnqueueLoadChapter(int chapter_idx, const PageMetrics& m, TargetMode mode,
                        uint32_t target_file_off, uint8_t tag) {
    Cmd cmd = {};
    cmd.kind = CMD_LOAD;
    cmd.chapter_idx = chapter_idx;
    cmd.metrics = m;
    cmd.target_mode = mode;
    cmd.target_file_off = target_file_off;
    cmd.tag = tag;
    return Enqueue(cmd);
}

bool EnqueueCover(const BookReq& book) {
    Cmd cmd = {};
    cmd.kind = CMD_COVER;
    cmd.book = book;
    return Enqueue(cmd);
}

bool EnqueueFence(uint32_t fence_id) {
    Cmd cmd = {};
    cmd.kind = CMD_FENCE;
    cmd.fence_id = fence_id;
    return Enqueue(cmd);
}

}  // namespace ebook_worker
