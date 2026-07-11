// epub_source.cc — EPUB 后端：容器/OPF/NCX/nav 解析、虚拟线性坐标索引、章节载入、
// 插图准备、封面提取。全部跑在 ebook_worker 线程（含一个线程私有的"当前书"上下文）。

#include "epub_source.h"

#include "book_source.h"
#include "book_store.h"
#include "html_extract.h"
#include "image_decode.h"
#include "zip_reader.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace epub_source {

namespace {

constexpr char TAG[] = "epub_source";

// 与 book_source.cc 的 TXT 切块参数保持一致（章节体验统一）。
constexpr uint32_t kMaxChapterBytes = 256 * 1024;  // 超此按块细分
constexpr uint32_t kBlockBytes = 64 * 1024;
constexpr size_t kMaxMetaBytes = 2 * 1024 * 1024;    // container/OPF/NCX/nav 解压保险丝
constexpr size_t kMaxXhtmlBytes = 8 * 1024 * 1024;   // 单个 spine 项解压保险丝
constexpr size_t kMaxImgEncoded = 4 * 1024 * 1024;   // 单图编码字节保险丝
constexpr size_t kChapterImgBudget = 3 * 1024 * 1024;  // 单章解码像素总预算（RGB888）

void* PsramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

// ---- 通用小工具 --------------------------------------------------------------

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

// XML 属性/短文本的实体解码（常用集 + 数字实体；未知原样保留）。
std::string DecodeEntities(const char* s, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len;) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        size_t semi = std::string::npos;
        for (size_t k = i + 1; k < len && k < i + 12; k++) {
            if (s[k] == ';') {
                semi = k;
                break;
            }
        }
        if (semi == std::string::npos) {
            out.push_back(s[i++]);
            continue;
        }
        std::string ent(s + i + 1, semi - i - 1);
        char buf[4];
        if (ent == "amp") {
            out.push_back('&');
        } else if (ent == "lt") {
            out.push_back('<');
        } else if (ent == "gt") {
            out.push_back('>');
        } else if (ent == "quot") {
            out.push_back('"');
        } else if (ent == "apos") {
            out.push_back('\'');
        } else if (ent == "nbsp") {
            out.push_back(' ');
        } else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = 0;
            bool ok = false;
            if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                for (size_t k = 2; k < ent.size(); k++) {
                    char c = ent[k];
                    uint32_t d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else { ok = false; break; }
                    cp = cp * 16 + d;
                    ok = true;
                }
            } else {
                for (size_t k = 1; k < ent.size(); k++) {
                    if (ent[k] < '0' || ent[k] > '9') { ok = false; break; }
                    cp = cp * 10 + (ent[k] - '0');
                    ok = true;
                }
            }
            if (ok && cp > 0 && cp <= 0x10FFFF) {
                out.append(buf, Utf8Encode(cp, buf));
            } else {
                out.append(s + i, semi - i + 1);
            }
        } else {
            out.append(s + i, semi - i + 1);  // 未知实体原样
        }
        i = semi + 1;
    }
    return out;
}

// URL percent 解码（href 里的 %E4%BD%93 等）。
std::string PercentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(in[i + 1]), l = hex(in[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 3;
                continue;
            }
        }
        out.push_back(in[i++]);
    }
    return out;
}

std::string DirOf(const std::string& path) {
    size_t p = path.rfind('/');
    return (p == std::string::npos) ? "" : path.substr(0, p + 1);
}

// href（相对 base_dir）→ zip 内规范路径：去 fragment/query、percent 解码、消 ./ ../。
std::string NormalizePath(const std::string& base_dir, const std::string& href_raw) {
    std::string href = href_raw;
    size_t cut = href.find_first_of("#?");
    if (cut != std::string::npos) href.resize(cut);
    href = PercentDecode(href);
    if (href.empty()) return "";
    std::string full = (href[0] == '/') ? href.substr(1) : base_dir + href;

    // 消解 "." ".." 段
    std::vector<std::string> seg;
    size_t start = 0;
    while (start <= full.size()) {
        size_t e = full.find('/', start);
        if (e == std::string::npos) e = full.size();
        std::string part = full.substr(start, e - start);
        if (part == "..") {
            if (!seg.empty()) seg.pop_back();
        } else if (!part.empty() && part != ".") {
            seg.push_back(std::move(part));
        }
        start = e + 1;
    }
    std::string out;
    for (size_t i = 0; i < seg.size(); i++) {
        if (i) out.push_back('/');
        out += seg[i];
    }
    return out;
}

// 标题拷入 char[64]：截断对齐 UTF-8 边界。
void CopyTitle(const std::string& t, char* dst, size_t cap) {
    size_t n = t.size();
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && (static_cast<uint8_t>(t[n]) & 0xC0) == 0x80) n--;
    }
    std::memcpy(dst, t.data(), n);
    dst[n] = '\0';
}

// ---- 极简容错 XML 标签扫描（OPF/container/NCX 用）---------------------------

struct TagInfo {
    size_t name_begin = 0;
    size_t name_len = 0;
    size_t attrs_begin = 0;
    size_t attrs_end = 0;
    bool closing = false;
    size_t next = 0;  // '>' 之后的位置
};

// 从 pos 起找下一个标签。跳过注释/CDATA/声明。找不到返回 false。
bool NextTag(const char* s, size_t len, size_t pos, TagInfo& t) {
    while (pos < len) {
        size_t lt = pos;
        while (lt < len && s[lt] != '<') lt++;
        if (lt >= len) return false;
        if (lt + 3 < len && s[lt + 1] == '!' && s[lt + 2] == '-' && s[lt + 3] == '-') {
            const char* e = static_cast<const char*>(memmem(s + lt + 4, len - lt - 4, "-->", 3));
            pos = e ? (e - s) + 3 : len;
            continue;
        }
        if (lt + 8 < len && memcmp(s + lt + 1, "![CDATA[", 8) == 0) {
            const char* e = static_cast<const char*>(memmem(s + lt + 9, len - lt - 9, "]]>", 3));
            pos = e ? (e - s) + 3 : len;
            continue;
        }
        if (lt + 1 < len && (s[lt + 1] == '!' || s[lt + 1] == '?')) {  // DOCTYPE / <?xml
            size_t gt = lt + 1;
            while (gt < len && s[gt] != '>') gt++;
            pos = gt + 1;
            continue;
        }
        size_t p = lt + 1;
        t.closing = (p < len && s[p] == '/');
        if (t.closing) p++;
        size_t nb = p;
        while (p < len && (isalnum(static_cast<unsigned char>(s[p])) || s[p] == ':' || s[p] == '_' ||
                           s[p] == '-')) {
            p++;
        }
        if (p == nb) {  // "<" 后不是标签名（如裸 "<"），当文本跳过
            pos = lt + 1;
            continue;
        }
        t.name_begin = nb;
        t.name_len = p - nb;
        t.attrs_begin = p;
        // 引号感知地找 '>'
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
        t.attrs_end = p;
        t.next = (p < len) ? p + 1 : len;
        return true;
    }
    return false;
}

// 标签名匹配（大小写不敏感，忽略命名空间前缀 opf: / xhtml: 等）。
bool TagIs(const char* s, const TagInfo& t, const char* name) {
    size_t b = t.name_begin, n = t.name_len;
    for (size_t i = 0; i < n; i++) {
        if (s[b + i] == ':') {
            b += i + 1;
            n -= i + 1;
            break;
        }
    }
    size_t m = strlen(name);
    if (n != m) return false;
    for (size_t i = 0; i < m; i++) {
        if (tolower(static_cast<unsigned char>(s[b + i])) != tolower(static_cast<unsigned char>(name[i])))
            return false;
    }
    return true;
}

// 属性值（大小写不敏感、忽略命名空间前缀；实体解码后返回）。缺失返回空。
std::string AttrValue(const char* s, const TagInfo& t, const char* name) {
    size_t m = strlen(name);
    size_t p = t.attrs_begin;
    while (p < t.attrs_end) {
        while (p < t.attrs_end && (isspace(static_cast<unsigned char>(s[p])) || s[p] == '/')) p++;
        size_t nb = p;
        while (p < t.attrs_end && s[p] != '=' && !isspace(static_cast<unsigned char>(s[p]))) p++;
        size_t ne = p;
        // 属性名去命名空间前缀
        size_t ab = nb;
        for (size_t i = nb; i < ne; i++) {
            if (s[i] == ':') ab = i + 1;
        }
        bool match = (ne - ab == m);
        if (match) {
            for (size_t i = 0; i < m; i++) {
                if (tolower(static_cast<unsigned char>(s[ab + i])) !=
                    tolower(static_cast<unsigned char>(name[i]))) {
                    match = false;
                    break;
                }
            }
        }
        while (p < t.attrs_end && isspace(static_cast<unsigned char>(s[p]))) p++;
        if (p >= t.attrs_end || s[p] != '=') {
            if (match) return "";  // 布尔属性
            continue;
        }
        p++;  // '='
        while (p < t.attrs_end && isspace(static_cast<unsigned char>(s[p]))) p++;
        size_t vb, ve;
        if (p < t.attrs_end && (s[p] == '"' || s[p] == '\'')) {
            char q = s[p++];
            vb = p;
            while (p < t.attrs_end && s[p] != q) p++;
            ve = p;
            if (p < t.attrs_end) p++;
        } else {
            vb = p;
            while (p < t.attrs_end && !isspace(static_cast<unsigned char>(s[p])) && s[p] != '>') p++;
            ve = p;
        }
        if (match) return DecodeEntities(s + vb, ve - vb);
    }
    return "";
}

// 标签后的紧邻文本（到下一个 '<'），实体解码 + 空白折叠 + trim。NCX <text> 用。
std::string TextAfter(const char* s, size_t len, size_t pos) {
    size_t e = pos;
    while (e < len && s[e] != '<') e++;
    std::string raw = DecodeEntities(s + pos, e - pos);
    std::string out;
    out.reserve(raw.size());
    bool ws = true;  // 开头视为空白态（吃掉前导）
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!ws && !out.empty()) out.push_back(' ');
            ws = true;
        } else {
            out.push_back(c);
            ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---- EPUB 容器上下文（worker 线程私有）--------------------------------------

struct ManifestItem {
    std::string path;  // zip 内规范路径
    std::string media_type;
    std::string properties;
};

struct EpubContext {
    std::string book_path;
    uint32_t book_size = 0;
    bool valid = false;
    zip_reader::ZipReader zip;
    std::vector<std::string> spine;  // 按阅读顺序的 XHTML zip 路径
    std::string ncx_path;
    std::string nav_path;
    std::string cover_path;  // 封面图 zip 路径（可空）
};

EpubContext s_ctx;

// 解析 container.xml + OPF，填 ctx（spine/ncx/nav/cover）。zip 需已 Open。
bool ParseContainer(EpubContext& ctx) {
    size_t clen = 0;
    uint8_t* cxml = ctx.zip.ExtractByName("META-INF/container.xml", &clen, kMaxMetaBytes);
    if (cxml == nullptr) {
        ESP_LOGW(TAG, "no container.xml");
        return false;
    }
    std::string opf_path;
    {
        const char* s = reinterpret_cast<const char*>(cxml);
        TagInfo t;
        size_t pos = 0;
        while (NextTag(s, clen, pos, t)) {
            pos = t.next;
            if (t.closing || !TagIs(s, t, "rootfile")) continue;
            std::string mt = AttrValue(s, t, "media-type");
            if (!mt.empty() && mt.find("oebps-package") == std::string::npos) continue;
            opf_path = NormalizePath("", AttrValue(s, t, "full-path"));
            if (!opf_path.empty()) break;
        }
    }
    heap_caps_free(cxml);
    if (opf_path.empty()) {
        ESP_LOGW(TAG, "no rootfile in container.xml");
        return false;
    }

    size_t olen = 0;
    uint8_t* opf = ctx.zip.ExtractByName(opf_path, &olen, kMaxMetaBytes);
    if (opf == nullptr) {
        ESP_LOGW(TAG, "opf missing: %s", opf_path.c_str());
        return false;
    }
    const std::string opf_dir = DirOf(opf_path);
    const char* s = reinterpret_cast<const char*>(opf);

    std::unordered_map<std::string, ManifestItem> manifest;
    std::vector<std::string> spine_ids;
    std::string ncx_id, cover_meta_id;

    TagInfo t;
    size_t pos = 0;
    while (NextTag(s, olen, pos, t)) {
        pos = t.next;
        if (t.closing) continue;
        if (TagIs(s, t, "item")) {
            std::string id = AttrValue(s, t, "id");
            std::string href = AttrValue(s, t, "href");
            if (id.empty() || href.empty()) continue;
            ManifestItem mi;
            mi.path = NormalizePath(opf_dir, href);
            mi.media_type = AttrValue(s, t, "media-type");
            mi.properties = AttrValue(s, t, "properties");
            manifest.emplace(std::move(id), std::move(mi));
        } else if (TagIs(s, t, "itemref")) {
            std::string idref = AttrValue(s, t, "idref");
            if (!idref.empty()) spine_ids.push_back(std::move(idref));
        } else if (TagIs(s, t, "spine")) {
            std::string toc = AttrValue(s, t, "toc");
            if (!toc.empty()) ncx_id = std::move(toc);
        } else if (TagIs(s, t, "meta")) {
            std::string name = AttrValue(s, t, "name");
            if (name == "cover") cover_meta_id = AttrValue(s, t, "content");
        }
    }
    heap_caps_free(opf);

    // spine：只收 (X)HTML 项
    ctx.spine.clear();
    for (const auto& id : spine_ids) {
        auto it = manifest.find(id);
        if (it == manifest.end()) continue;
        const std::string& mt = it->second.media_type;
        if (mt.find("html") == std::string::npos && mt.find("xml") == std::string::npos) continue;
        if (ctx.zip.Find(it->second.path) < 0) continue;
        ctx.spine.push_back(it->second.path);
    }

    // NCX：spine@toc → manifest；fallback 按 media-type
    if (!ncx_id.empty()) {
        auto it = manifest.find(ncx_id);
        if (it != manifest.end()) ctx.ncx_path = it->second.path;
    }
    if (ctx.ncx_path.empty()) {
        for (const auto& kv : manifest) {
            if (kv.second.media_type.find("dtbncx") != std::string::npos) {
                ctx.ncx_path = kv.second.path;
                break;
            }
        }
    }
    // EPUB3 nav
    for (const auto& kv : manifest) {
        if (kv.second.properties.find("nav") != std::string::npos) {
            ctx.nav_path = kv.second.path;
            break;
        }
    }
    // 封面：meta[name=cover] → properties=cover-image → id/文件名启发式
    auto is_image = [](const ManifestItem& mi) {
        return mi.media_type.rfind("image/", 0) == 0;
    };
    if (!cover_meta_id.empty()) {
        auto it = manifest.find(cover_meta_id);
        if (it != manifest.end() && is_image(it->second)) ctx.cover_path = it->second.path;
    }
    if (ctx.cover_path.empty()) {
        for (const auto& kv : manifest) {
            if (kv.second.properties.find("cover-image") != std::string::npos && is_image(kv.second)) {
                ctx.cover_path = kv.second.path;
                break;
            }
        }
    }
    if (ctx.cover_path.empty()) {
        for (const auto& kv : manifest) {
            if (!is_image(kv.second)) continue;
            std::string id = kv.first;
            for (auto& c : id) c = tolower(static_cast<unsigned char>(c));
            std::string base = kv.second.path;
            size_t sp = base.rfind('/');
            if (sp != std::string::npos) base = base.substr(sp + 1);
            for (auto& c : base) c = tolower(static_cast<unsigned char>(c));
            if (id.find("cover") != std::string::npos || base.find("cover") != std::string::npos) {
                ctx.cover_path = kv.second.path;
                break;
            }
        }
    }

    if (ctx.spine.empty()) {
        ESP_LOGW(TAG, "empty spine");
        return false;
    }
    return true;
}

// 确保 s_ctx 对应当前书（换书时重建）。
bool EnsureContext(const BookInfo& book) {
    if (s_ctx.valid && s_ctx.book_path == book.path && s_ctx.book_size == book.size) return true;
    s_ctx = EpubContext{};
    s_ctx.book_path = book.path;
    s_ctx.book_size = book.size;
    if (!s_ctx.zip.Open(book.path)) {
        ESP_LOGW(TAG, "zip open failed: %s", book.path.c_str());
        return false;
    }
    if (!ParseContainer(s_ctx)) return false;
    s_ctx.valid = true;
    ESP_LOGI(TAG, "ctx ready: %s spine=%d ncx=%d nav=%d cover=%d", book.name.c_str(),
             static_cast<int>(s_ctx.spine.size()), s_ctx.ncx_path.empty() ? 0 : 1,
             s_ctx.nav_path.empty() ? 0 : 1, s_ctx.cover_path.empty() ? 0 : 1);
    return true;
}

// ---- 目录（TOC）--------------------------------------------------------------

// NCX：串行扫描，<text> 文本与随后的 <content src> 配对。
void ParseNcx(const char* s, size_t len, const std::string& ncx_dir,
              std::vector<std::pair<std::string, std::string>>& out) {
    TagInfo t;
    size_t pos = 0;
    std::string pending;
    while (NextTag(s, len, pos, t)) {
        pos = t.next;
        if (t.closing) continue;
        if (TagIs(s, t, "text")) {
            pending = TextAfter(s, len, t.next);
        } else if (TagIs(s, t, "content")) {
            std::string src = AttrValue(s, t, "src");
            if (!src.empty() && !pending.empty()) {
                out.emplace_back(NormalizePath(ncx_dir, src), pending);
            }
        }
    }
}

// 建 zip 路径 → 标题 的映射（NCX 优先，空则 EPUB3 nav；同一路径首个条目生效）。
void BuildTocMap(EpubContext& ctx, std::map<std::string, std::string>& title_of) {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!ctx.ncx_path.empty()) {
        size_t n = 0;
        uint8_t* d = ctx.zip.ExtractByName(ctx.ncx_path, &n, kMaxMetaBytes);
        if (d != nullptr) {
            ParseNcx(reinterpret_cast<const char*>(d), n, DirOf(ctx.ncx_path), pairs);
            heap_caps_free(d);
        }
    }
    if (pairs.empty() && !ctx.nav_path.empty()) {
        size_t n = 0;
        uint8_t* d = ctx.zip.ExtractByName(ctx.nav_path, &n, kMaxMetaBytes);
        if (d != nullptr) {
            std::vector<html_extract::LinkItem> links;
            html_extract::ExtractLinks(reinterpret_cast<const char*>(d), n, links);
            heap_caps_free(d);
            std::string nav_dir = DirOf(ctx.nav_path);
            for (auto& l : links) {
                if (!l.href.empty() && !l.text.empty())
                    pairs.emplace_back(NormalizePath(nav_dir, l.href), l.text);
            }
        }
    }
    for (auto& p : pairs) {
        if (title_of.find(p.first) == title_of.end()) title_of.emplace(p.first, std::move(p.second));
    }
}

// ---- 抽取文本切块（索引构建与 LoadChapter 必须走同一规则，保证偏移一致）------

// 超大抽取文本按 ~64KB 切块：优先切在 '\n' 之后（段边界），16KB 内无 '\n' 则退 UTF-8 边界。
void SplitBlocks(const char* text, size_t len, std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.clear();
    if (len <= kMaxChapterBytes) {
        out.emplace_back(0, static_cast<uint32_t>(len));
        return;
    }
    size_t off = 0;
    while (off < len) {
        size_t end = off + kBlockBytes;
        if (end >= len) {
            out.emplace_back(static_cast<uint32_t>(off), static_cast<uint32_t>(len - off));
            break;
        }
        size_t cut = end;
        size_t floor_ = (end > off + 16 * 1024) ? end - 16 * 1024 : off + 1;
        size_t p = end;
        bool found = false;
        while (p > floor_) {
            if (text[p - 1] == '\n') {
                cut = p;
                found = true;
                break;
            }
            p--;
        }
        if (!found) {
            while (cut > off + 1 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80) cut--;
        }
        out.emplace_back(static_cast<uint32_t>(off), static_cast<uint32_t>(cut - off));
        off = cut;
    }
}

}  // namespace

// ---- OpenBook -----------------------------------------------------------------

bool OpenBook(const BookInfo& book, ChapterIndex& out) {
    if (!EnsureContext(book)) return false;
    if (book_source::LoadIndexCache(book, out, kExtractorVersion)) return true;

    std::map<std::string, std::string> title_of;
    BuildTocMap(s_ctx, title_of);

    out.chapters.clear();
    out.encoding = TextEncoding::kUtf8;
    out.file_size = book.size;
    out.name_hash = book_store::BookKeyHash(book.name, 0);
    out.has_real_chapters = !title_of.empty();

    html_extract::Result res;
    uint32_t acc = 0;  // 虚拟线性偏移累计
    for (size_t si = 0; si < s_ctx.spine.size(); si++) {
        const std::string& path = s_ctx.spine[si];
        size_t xlen = 0;
        uint8_t* xhtml = s_ctx.zip.ExtractByName(path, &xlen, kMaxXhtmlBytes);
        if (xhtml == nullptr) {
            ESP_LOGW(TAG, "spine extract failed: %s", path.c_str());
            continue;  // 跳过坏项，虚拟坐标不推进
        }
        html_extract::Extract(reinterpret_cast<const char*>(xhtml), xlen, res);
        heap_caps_free(xhtml);
        if (res.text.empty()) continue;

        // 章节标题：TOC → XHTML <title> → 第 N 节
        std::string title;
        auto it = title_of.find(path);
        if (it != title_of.end()) {
            title = it->second;
        } else if (!res.title.empty()) {
            title = res.title;
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "第 %d 节", static_cast<int>(si) + 1);
            title = buf;
        }

        std::vector<std::pair<uint32_t, uint32_t>> blocks;
        SplitBlocks(res.text.data(), res.text.size(), blocks);
        for (size_t bi = 0; bi < blocks.size(); bi++) {
            ChapterEntry ce;
            ce.byte_start = acc + blocks[bi].first;
            ce.byte_end = ce.byte_start + blocks[bi].second;
            ce.src_off = blocks[bi].first;
            ce.spine_idx = static_cast<uint16_t>(si);
            if (bi == 0) {
                CopyTitle(title, ce.title, sizeof(ce.title));
            } else {
                char base[49];
                CopyTitle(title, base, sizeof(base));
                snprintf(ce.title, sizeof(ce.title), "%s (%d)", base, static_cast<int>(bi) + 1);
            }
            out.chapters.push_back(ce);
        }
        acc += static_cast<uint32_t>(res.text.size());
    }

    if (out.chapters.empty()) {
        ESP_LOGW(TAG, "no readable chapter: %s", book.name.c_str());
        return false;
    }
    out.virt_size = acc;
    ESP_LOGI(TAG, "indexed %s: %d chapters, virt=%lu, toc=%d", book.name.c_str(),
             static_cast<int>(out.chapters.size()), static_cast<unsigned long>(acc),
             static_cast<int>(title_of.size()));
    book_source::SaveIndexCache(book, out, kExtractorVersion);
    return true;
}

// ---- LoadChapter ----------------------------------------------------------------

bool LoadChapter(const BookInfo& book, const ChapterIndex& idx, int chapter_idx,
                 PaginatedChapter& out) {
    if (chapter_idx < 0 || chapter_idx >= static_cast<int>(idx.chapters.size())) return false;
    if (!EnsureContext(book)) return false;
    const ChapterEntry& ch = idx.chapters[chapter_idx];
    if (ch.spine_idx >= s_ctx.spine.size()) return false;
    const std::string& spine_path = s_ctx.spine[ch.spine_idx];

    size_t xlen = 0;
    uint8_t* xhtml = s_ctx.zip.ExtractByName(spine_path, &xlen, kMaxXhtmlBytes);
    if (xhtml == nullptr) return false;
    html_extract::Result res;
    html_extract::Extract(reinterpret_cast<const char*>(xhtml), xlen, res);
    heap_caps_free(xhtml);

    uint32_t want = (ch.byte_end > ch.byte_start) ? (ch.byte_end - ch.byte_start) : 0;
    if (want == 0 || ch.src_off + want > res.text.size()) {
        // 文件被替换但 size 未变 / 抽取不一致：宁可失败也不给错位内容
        ESP_LOGW(TAG, "slice out of range: ch=%d src_off=%lu want=%lu text=%u", chapter_idx,
                 static_cast<unsigned long>(ch.src_off), static_cast<unsigned long>(want),
                 static_cast<unsigned>(res.text.size()));
        return false;
    }

    char* buf = static_cast<char*>(PsramAlloc(want));
    if (buf == nullptr) return false;
    std::memcpy(buf, res.text.data() + ch.src_off, want);

    out.FreeBuf();
    out.chapter_idx = chapter_idx;
    out.encoding = TextEncoding::kUtf8;
    out.chapter_file_start = ch.byte_start;  // 虚拟域起点
    out.utf8_buf = buf;
    out.utf8_len = want;

    // 旁表映射到切片坐标
    const uint32_t s0 = ch.src_off, s1 = ch.src_off + want;
    const std::string spine_dir = DirOf(spine_path);
    for (const auto& b : res.blocks) {
        if (b.text_off < s0 || b.text_off >= s1) continue;
        if (b.type == html_extract::BlockType::kImage) {
            ImageRef ir;
            ir.off = b.text_off - s0;
            ir.src_path = NormalizePath(spine_dir, b.src);
            if (!ir.src_path.empty() && s_ctx.zip.Find(ir.src_path) < 0) ir.src_path.clear();
            out.images.push_back(std::move(ir));
        } else if (b.type == html_extract::BlockType::kHeading) {
            HeadingSpan hs;
            hs.off = b.text_off - s0;
            hs.len = b.text_len;
            if (hs.off + hs.len > want) hs.len = want - hs.off;  // 跨块 clamp
            out.headings.push_back(hs);
        } else if (b.type == html_extract::BlockType::kEmphasis) {
            EmphasisSpan es;
            es.off = b.text_off - s0;
            es.len = b.text_len;
            if (es.off + es.len > want) es.len = want - es.off;  // 跨块 clamp
            out.emphasis.push_back(es);
        }
    }
    return true;
}

// ---- PrepareImages（分页前，worker 线程）----------------------------------------

void PrepareImages(const BookInfo& book, PaginatedChapter& pc, const PageMetrics& m) {
    if (pc.images.empty()) return;
    bool ctx_ok = EnsureContext(book);

    const int32_t box_w = m.content_w;
    const int32_t box_h = (m.content_h - 2 * kImageVMargin) * 9 / 10;  // 顶满页会挤掉页脚观感，留 10%
    size_t budget = kChapterImgBudget;

    for (auto& img : pc.images) {
        // 默认 placeholder 尺寸（探测失败/无效引用时的占位排版）
        img.disp_w = static_cast<uint16_t>(box_w < 320 ? box_w : 320);
        img.disp_h = 180;

        if (!ctx_ok || img.src_path.empty()) continue;
        size_t enc_len = 0;
        uint8_t* enc = s_ctx.zip.ExtractByName(img.src_path, &enc_len, kMaxImgEncoded);
        if (enc == nullptr) continue;

        uint16_t sw = 0, sh = 0;
        if (image_decode::ProbeSize(enc, enc_len, &sw, &sh) && sw > 0 && sh > 0) {
            img.src_w = sw;
            img.src_h = sh;
            // 等比 fit（只缩不放）→ placeholder 也用真实比例
            int32_t dw = sw, dh = sh;
            if (dw > box_w) {
                dh = dh * box_w / dw;
                dw = box_w;
            }
            if (dh > box_h) {
                dw = dw * box_h / dh;
                dh = box_h;
            }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            img.disp_w = static_cast<uint16_t>(dw);
            img.disp_h = static_cast<uint16_t>(dh);
        }

        size_t need = static_cast<size_t>(img.disp_w) * img.disp_h * 3;
        if (need > budget) {
            ESP_LOGW(TAG, "img budget exceeded, placeholder: %s", img.src_path.c_str());
            heap_caps_free(enc);
            continue;
        }
        uint16_t ow = 0, oh = 0;
        uint8_t* px = image_decode::DecodeFit(enc, enc_len, static_cast<uint16_t>(box_w),
                                              static_cast<uint16_t>(box_h), m.bg888, &ow, &oh);
        heap_caps_free(enc);
        if (px == nullptr || ow == 0 || oh == 0) {
            if (px != nullptr) heap_caps_free(px);
            continue;
        }
        img.px = px;
        img.disp_w = ow;
        img.disp_h = oh;
        budget -= static_cast<size_t>(ow) * oh * 3;

        img.dsc = lv_image_dsc_t{};
        img.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        img.dsc.header.cf = LV_COLOR_FORMAT_RGB888;
        img.dsc.header.w = ow;
        img.dsc.header.h = oh;
        img.dsc.header.stride = static_cast<uint32_t>(ow) * 3;
        img.dsc.data = px;
        img.dsc.data_size = static_cast<uint32_t>(ow) * oh * 3;
    }
}

// ---- ExtractCover（书架封面；独立解析，不动 s_ctx）------------------------------

uint8_t* ExtractCover(const BookInfo& book, uint16_t max_w, uint16_t max_h, uint16_t* out_w,
                      uint16_t* out_h) {
    EpubContext ctx;
    ctx.book_path = book.path;
    ctx.book_size = book.size;
    if (!ctx.zip.Open(book.path)) return nullptr;
    if (!ParseContainer(ctx)) return nullptr;
    if (ctx.cover_path.empty()) return nullptr;

    size_t enc_len = 0;
    uint8_t* enc = ctx.zip.ExtractByName(ctx.cover_path, &enc_len, kMaxImgEncoded);
    if (enc == nullptr) return nullptr;
    uint8_t* px = image_decode::DecodeFit(enc, enc_len, max_w, max_h, 0xFFFFFF, out_w, out_h);
    heap_caps_free(enc);
    return px;
}

}  // namespace epub_source
