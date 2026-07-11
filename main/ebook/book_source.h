// book_source.h
// 书籍文件访问层的统一入口：按 BookInfo.format 分发到 TXT（本文件实现）或 EPUB
// （epub_source）。TXT 路径：编码检测（UTF-8 / GBK）、章节索引（流式扫描 + sidecar
// 缓存）、章节正文载入为 UTF-8。纯逻辑，无 lv_obj。跑在 ebook_worker 线程（文件 IO 较重）。

#ifndef BOOK_SOURCE_H
#define BOOK_SOURCE_H

#include "ebook_models.h"

namespace book_source {

// 检测 TXT 正文编码：有 UTF-8 BOM 直接判 UTF-8（*bom_skip=3）；否则读前若干 KB 做严格
// UTF-8 校验，通过判 UTF-8，失败回退 GBK。*bom_skip 为需跳过的前导字节数。
TextEncoding DetectEncoding(const BookInfo& book, uint32_t* bom_skip);

// 打开书（按 format 分发）：优先读 sidecar 缓存（校验 size+hash）；失效或缺失则
// 重建并写回。成功填 out（encoding / virt_size / chapters / has_real_chapters）。
bool OpenBook(const BookInfo& book, ChapterIndex& out);

// 载入第 chapter_idx 章正文为 UTF-8 到 PSRAM（按 format 分发），填 out.utf8_buf/len/
// encoding/chapter_file_start/chapter_idx（不分页；EPUB 另填 images 旁表）。失败返回 false。
bool LoadChapter(const BookInfo& book, const ChapterIndex& idx, int chapter_idx,
                 PaginatedChapter& out);

// ---- sidecar 缓存（TXT / EPUB 共用；文件名由 book.hash 决定，格式版本见 .cc）----
// extractor_ver：内容抽取算法版本（EPUB 用，算法升级时使旧缓存失效；TXT 传 0）。
bool LoadIndexCache(const BookInfo& book, ChapterIndex& out, uint8_t extractor_ver = 0);
void SaveIndexCache(const BookInfo& book, const ChapterIndex& idx, uint8_t extractor_ver = 0);

}  // namespace book_source

#endif  // BOOK_SOURCE_H
