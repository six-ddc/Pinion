// epub_source.h
// EPUB 书籍后端：ZIP 容器（zip_reader）+ OPF/NCX/nav 解析 + XHTML 正文抽取
// （html_extract）+ 插图解码（image_decode）。对上层与 TXT 后端呈现同构接口
// （book_source 分发），坐标系为**虚拟线性偏移**（全书各 spine 项抽取文本首尾相接，
// 见 ebook_models.h ChapterEntry 注释）。在 ebook_worker 线程使用。

#ifndef EPUB_SOURCE_H
#define EPUB_SOURCE_H

#include "ebook_models.h"

namespace epub_source {

// 内容抽取算法版本：html_extract/切块规则变更时 +1，使旧 sidecar（含虚拟偏移）失效重建。
constexpr uint8_t kExtractorVersion = 1;

// 打开书：sidecar 命中时只重解容器元数据（毫秒级）；否则全书抽取建索引并写缓存。
bool OpenBook(const BookInfo& book, ChapterIndex& out);

// 载入章节：解压所属 spine 项 → 抽取 → 按 src_off 切块，填 utf8_buf 与 images/headings
// 旁表（images 只填 src_path/src_w/src_h，像素解码在 PrepareImages）。
bool LoadChapter(const BookInfo& book, const ChapterIndex& idx, int chapter_idx,
                 PaginatedChapter& out);

// 分页前调用（worker 线程）：按排版参数确定各插图显示尺寸（fit content_w × content_h，
// 只缩不放）并解码像素（RGB888，PNG alpha 以 m.bg888 合成）。受单章解码预算约束，
// 超限/失败的图 px 置空（渲染 placeholder）。TXT 章节（images 空）调用无副作用。
void PrepareImages(const BookInfo& book, PaginatedChapter& pc, const PageMetrics& m);

// 提取封面并解码缩放进 (max_w, max_h)，RGB888（书架缩略图用）。失败返回 nullptr。
// 独立于 OpenBook（书架批量生成封面时不建全书索引）。
uint8_t* ExtractCover(const BookInfo& book, uint16_t max_w, uint16_t max_h, uint16_t* out_w,
                      uint16_t* out_h);

}  // namespace epub_source

#endif  // EPUB_SOURCE_H
