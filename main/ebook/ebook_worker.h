// ebook_worker.h
// 常驻后台 worker：把文件 IO / 编码转换 / 分页隔离到独立 FreeRTOS 任务（core0），
// LVGL 线程只值传递命令入队、不阻塞。结果由 worker 在 esp_lv_adapter_lock 临界区、
// 校验 session 后回调进 UI（照抄 stock_fetch_worker 范式）。
//
// 铁律：worker 里除回调那一小段（已持 LVGL 锁）外，绝不可调 lv_*。例外：分页测宽
// lv_font_get_glyph_width 是安全的 —— 内置字体是永生 const 数据；用户 FT 字体由 lv_freetype
// 的内部 face_lock 串行化度量与渲染。故分页无需外层持锁（字体在世由 fence/graveyard 保证）。

#ifndef EBOOK_WORKER_H
#define EBOOK_WORKER_H

#include "ebook_models.h"

#include <cstdint>

namespace ebook_worker {

// 目标页定位模式（LoadChapter）。
enum TargetMode : uint8_t {
    kTargetByOffset = 0,  // 按 target_file_off 定位（重分页/续读）
    kTargetFirst = 1,     // 章首（下一章正向翻入）
    kTargetLast = 2,      // 章尾（上一章反向翻入）
};

// 打开书的请求（值传递；BookInfo 含 std::string 不能过队列）。
struct BookReq {
    char path[256];
    char name[128];
    uint32_t size;
    uint32_t hash;
    BookFormat format;
};

// 书架封面缩略图盒尺寸（生成与展示共用；实际输出等比 ≤ 此盒）。
constexpr uint16_t kCoverBoxW = 210;
constexpr uint16_t kCoverBoxH = 280;

// on_book_ready：idx 在回调期间有效（worker 持有），UI 需在回调内拷走所需内容。
typedef void (*BookReadyCb)(const ChapterIndex* idx, bool ok);
// on_chapter_ready：pc 所有权转移给 UI（成功时非空，UI 负责 FreeBuf + delete）。
// target_page 为 worker 依 TargetMode 算好的目标页下标。tag 为宿主入队时的透传标签
// （区分 当前章 / 预载上一章 / 预载下一章）。
typedef void (*ChapterReadyCb)(PaginatedChapter* pc, int target_page, bool ok, uint8_t tag);
// on_cover_ready：书架封面就绪。px（LVGL RGB888 布局，w*h*3）所有权转移给 UI，
// heap_caps_free 释放；px==nullptr 表示该书无封面/生成失败（UI 保留生成式封面）。
// 每个 EnqueueCover 恰好回调一次（session 有效时），UI 借此驱动"逐本串行"生成链。
typedef void (*CoverReadyCb)(uint32_t hash, uint8_t* px, uint16_t w, uint16_t h);
// on_fence：FIFO 屏障命令到达（EnqueueFence 入队的 fence_id）。**不校验 session**（即便切书/
// 卸载也必须执行，否则退休字体泄漏）。宿主借此在"所有引用旧 FT 字体的 LOAD 命令已排空"后安全
// 释放旧字体（见 ebook_font::OnWorkerFence）。回调在 worker 线程、持 LVGL 锁下调用。
typedef void (*FenceCb)(uint32_t fence_id);

struct Callbacks {
    BookReadyCb on_book_ready = nullptr;
    ChapterReadyCb on_chapter_ready = nullptr;
    CoverReadyCb on_cover_ready = nullptr;
    FenceCb on_fence = nullptr;
};

// 创建 queue + task（幂等，常驻）。每次进 screen 调用以登记回调并 BumpSession。
void Begin(const Callbacks& cb);

// 递增 session，作废在途结果（screen 卸载/切书时调）。
uint32_t BumpSession();

// 入队（LVGL 线程调用）。队列满返回 false。
bool EnqueueOpenBook(const BookReq& book);
bool EnqueueLoadChapter(int chapter_idx, const PageMetrics& m, TargetMode mode,
                        uint32_t target_file_off, uint8_t tag);
// 生成/读取一本书的封面缩略（EPUB；有 .cov 缓存则直读）。结果回 on_cover_ready。
bool EnqueueCover(const BookReq& book);
// FIFO 屏障：待此命令前的所有命令处理完，回 on_fence(fence_id)（用于安全释放旧 FT 字体）。
bool EnqueueFence(uint32_t fence_id);

}  // namespace ebook_worker

#endif  // EBOOK_WORKER_H
