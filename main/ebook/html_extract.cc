// html_extract.cc — 容错 XHTML/HTML → 纯文本抽取。单趟状态机，无 DOM、无递归。
// 见 html_extract.h 的产出规则。纯 STL、host 可测。

#include "html_extract.h"

#include <cctype>
#include <cstring>

namespace html_extract {

namespace {

// ---- UTF-8 编码 ------------------------------------------------------------
int Utf8Encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// ---- 实体解码 --------------------------------------------------------------
// s 指向 '&'，len 为剩余长度。解码结果追加到 out。返回消耗的输入字节数。
// 非法/未知实体：原样输出 '&' 起的相应字节（返回 1，只吐 '&'）。
size_t DecodeEntity(const char* s, size_t len, std::string& out) {
    // 找 ';'（限 12 字节内）
    size_t semi = 0;
    for (size_t k = 1; k < len && k < 13; k++) {
        if (s[k] == ';') {
            semi = k;
            break;
        }
    }
    if (semi == 0) {
        out.push_back('&');
        return 1;
    }
    size_t nlen = semi - 1;  // 实体名长度（'&' 与 ';' 之间）
    const char* name = s + 1;

    if (name[0] == '#') {  // 数字实体
        uint32_t cp = 0;
        bool ok = false;
        if (nlen > 1 && (name[1] == 'x' || name[1] == 'X')) {
            for (size_t k = 2; k < nlen; k++) {
                char c = name[k];
                uint32_t d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { ok = false; break; }
                cp = cp * 16 + d;
                ok = true;
            }
        } else if (nlen > 1) {
            for (size_t k = 1; k < nlen; k++) {
                if (name[k] < '0' || name[k] > '9') { ok = false; break; }
                cp = cp * 10 + (name[k] - '0');
                ok = true;
            }
        }
        if (ok && cp > 0 && cp <= 0x10FFFF) {
            char buf[4];
            out.append(buf, Utf8Encode(cp, buf));
            return semi + 1;
        }
        out.push_back('&');
        return 1;
    }

    // 命名实体
    struct Named {
        const char* name;
        uint32_t cp;  // 0 = 丢弃（如 shy）
    };
    static const Named kTable[] = {
        {"amp", '&'},      {"lt", '<'},        {"gt", '>'},      {"quot", '"'},
        {"apos", '\''},    {"nbsp", ' '},      {"hellip", 0x2026}, {"mdash", 0x2014},
        {"ndash", 0x2013}, {"ldquo", 0x201C},  {"rdquo", 0x201D}, {"lsquo", 0x2018},
        {"rsquo", 0x2019}, {"middot", 0x00B7}, {"times", 0x00D7}, {"copy", 0x00A9},
        {"shy", 0},
    };
    for (const Named& e : kTable) {
        if (strlen(e.name) == nlen && strncmp(e.name, name, nlen) == 0) {
            if (e.cp != 0) {
                char buf[4];
                out.append(buf, Utf8Encode(e.cp, buf));
            }
            return semi + 1;
        }
    }
    out.push_back('&');  // 未知命名实体：原样保留 &
    return 1;
}

// 文本追加的跨调用状态（段内连续折叠）。last=0 表示段首（尚无字符）。
struct TextState {
    bool prev_space = true;   // 末尾已是空白 / 段首（吞前导空白）
    bool pending_nl = false;  // 待决空白里含换行（软换行）
    unsigned char last = 0;   // 上一个已输出字符的末字节（判 CJK 折叠用）
};

// 把 [b, e) 原始文本（含实体）解码后追加到 dst，折叠空白。关键：源码里两个 CJK 字符
// 之间的**软换行**（\n/\r）不产生空格——浏览器对 CJK 间换行即如此，否则中文正文每逢
// 源码换行处都会多一个空格。而有意打的半角空格（"第一章 归途"）予以保留。
// 判据：待决空白含换行 且 两侧字节均 ≥0x80（非 ASCII，涵盖中日韩及全角标点）→ 丢弃；
// 其余情况（纯空格、英文词间、中英之间）保留一个空格。
void AppendText(const char* b, const char* e, std::string& dst, TextState& st) {
    std::string dec;
    auto emit = [&](const char* piece, size_t n) {
        if (n == 0) return;
        unsigned char first = static_cast<unsigned char>(piece[0]);
        if (st.prev_space && st.last != 0) {
            bool both_cjk = (st.last & 0x80) && (first & 0x80);
            bool drop = st.pending_nl && both_cjk;  // 仅"软换行 + 两侧 CJK"折掉
            if (!drop) dst.push_back(' ');
        }
        dst.append(piece, n);
        st.last = static_cast<unsigned char>(piece[n - 1]);
        st.prev_space = false;
        st.pending_nl = false;
    };
    for (const char* p = b; p < e;) {
        if (*p == '&') {
            dec.clear();
            size_t used = DecodeEntity(p, static_cast<size_t>(e - p), dec);
            if (dec.size() == 1 && dec[0] == ' ') {
                st.prev_space = true;  // nbsp 视作可折叠空白（非换行）
            } else {
                emit(dec.data(), dec.size());
            }
            p += used;
            continue;
        }
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\n' || c == '\r') {
            st.prev_space = true;
            st.pending_nl = true;
        } else if (c == ' ' || c == '\t') {
            st.prev_space = true;
        } else {
            emit(p, 1);
        }
        p++;
    }
}

// ---- 标签名匹配 ------------------------------------------------------------
// 去命名空间前缀（svg:image → image）后与 name 比（大小写不敏感）。
bool NameEq(const char* tag, size_t tlen, const char* name) {
    for (size_t i = 0; i < tlen; i++) {
        if (tag[i] == ':') {
            tag += i + 1;
            tlen -= i + 1;
            break;
        }
    }
    size_t m = strlen(name);
    if (tlen != m) return false;
    for (size_t i = 0; i < m; i++) {
        if (Lower(tag[i]) != Lower(name[i])) return false;
    }
    return true;
}

bool IsBlockTag(const char* n, size_t len) {
    static const char* kBlocks[] = {
        "p",      "div",    "h1",     "h2",      "h3",         "h4",     "h5",
        "h6",     "li",     "ul",     "ol",      "blockquote", "table",  "tr",
        "td",     "th",     "section","article", "header",     "footer", "figure",
        "figcaption", "aside", "hr",  "pre",     "dd",         "dt",     "dl",
        "br",     "nav",    "main",
    };
    for (const char* b : kBlocks) {
        if (NameEq(n, len, b)) return true;
    }
    return false;
}

bool IsRawSkipTag(const char* n, size_t len) {
    return NameEq(n, len, "style") || NameEq(n, len, "script") || NameEq(n, len, "rt") ||
           NameEq(n, len, "template");
}

// 行内强调标签（换色呈现）：<b>/<strong>/<i>/<em>。
bool IsEmphasisTag(const char* n, size_t len) {
    return NameEq(n, len, "b") || NameEq(n, len, "strong") || NameEq(n, len, "i") ||
           NameEq(n, len, "em");
}

// 从 attr_begin..attr_end 提取属性值（大小写不敏感、去命名空间前缀；实体解码）。缺失返回空。
std::string AttrValue(const char* s, size_t ab, size_t ae, const char* name) {
    size_t m = strlen(name);
    size_t p = ab;
    while (p < ae) {
        while (p < ae && (std::isspace(static_cast<unsigned char>(s[p])) || s[p] == '/')) p++;
        size_t nb = p;
        while (p < ae && s[p] != '=' && !std::isspace(static_cast<unsigned char>(s[p]))) p++;
        size_t ne = p;
        size_t abp = nb;
        for (size_t i = nb; i < ne; i++) {
            if (s[i] == ':') abp = i + 1;
        }
        bool match = (ne - abp == m);
        if (match) {
            for (size_t i = 0; i < m; i++) {
                if (Lower(s[abp + i]) != Lower(name[i])) {
                    match = false;
                    break;
                }
            }
        }
        while (p < ae && std::isspace(static_cast<unsigned char>(s[p]))) p++;
        if (p >= ae || s[p] != '=') {
            if (match) return "";  // 布尔属性
            continue;
        }
        p++;  // '='
        while (p < ae && std::isspace(static_cast<unsigned char>(s[p]))) p++;
        size_t vb, ve;
        if (p < ae && (s[p] == '"' || s[p] == '\'')) {
            char q = s[p++];
            vb = p;
            while (p < ae && s[p] != q) p++;
            ve = p;
            if (p < ae) p++;
        } else {
            vb = p;
            while (p < ae && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '>') p++;
            ve = p;
        }
        if (match) {
            std::string out;
            bool ps = false;  // 属性值不折叠首尾（保留 href 原样），但实体要解码
            for (const char* q = s + vb; q < s + ve;) {
                if (*q == '&') {
                    std::string dec;
                    size_t used = DecodeEntity(q, static_cast<size_t>(s + ve - q), dec);
                    out += dec;
                    q += used;
                } else {
                    out.push_back(*q++);
                }
            }
            (void)ps;
            return out;
        }
    }
    return "";
}

// 找字面闭标签 </name>（大小写不敏感）的 '<' 位置；找不到返回 len。
size_t FindClose(const char* s, size_t len, size_t from, const char* name) {
    size_t m = strlen(name);
    for (size_t p = from; p + 1 < len; p++) {
        if (s[p] != '<' || s[p + 1] != '/') continue;
        size_t q = p + 2;
        // 跳过可能的命名空间前缀
        size_t nb = q;
        while (q < len && s[q] != '>' && !std::isspace(static_cast<unsigned char>(s[q]))) q++;
        size_t nlen = q - nb;
        const char* tag = s + nb;
        // 去前缀
        for (size_t i = 0; i < nlen; i++) {
            if (tag[i] == ':') {
                tag += i + 1;
                nlen -= i + 1;
                break;
            }
        }
        if (nlen == m) {
            bool eq = true;
            for (size_t i = 0; i < m; i++) {
                if (Lower(tag[i]) != Lower(name[i])) {
                    eq = false;
                    break;
                }
            }
            if (eq) return p;
        }
    }
    return len;
}

// 解析一个标签的骨架。s[lt]=='<'。返回 '>' 之后的位置（无 '>' 则 len）。
struct Tag {
    bool closing = false;
    const char* name = nullptr;
    size_t name_len = 0;
    size_t attr_begin = 0;
    size_t attr_end = 0;
    size_t next = 0;
};

bool ParseTag(const char* s, size_t len, size_t lt, Tag& t) {
    size_t p = lt + 1;
    t.closing = (p < len && s[p] == '/');
    if (t.closing) p++;
    size_t nb = p;
    while (p < len && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == ':' || s[p] == '_' ||
                       s[p] == '-')) {
        p++;
    }
    if (p == nb) return false;  // 非标签
    t.name = s + nb;
    t.name_len = p - nb;
    t.attr_begin = p;
    char quote = 0;
    while (p < len) {
        char c = s[p];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            break;
        }
        p++;
    }
    t.attr_end = p;
    t.next = (p < len) ? p + 1 : len;
    return true;
}

}  // namespace

void Extract(const char* html, size_t len, Result& out) {
    out.text.clear();
    out.blocks.clear();
    out.title.clear();
    if (html == nullptr || len == 0) return;

    std::string cur;        // 当前段（折叠后，未 trim）
    TextState st;           // 文本折叠状态
    int heading_depth = 0;  // >0：在 h1/h2 内
    int suppress = 0;       // >0：文本不进正文（head 内）
    int emph_depth = 0;     // >0：在 <b>/<strong>/<i>/<em> 内
    uint32_t emph_start = 0;  // 当前强调区间在 cur 里的起点（cur 空间）
    std::vector<std::pair<uint32_t, uint32_t>> cur_emph;  // 本段已闭合的强调区间（cur 空间）

    auto flush = [&]() {
        // 段末仍开着的强调：截到段尾（跨块的 <b> 在此断开，下一段从段首续）。
        if (emph_depth > 0) cur_emph.emplace_back(emph_start, static_cast<uint32_t>(cur.size()));
        size_t a = 0, b = cur.size();
        while (a < b && cur[a] == ' ') a++;
        while (b > a && cur[b - 1] == ' ') b--;
        if (b > a) {
            if (!out.text.empty()) out.text.push_back('\n');
            uint32_t off = static_cast<uint32_t>(out.text.size());
            out.text.append(cur, a, b - a);
            if (heading_depth > 0) {
                Block blk;
                blk.type = BlockType::kHeading;
                blk.text_off = off;
                blk.text_len = static_cast<uint32_t>(b - a);
                out.blocks.push_back(std::move(blk));
            }
            // 强调区间：clip 到 trim 后范围 [a,b)，再去掉边界空格（折叠出的软空格会落进区间
            // 起点，染色空格虽不可见但不精确），映射到 out.text 偏移。
            for (const auto& r : cur_emph) {
                uint32_t s = r.first < a ? static_cast<uint32_t>(a) : r.first;
                uint32_t e = r.second > b ? static_cast<uint32_t>(b) : r.second;
                while (s < e && cur[s] == ' ') s++;
                while (e > s && cur[e - 1] == ' ') e--;
                if (e <= s) continue;
                Block blk;
                blk.type = BlockType::kEmphasis;
                blk.text_off = off + (s - static_cast<uint32_t>(a));
                blk.text_len = e - s;
                out.blocks.push_back(std::move(blk));
            }
        }
        cur.clear();
        cur_emph.clear();
        emph_start = 0;  // 新段：若强调仍开着，从段首续
        st = TextState{};
    };

    auto emit_image = [&](const std::string& src) {
        flush();
        if (src.empty()) return;
        if (!out.text.empty()) out.text.push_back('\n');
        uint32_t off = static_cast<uint32_t>(out.text.size());
        out.text.append("\xEF\xBF\xBC", 3);  // U+FFFC
        Block blk;
        blk.type = BlockType::kImage;
        blk.text_off = off;
        blk.text_len = 3;
        blk.src = src;
        out.blocks.push_back(std::move(blk));
        st = TextState{};
    };

    size_t pos = 0;
    size_t text_start = 0;
    while (pos < len) {
        if (html[pos] != '<') {
            pos++;
            continue;
        }
        // 提交 '<' 前的文本
        if (suppress == 0 && pos > text_start) {
            AppendText(html + text_start, html + pos, cur, st);
        }

        // 特殊构造
        if (pos + 3 < len && html[pos + 1] == '!' && html[pos + 2] == '-' && html[pos + 3] == '-') {
            const char* e = static_cast<const char*>(memmem(html + pos + 4, len - pos - 4, "-->", 3));
            pos = e ? static_cast<size_t>(e - html) + 3 : len;
            text_start = pos;
            continue;
        }
        if (pos + 8 < len && memcmp(html + pos + 1, "![CDATA[", 8) == 0) {
            const char* e = static_cast<const char*>(memmem(html + pos + 9, len - pos - 9, "]]>", 3));
            size_t cend = e ? static_cast<size_t>(e - html) : len;
            if (suppress == 0) AppendText(html + pos + 9, html + cend, cur, st);
            pos = e ? cend + 3 : len;
            text_start = pos;
            continue;
        }
        if (pos + 1 < len && (html[pos + 1] == '!' || html[pos + 1] == '?')) {  // DOCTYPE / <?xml
            size_t gt = pos + 1;
            while (gt < len && html[gt] != '>') gt++;
            pos = (gt < len) ? gt + 1 : len;
            text_start = pos;
            continue;
        }

        Tag t;
        if (!ParseTag(html, len, pos, t)) {  // 裸 '<' 当文本
            if (suppress == 0) {
                const char lt = '<';
                AppendText(&lt, &lt + 1, cur, st);
            }
            pos++;
            text_start = pos;
            continue;
        }

        // <title>：抓文本
        if (!t.closing && NameEq(t.name, t.name_len, "title")) {
            size_t close = FindClose(html, len, t.next, "title");
            if (out.title.empty()) {
                std::string tt;
                TextState tst;
                AppendText(html + t.next, html + close, tt, tst);
                size_t a = 0, b = tt.size();
                while (a < b && tt[a] == ' ') a++;
                while (b > a && tt[b - 1] == ' ') b--;
                out.title.assign(tt, a, b - a);
            }
            size_t after = FindClose(html, len, t.next, "title");
            // 跳过 </title>
            if (after < len) {
                size_t gt = after;
                while (gt < len && html[gt] != '>') gt++;
                pos = (gt < len) ? gt + 1 : len;
            } else {
                pos = len;
            }
            text_start = pos;
            continue;
        }

        // 自闭合判定
        bool self_close = (t.attr_end > t.attr_begin && html[t.attr_end - 1] == '/');

        // style/script/rt/template：快进到闭标签
        if (!t.closing && IsRawSkipTag(t.name, t.name_len)) {
            if (self_close) {
                pos = t.next;
                text_start = pos;
                continue;
            }
            char nbuf[16];
            size_t nl = t.name_len < 15 ? t.name_len : 15;
            // 去前缀存名
            const char* nm = t.name;
            for (size_t i = 0; i < nl; i++) {
                if (nm[i] == ':') {
                    nm += i + 1;
                    nl -= i + 1;
                    break;
                }
            }
            memcpy(nbuf, nm, nl);
            nbuf[nl] = '\0';
            size_t close = FindClose(html, len, t.next, nbuf);
            if (close < len) {
                size_t gt = close;
                while (gt < len && html[gt] != '>') gt++;
                pos = (gt < len) ? gt + 1 : len;
            } else {
                pos = len;
            }
            text_start = pos;
            continue;
        }

        // img / SVG image：插图哨兵
        if (!t.closing && (NameEq(t.name, t.name_len, "img") || NameEq(t.name, t.name_len, "image"))) {
            std::string src = AttrValue(html, t.attr_begin, t.attr_end, "src");
            if (src.empty()) src = AttrValue(html, t.attr_begin, t.attr_end, "href");
            if (suppress == 0) emit_image(src);
            pos = t.next;
            text_start = pos;
            continue;
        }

        // head：suppress 文本
        if (NameEq(t.name, t.name_len, "head")) {
            if (t.closing) {
                if (suppress > 0) suppress--;
            } else if (!self_close) {
                suppress++;
            }
            pos = t.next;
            text_start = pos;
            continue;
        }

        // 块级标签：分段（含 h1/h2 的 heading 管理）
        if (IsBlockTag(t.name, t.name_len)) {
            bool is_h12 = NameEq(t.name, t.name_len, "h1") || NameEq(t.name, t.name_len, "h2");
            bool is_li = NameEq(t.name, t.name_len, "li");
            if (!t.closing) {
                flush();               // 提交上一段
                if (is_h12) heading_depth++;
                if (is_li && suppress == 0) {  // 列表项前缀
                    cur += "\xC2\xB7 ";  // "· "
                    st = TextState{};    // 前缀空格后，内容从"段首"起（吃前导空白、不误插）
                }
            } else {
                flush();               // 提交本段
                if (is_h12 && heading_depth > 0) heading_depth--;
            }
        }
        // 行内强调 <b>/<strong>/<i>/<em>：记录换色区间（cur 空间；文本本身照常剥离）。
        else if (suppress == 0 && !self_close && IsEmphasisTag(t.name, t.name_len)) {
            if (!t.closing) {
                if (emph_depth == 0) emph_start = static_cast<uint32_t>(cur.size());
                emph_depth++;
            } else if (emph_depth > 0) {
                emph_depth--;
                if (emph_depth == 0)
                    cur_emph.emplace_back(emph_start, static_cast<uint32_t>(cur.size()));
            }
        }
        // 其余（span/a 等行内）：剥离，不分段

        pos = t.next;
        text_start = pos;
    }

    if (suppress == 0 && pos > text_start) {
        AppendText(html + text_start, html + (len < pos ? len : pos), cur, st);
    }
    flush();
}

void ExtractLinks(const char* html, size_t len, std::vector<LinkItem>& out) {
    out.clear();
    if (html == nullptr || len == 0) return;
    size_t pos = 0;
    while (pos < len) {
        // 找 <a ...>
        if (html[pos] != '<') {
            pos++;
            continue;
        }
        Tag t;
        if (!ParseTag(html, len, pos, t) || t.closing || !NameEq(t.name, t.name_len, "a")) {
            pos = (pos < len) ? pos + 1 : len;
            continue;
        }
        std::string href = AttrValue(html, t.attr_begin, t.attr_end, "href");
        // 累积链接文本到 </a>，剥离内部标签
        size_t close = FindClose(html, len, t.next, "a");
        std::string text;
        TextState ts_state;
        size_t p = t.next, ts = t.next;
        while (p < close) {
            if (html[p] != '<') {
                p++;
                continue;
            }
            if (p > ts) AppendText(html + ts, html + p, text, ts_state);
            Tag inner;
            if (ParseTag(html, len, p, inner)) {
                p = inner.next;
            } else {
                p++;
            }
            ts = p;
        }
        if (close > ts) AppendText(html + ts, html + close, text, ts_state);
        size_t a = 0, b = text.size();
        while (a < b && text[a] == ' ') a++;
        while (b > a && text[b - 1] == ' ') b--;
        if (!href.empty()) {
            LinkItem li;
            li.href = href;
            li.text.assign(text, a, b - a);
            out.push_back(std::move(li));
        }
        // 跳过 </a>
        if (close < len) {
            size_t gt = close;
            while (gt < len && html[gt] != '>') gt++;
            pos = (gt < len) ? gt + 1 : len;
        } else {
            pos = len;
        }
    }
}

}  // namespace html_extract
