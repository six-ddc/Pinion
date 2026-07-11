// paginator.h
// 中文排版分页引擎：对已载入 UTF-8 正文的章节做断行 + 组页。规则包括段首全角缩进、
// CJK 逐字可断 / ASCII 整词断、行首禁则（闭合标点不悬于行首）、行尾禁则（开引号/
// 括号不留行尾）。纯逻辑（仅用 lv_font 只读测宽），可在 worker 线程运行。

#ifndef PAGINATOR_H
#define PAGINATOR_H

#include "ebook_models.h"

namespace paginator {

// 依据 m 对 chapter 分页，填 chapter.lines / chapter.pages（含每页 byte_start 文件偏移）。
// 反复调用可用于设置变更后的重分页（会先清空旧结果）。
void Paginate(PaginatedChapter& chapter, const PageMetrics& m);

}  // namespace paginator

#endif  // PAGINATOR_H
