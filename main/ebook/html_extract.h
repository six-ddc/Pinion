// html_extract.h
// 容错 XHTML/HTML → 纯文本抽取（EPUB 正文/nav 用）。不追求 XML 严格性：真实世界的
// EPUB 常有未转义 & / 未闭合标签，容错扫描比严格解析更稳。迭代实现（无递归，worker
// 栈 16KB）。纯逻辑，无 lv_obj；在 ebook_worker 线程使用。
//
// 产出规则：
//   - 块级标签边界（p/div/h1..h6/li/blockquote/br/tr/table/section/article/hr…）→ '\n'
//     分段；连续空段折叠（输出文本不出现连续 '\n'）。
//   - 行内空白折叠为单个空格（XHTML 源码缩进/换行不进正文）；段首尾空白 trim。
//   - head/style/script/rt(注音)/template 整段跳过；其余未知标签剥离保留文本。
//   - 实体：命名常用集（amp/lt/gt/quot/apos/nbsp/hellip/mdash/ldquo/rdquo/lsquo/rsquo/
//     middot/times/copy…）+ 数字 &#123; / &#x1F; 全支持；未知实体原样保留。
//   - <img src=…> / SVG <image xlink:href|href=…> → 文本流插入 U+FFFC（EF BF BC）
//     哨兵、独立成段，并在 blocks 记录 kImage（src 原样，未归一化）。
//   - h1/h2 → blocks 记录 kHeading（off/len 覆盖该标题段文本）；h3-h6 按普通段落。
//   - <b>/<strong>/<i>/<em> → blocks 记录 kEmphasis（off/len 覆盖强调文本，按 off 升序、
//     不重叠；跨块自动断开）。文本本身照常剥离进正文，仅供 reader 换色呈现。
//   - <title> 文本单独抓出（不进正文文本流）。

#ifndef HTML_EXTRACT_H
#define HTML_EXTRACT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace html_extract {

enum class BlockType : uint8_t {
    kHeading = 1,   // h1/h2 标题段
    kImage = 2,     // 插图哨兵
    kEmphasis = 3,  // 行内强调（<b>/<strong>/<i>/<em>）：text_off/len 覆盖强调文本，src 空
};

struct Block {
    BlockType type;
    uint32_t text_off = 0;  // 在 Result.text 中的字节偏移（kImage：指向 U+FFFC 首字节）
    uint32_t text_len = 0;  // kHeading：标题文本字节长；kImage：恒 3
    std::string src;        // kImage：src/href 原始值（未做实体解码外的处理）；kHeading 空
};

struct Result {
    std::string text;           // 抽取正文（UTF-8，段落 '\n' 分隔，无首尾空段）
    std::vector<Block> blocks;  // 标题/图片旁表（按 text_off 升序）
    std::string title;          // <title> 内容（trim；无则空）
};

// 对 html[0..len) 抽取。总是"成功"（容错，最坏产出空文本）。out 会被清空重填。
void Extract(const char* html, size_t len, Result& out);

// 便捷：抽取 <a href="…">文本</a> 列表（解析 EPUB3 nav.xhtml 目录用）。
// 返回按文档顺序的 (href, 链接文本) 对；href 未归一化。
struct LinkItem {
    std::string href;
    std::string text;
};
void ExtractLinks(const char* html, size_t len, std::vector<LinkItem>& out);

}  // namespace html_extract

#endif  // HTML_EXTRACT_H
