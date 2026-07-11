// paginator.cc — 断行 / 组页 / 文件偏移映射。见头文件说明。

#include "paginator.h"

#include <esp_heap_caps.h>

#include <cstddef>

namespace paginator {

namespace {

// 解码 buf[off..end) 下一个码点，*adv=字节数（非法则 1 → 0xFFFD）。
uint32_t Decode(const char* buf, size_t off, size_t end, uint8_t* adv) {
    uint8_t c = static_cast<uint8_t>(buf[off]);
    if (c < 0x80) {
        *adv = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && off + 1 < end) {
        *adv = 2;
        return ((c & 0x1F) << 6) | (static_cast<uint8_t>(buf[off + 1]) & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && off + 2 < end) {
        *adv = 3;
        return ((c & 0x0F) << 12) | ((static_cast<uint8_t>(buf[off + 1]) & 0x3F) << 6) |
               (static_cast<uint8_t>(buf[off + 2]) & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && off + 3 < end) {
        *adv = 4;
        return ((c & 0x07) << 18) | ((static_cast<uint8_t>(buf[off + 1]) & 0x3F) << 12) |
               ((static_cast<uint8_t>(buf[off + 2]) & 0x3F) << 6) |
               (static_cast<uint8_t>(buf[off + 3]) & 0x3F);
    }
    *adv = 1;
    return 0xFFFD;
}

bool IsCjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 统一
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // 扩展 A
           (cp >= 0x3000 && cp <= 0x303F) ||   // CJK 标点
           (cp >= 0xFF00 && cp <= 0xFFEF);     // 全角
}

// 行首禁则：这些字符不能出现在一行开头（挂到上一行行尾）。
bool ForbidLineStart(uint32_t cp) {
    switch (cp) {
        case 0x3001: case 0x3002:  // 、。
        case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B:  // ，．：；
        case 0xFF1F: case 0xFF01:  // ？！
        case 0x3009: case 0x300B: case 0x300D: case 0x300F:  // 〉》」』
        case 0x3011: case 0x3015: case 0x3017:  // 】〕〗
        case 0xFF09: case 0xFF3D: case 0xFF5D:  // ）］｝
        case 0x2019: case 0x201D:  // ’ ”
        case 0x00B7: case 0x2026: case 0x2014: case 0xFF5E:  // · … — ～
        case ',': case '.': case '!': case '?': case ':': case ';':
        case ')': case ']': case '}': case '%':
            return true;
        default:
            return false;
    }
}

// 行尾禁则：这些字符不能出现在一行末尾（拉到下一行行首）。
bool ForbidLineEnd(uint32_t cp) {
    switch (cp) {
        case 0x3008: case 0x300A: case 0x300C: case 0x300E:  // 〈《「『
        case 0x3010: case 0x3014: case 0x3016:  // 【〔〖
        case 0xFF08: case 0xFF3B: case 0xFF5B:  // （［｛
        case 0x2018: case 0x201C:  // ‘ “
        case '(': case '[': case '{':
            return true;
        default:
            return false;
    }
}

// 从 off 往前退一个 UTF-8 字符的起点（用于禁则回退）。
size_t PrevCharStart(const char* buf, size_t off, size_t lo) {
    if (off <= lo) return lo;
    size_t p = off - 1;
    while (p > lo && (static_cast<uint8_t>(buf[p]) & 0xC0) == 0x80) p--;
    return p;
}

// FT 用户字体测宽缓存（仅 m.ft_font 时启用）：lv_freetype 度量 miss 要持 face_lock 走
// FT_Load_Glyph（文件流 face = SD 随机读），比内置点阵查表贵几个量级，且与 draw 线程的字形
// 光栅化争同一把锁；中文正文重复率极高，每章一张开放寻址表把 O(章字数) 次 FT 调用压到
// O(去重字形数)。内置字体不走缓存：查表本身就快，且其 kerning 使宽度依赖下一字符，按 cp
// 缓存会改变断行。FT 字体 kerning=NONE（适配器默认），宽度只由 cp 决定；缺字 fallback 到
// 内置的罕见字忽略 kerning 误差（罕见字几乎无 kerning 对，且分页只需近似不越界）。
class WidthCache {
public:
    explicit WidthCache(bool enable) {
        if (enable) {
            e_ = static_cast<Entry*>(
                heap_caps_calloc(kSlots, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
    }
    ~WidthCache() {
        if (e_ != nullptr) heap_caps_free(e_);
    }
    WidthCache(const WidthCache&) = delete;
    WidthCache& operator=(const WidthCache&) = delete;

    uint16_t Get(const lv_font_t* font, uint32_t cp, uint32_t ncp) {
        if (e_ == nullptr || cp == 0) return lv_font_get_glyph_width(font, cp, ncp);
        size_t h = (cp * 2654435761u) & (kSlots - 1);
        size_t empty = SIZE_MAX;
        for (int p = 0; p < kMaxProbe; p++) {
            Entry& s = e_[(h + p) & (kSlots - 1)];
            if (s.cp == cp) return s.w;
            if (s.cp == 0) {
                empty = (h + p) & (kSlots - 1);
                break;
            }
        }
        uint16_t w = lv_font_get_glyph_width(font, cp, ncp);
        if (empty != SIZE_MAX) {  // 探测窗口满则放弃写入（近满时退化为直测，不淘汰）
            e_[empty].cp = cp;
            e_[empty].w = w;
        }
        return w;
    }

private:
    struct Entry {
        uint32_t cp;  // 0 = 空槽（cp==0 不缓存）
        uint16_t w;
    };
    static constexpr size_t kSlots = 4096;  // 2^n；单章去重字形数常 <3k，32KB PSRAM
    static constexpr int kMaxProbe = 8;
    Entry* e_ = nullptr;
};

// 行高按行型展开：文本行 / 标题行 / 图片行（fit = 占位判断高，adv = 实际推进高）。
void LineHeights(const PaginatedChapter& ch, const PageMetrics& m, const LineSpan& ls, int* fit,
                 int* adv) {
    if (ls.flags & kLineFlagImage) {
        const ImageRef* ir = ch.FindImage(ls.off);
        int ih = (ir != nullptr && ir->disp_h > 0) ? ir->disp_h : 180;
        *fit = ih + 2 * kImageVMargin;
        *adv = *fit;
        return;
    }
    if (ls.flags & kLineFlagHeading) {
        int lh = (m.heading_line_height > 0) ? m.heading_line_height : m.line_height;
        *fit = lh;
        *adv = lh + m.line_space;
        return;
    }
    *fit = m.line_height;
    *adv = m.line_height + m.line_space;
}

void BuildPages(PaginatedChapter& ch, const PageMetrics& m) {
    ch.pages.clear();
    if (ch.lines.empty()) return;
    size_t page_first = 0;
    int y = 0;
    for (size_t idx = 0; idx < ch.lines.size(); idx++) {
        bool first_on_page = (idx == page_first);
        int top_pad = 0;
        if (!first_on_page && (ch.lines[idx].flags & kLineFlagParaFirst)) top_pad = m.para_space;
        int fit, adv;
        LineHeights(ch, m, ch.lines[idx], &fit, &adv);
        // 放不下则另起一页（当前页至少已有一行）
        if (!first_on_page && y + top_pad + fit > m.content_h) {
            PageSpan p;
            p.first_line = static_cast<uint32_t>(page_first);
            p.line_count = static_cast<uint16_t>(idx - page_first);
            ch.pages.push_back(p);
            page_first = idx;
            y = 0;
            top_pad = 0;
        }
        y += top_pad + adv;
    }
    PageSpan p;
    p.first_line = static_cast<uint32_t>(page_first);
    p.line_count = static_cast<uint16_t>(ch.lines.size() - page_first);
    ch.pages.push_back(p);
}

// 单次前向遍历，为每页 first line 填原文件字节偏移。
void ComputeFileOffsets(PaginatedChapter& ch) {
    const char* buf = ch.utf8_buf;
    size_t len = ch.utf8_len;
    bool gbk = (ch.encoding == TextEncoding::kGbk);
    size_t bufpos = 0;
    uint32_t file_off = ch.chapter_file_start;
    for (auto& pg : ch.pages) {
        uint32_t target = ch.lines[pg.first_line].off;
        while (bufpos < target && bufpos < len) {
            uint8_t adv;
            uint32_t cp = Decode(buf, bufpos, len, &adv);
            file_off += gbk ? (cp < 0x80 ? 1 : 2) : adv;
            bufpos += adv;
        }
        pg.byte_start = file_off;
    }
}

}  // namespace

void Paginate(PaginatedChapter& ch, const PageMetrics& m) {
    ch.lines.clear();
    ch.pages.clear();
    if (ch.utf8_buf == nullptr || ch.utf8_len == 0 || m.font == nullptr || m.content_w <= 0) return;

    const char* buf = ch.utf8_buf;
    size_t len = ch.utf8_len;

    int indent_w = m.indent_w;  // 段首缩进宽（BuildMetrics 统一测算，与 reader_view 渲染同源）

    // 正文字体测宽缓存（标题字体行占比极小，不值得再开一张表，走直测）。
    WidthCache wcache(m.ft_font);

    size_t heading_cur = 0;  // ch.headings 升序游标

    size_t i = 0;
    while (i < len) {
        if (buf[i] == '\n') {  // 空段：跳过（段间距由 para_space 承担）
            i++;
            continue;
        }
        size_t pstart = i;
        size_t pnl = pstart;
        while (pnl < len && buf[pnl] != '\n') pnl++;
        size_t pend = pnl;
        if (pend > pstart && buf[pend - 1] == '\r') pend--;

        // 图片段：抽取器保证 U+FFFC 哨兵独立成段，整段即一图片行，不测宽。
        if (pend - pstart == 3 && static_cast<uint8_t>(buf[pstart]) == 0xEF &&
            static_cast<uint8_t>(buf[pstart + 1]) == 0xBF &&
            static_cast<uint8_t>(buf[pstart + 2]) == 0xBC) {
            LineSpan ls;
            ls.off = static_cast<uint32_t>(pstart);
            ls.len = 3;
            ls.flags = kLineFlagImage | kLineFlagParaFirst | kLineFlagParaLast;
            ch.lines.push_back(ls);
            i = (pnl < len) ? pnl + 1 : len;
            continue;
        }

        // 标题段：段起点落在 headings 旁表区间内 → 大字号 + 居中 + 无缩进。
        while (heading_cur < ch.headings.size() &&
               ch.headings[heading_cur].off + ch.headings[heading_cur].len <= pstart) {
            heading_cur++;
        }
        bool is_heading = heading_cur < ch.headings.size() &&
                          pstart >= ch.headings[heading_cur].off &&
                          pstart < ch.headings[heading_cur].off + ch.headings[heading_cur].len;
        const lv_font_t* font = (is_heading && m.heading_font != nullptr) ? m.heading_font : m.font;
        uint8_t style_flags = is_heading ? (kLineFlagHeading | kLineFlagCenter) : 0;

        bool need_indent;
        {
            uint8_t a;
            uint32_t c0 = Decode(buf, pstart, pend, &a);
            need_indent = !is_heading && !(c0 == ' ' || c0 == '\t' || c0 == 0x3000);
        }

        size_t cur = pstart;
        bool para_first = true;
        size_t para_line0 = ch.lines.size();
        while (cur < pend) {
            int width = (para_first && need_indent) ? indent_w : 0;
            size_t j = cur;
            size_t cand = SIZE_MAX;  // 最近合法断点（下一行起点）
            size_t line_end = pend;
            bool broke = false;
            bool prev_space = false;
            while (j < pend) {
                uint8_t adv;
                uint32_t cp = Decode(buf, j, pend, &adv);
                uint8_t adv2;
                uint32_t ncp = (j + adv < pend) ? Decode(buf, j + adv, pend, &adv2) : 0;
                uint16_t gw = (font == m.font) ? wcache.Get(font, cp, ncp)
                                               : lv_font_get_glyph_width(font, cp, ncp);

                bool can_break_before = (IsCjk(cp) || prev_space) && !ForbidLineStart(cp);
                if (j > cur && can_break_before) cand = j;

                if (j > cur && width + gw > m.content_w) {
                    line_end = (cand != SIZE_MAX && cand > cur) ? cand : j;  // 无断点→硬断
                    broke = true;
                    break;
                }
                width += gw;
                prev_space = (cp == ' ');
                j += adv;
            }
            if (!broke) line_end = pend;

            // 行首禁则：下一行首若是闭合标点，挂到本行尾（前移一字），有限次。
            for (int k = 0; k < 4 && line_end < pend; k++) {
                uint8_t adv;
                uint32_t cp = Decode(buf, line_end, pend, &adv);
                if (!ForbidLineStart(cp)) break;
                line_end += adv;
            }
            // 行尾禁则：本行尾若是开引号/括号，拉到下一行（回退一字），有限次。
            for (int k = 0; k < 4 && line_end > cur; k++) {
                size_t last = PrevCharStart(buf, line_end, cur);
                if (last <= cur) break;  // 至少保留一字，避免空行
                uint8_t adv;
                uint32_t cp = Decode(buf, last, pend, &adv);
                if (!ForbidLineEnd(cp)) break;
                line_end = last;
            }
            if (line_end <= cur) {  // 兜底：保证前进，避免死循环
                uint8_t adv;
                Decode(buf, cur, pend, &adv);
                line_end = cur + adv;
            }

            LineSpan ls;
            ls.off = static_cast<uint32_t>(cur);
            ls.len = static_cast<uint16_t>(line_end - cur);
            // 首行且需缩进 → 打 kLineFlagIndent：此行 width 已含 indent_w，reader 据此左移行首。
            uint8_t indent_flag = (para_first && need_indent) ? kLineFlagIndent : 0;
            ls.flags = (para_first ? kLineFlagParaFirst : 0) | indent_flag | style_flags;
            ch.lines.push_back(ls);

            cur = line_end;
            while (cur < pend && (buf[cur] == ' ' || buf[cur] == '\t')) cur++;  // 吃掉行首空格
            para_first = false;
        }
        if (ch.lines.size() > para_line0) ch.lines.back().flags |= kLineFlagParaLast;

        i = (pnl < len) ? pnl + 1 : len;
    }

    BuildPages(ch, m);
    ComputeFileOffsets(ch);
}

}  // namespace paginator

// ---- PaginatedChapter 成员（放这里与分页语义同源）----------------------------
void PaginatedChapter::FreeBuf() {
    if (utf8_buf != nullptr) {
        heap_caps_free(utf8_buf);
        utf8_buf = nullptr;
    }
    utf8_len = 0;
    lines.clear();
    pages.clear();
    for (auto& img : images) {
        if (img.px != nullptr) heap_caps_free(img.px);
        img.px = nullptr;
    }
    images.clear();
    headings.clear();
    emphasis.clear();
}

const ImageRef* PaginatedChapter::FindImage(uint32_t off) const {
    int lo = 0, hi = static_cast<int>(images.size()) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (images[mid].off == off) return &images[mid];
        if (images[mid].off < off) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return nullptr;
}

int PaginatedChapter::FindPageByFileOffset(uint32_t file_off) const {
    if (pages.empty()) return 0;
    int lo = 0, hi = static_cast<int>(pages.size()) - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (pages[mid].byte_start <= file_off) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}
