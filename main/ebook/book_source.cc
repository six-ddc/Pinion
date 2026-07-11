// book_source.cc — 格式分发 + TXT 编码检测 / 章节扫描 / 章节载入 + sidecar 缓存（共用）。

#include "book_source.h"

#include "book_store.h"
#include "epub_source.h"

#include "ff.h"  // ff_oem2uni（codepage 936 已启用）

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace book_source {

namespace {

constexpr char TAG[] = "book_source";
constexpr size_t kDetectBytes = 32 * 1024;
constexpr size_t kScanChunk = 4096;
constexpr uint32_t kMaxChapterBytes = 256 * 1024;  // 超此按块细分
constexpr uint32_t kBlockBytes = 64 * 1024;        // 无章节/超大章的切块尺寸
constexpr uint16_t kSidecarVersion = 2;  // v2：+virt_size/format/extractor_ver，ChapterEntry 扩字段

void* PsramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

// ---- UTF-8 严格校验（末尾不完整多字节序列宽容处理）------------------------
bool IsValidUtf8(const uint8_t* d, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = d[i];
        size_t need;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
            if (c < 0xC2) return false;  // 过长编码
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            if (c > 0xF4) return false;
        } else {
            return false;
        }
        if (i + need >= n) return true;  // 末尾截断：宽容视为有效
        for (size_t k = 1; k <= need; k++) {
            if ((d[i + 1 + k - 1] & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

// ---- UTF-8 解码 / 编码 ------------------------------------------------------
// 解码 buf[off..len) 的下一个码点，返回码点，*adv=字节数（非法则 1，返回 0xFFFD）。
uint32_t Utf8Decode(const char* buf, size_t off, size_t len, uint8_t* adv) {
    uint8_t c = static_cast<uint8_t>(buf[off]);
    if (c < 0x80) {
        *adv = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && off + 1 < len) {
        *adv = 2;
        return ((c & 0x1F) << 6) | (static_cast<uint8_t>(buf[off + 1]) & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && off + 2 < len) {
        *adv = 3;
        return ((c & 0x0F) << 12) | ((static_cast<uint8_t>(buf[off + 1]) & 0x3F) << 6) |
               (static_cast<uint8_t>(buf[off + 2]) & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && off + 3 < len) {
        *adv = 4;
        return ((c & 0x07) << 18) | ((static_cast<uint8_t>(buf[off + 1]) & 0x3F) << 12) |
               ((static_cast<uint8_t>(buf[off + 2]) & 0x3F) << 6) |
               (static_cast<uint8_t>(buf[off + 3]) & 0x3F);
    }
    *adv = 1;
    return 0xFFFD;
}

// 编码 BMP 码点到 out（≤3 字节），返回写入字节数。
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
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
}

// GBK → UTF-8。raw[in_len] → 新分配的 PSRAM buffer，*out_len 为字节数。失败返回 nullptr。
char* GbkToUtf8(const uint8_t* raw, size_t in_len, size_t* out_len) {
    size_t cap = in_len + in_len / 2 + 16;  // 中文 2B→3B，最坏 ×1.5
    char* out = static_cast<char*>(PsramAlloc(cap));
    if (out == nullptr) return nullptr;
    size_t w = 0;
    for (size_t i = 0; i < in_len;) {
        uint8_t b = raw[i];
        uint32_t cp;
        if (b < 0x80) {
            cp = b;
            i += 1;
        } else if (i + 1 < in_len && b >= 0x81) {
            uint16_t oem = (static_cast<uint16_t>(b) << 8) | raw[i + 1];
            uint16_t wc = ff_oem2uni(oem, 936);
            cp = (wc != 0) ? wc : 0xFFFD;
            i += 2;
        } else {
            cp = 0xFFFD;
            i += 1;
        }
        if (w + 4 > cap) {
            size_t nc = cap * 2;
            char* nb = static_cast<char*>(PsramAlloc(nc));
            if (nb == nullptr) {
                heap_caps_free(out);
                return nullptr;
            }
            std::memcpy(nb, out, w);
            heap_caps_free(out);
            out = nb;
            cap = nc;
        }
        w += Utf8Encode(cp, out + w);
    }
    *out_len = w;
    return out;
}

// ---- 章节标题识别（输入为 UTF-8 短行）--------------------------------------
bool IsCjkNumeral(uint32_t cp) {
    // 零一二三四五六七八九十百千两〇
    static const uint32_t kN[] = {0x96F6, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94,
                                  0x516D, 0x4E03, 0x516B, 0x4E5D, 0x5341, 0x767E,
                                  0x5343, 0x4E24, 0x3007};
    for (uint32_t n : kN)
        if (cp == n) return true;
    return false;
}

bool IsChapterUnit(uint32_t cp) {
    // 章卷回节集部篇折
    static const uint32_t kU[] = {0x7AE0, 0x5377, 0x56DE, 0x8282,
                                  0x96C6, 0x90E8, 0x7BC7, 0x6298};
    for (uint32_t u : kU)
        if (cp == u) return true;
    return false;
}

// 判断一行（UTF-8）是否是章节标题，是则把标题（trim 后）拷进 title[64]。
bool MatchChapterTitle(const std::string& raw_line, char* title) {
    // trim 首尾空白（空格 / 全角空格 U+3000 / \t \r）
    size_t s = 0, e = raw_line.size();
    auto is_ws = [&](size_t i, uint8_t* adv) -> bool {
        uint32_t cp = Utf8Decode(raw_line.data(), i, raw_line.size(), adv);
        return cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x3000;
    };
    while (s < e) {
        uint8_t a;
        if (!is_ws(s, &a)) break;
        s += a;
    }
    // 尾部空白：简单按 ASCII 处理即可
    while (e > s && (raw_line[e - 1] == ' ' || raw_line[e - 1] == '\r' ||
                     raw_line[e - 1] == '\t'))
        e--;
    if (e <= s) return false;
    std::string t = raw_line.substr(s, e - s);

    // 解码为码点数组（限前 40 个，标题很短）
    uint32_t cps[40];
    int nc = 0;
    for (size_t i = 0; i < t.size() && nc < 40;) {
        uint8_t adv;
        cps[nc++] = Utf8Decode(t.data(), i, t.size(), &adv);
        i += adv;
    }
    if (nc == 0 || nc > 32) return false;

    bool matched = false;
    // 模式 a：第 [数字/中文数字]+ [单位]
    if (cps[0] == 0x7B2C /*第*/) {
        int digits = 0;
        for (int i = 1; i < nc && i <= 12; i++) {
            uint32_t cp = cps[i];
            if ((cp >= '0' && cp <= '9') || IsCjkNumeral(cp)) {
                digits++;
            } else if (IsChapterUnit(cp)) {
                if (digits > 0) matched = true;
                break;
            } else {
                break;
            }
        }
    }
    // 模式 b：常见短标题
    if (!matched && nc <= 6) {
        static const char* kTitles[] = {"序",   "序章", "楔子", "引子", "前言",
                                        "后记", "尾声", "番外", "自序", "后序"};
        for (const char* k : kTitles) {
            if (t.rfind(k, 0) == 0) {
                matched = true;
                break;
            }
        }
    }
    if (!matched) return false;

    // 拷贝标题（按 UTF-8 边界截断到 63 字节）
    size_t cn = t.size();
    if (cn > 63) {
        cn = 63;
        while (cn > 0 && (static_cast<uint8_t>(t[cn]) & 0xC0) == 0x80) cn--;
    }
    std::memcpy(title, t.data(), cn);
    title[cn] = '\0';
    return true;
}

// ---- sidecar 路径 ----------------------------------------------------------
std::string SidecarPath(const BookInfo& book) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/%08lx.idx", book_store::kSidecarDir,
             static_cast<unsigned long>(book.hash));
    return buf;
}

// sidecar 头（打包写入，字段小端）。
struct SidecarHeader {
    char magic[4];  // 'M','E','B','K'
    uint16_t version;
    uint8_t encoding;
    uint8_t flags;  // bit0 = has_real_chapters
    uint32_t file_size;
    uint32_t name_hash;
    uint32_t chapter_count;
    // ---- v2 起 ----
    uint32_t virt_size;     // 进度坐标系总长（TXT = file_size）
    uint8_t format;         // BookFormat
    uint8_t extractor_ver;  // 内容抽取算法版本（EPUB；TXT 恒 0）
    uint16_t reserved;
};

}  // namespace

TextEncoding DetectEncoding(const BookInfo& book, uint32_t* bom_skip) {
    *bom_skip = 0;
    FILE* f = fopen(book.path.c_str(), "rb");
    if (f == nullptr) return TextEncoding::kUtf8;
    uint8_t* buf = static_cast<uint8_t*>(PsramAlloc(kDetectBytes));
    if (buf == nullptr) {
        fclose(f);
        return TextEncoding::kUtf8;
    }
    size_t n = fread(buf, 1, kDetectBytes, f);
    fclose(f);

    TextEncoding enc = TextEncoding::kUtf8;
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        *bom_skip = 3;
        enc = TextEncoding::kUtf8;
    } else if (!IsValidUtf8(buf, n)) {
        enc = TextEncoding::kGbk;
    }
    heap_caps_free(buf);
    ESP_LOGI(TAG, "%s encoding=%s bom=%lu", book.name.c_str(),
             enc == TextEncoding::kGbk ? "GBK" : "UTF-8", static_cast<unsigned long>(*bom_skip));
    return enc;
}

namespace {

// 流式扫描全文，产出章节起点 + 标题。
bool ScanChapters(const BookInfo& book, TextEncoding enc, uint32_t bom_skip,
                  std::vector<ChapterEntry>& chapters, bool& has_real) {
    FILE* f = fopen(book.path.c_str(), "rb");
    if (f == nullptr) return false;
    if (bom_skip) fseek(f, bom_skip, SEEK_SET);

    char* chunk = static_cast<char*>(PsramAlloc(kScanChunk));
    if (chunk == nullptr) {
        fclose(f);
        return false;
    }

    std::vector<std::pair<uint32_t, std::string>> heads;  // (file_off, title)
    std::string carry;                                    // 跨块残行
    uint32_t line_off = bom_skip;                         // 当前残行起点的文件偏移
    uint32_t pos = bom_skip;
    char title[64];

    auto handle_line = [&](const std::string& line_bytes, uint32_t off) {
        // 章节标题都很短：> 80 字节的行必是正文，直接跳过，避免为每个正文段
        // 做一次 GBK→UTF-8 转换（大书扫描的主要开销）。
        if (line_bytes.size() > 80 || line_bytes.empty()) return;
        // 转成 UTF-8 短行再匹配
        std::string utf8;
        if (enc == TextEncoding::kGbk) {
            size_t ol = 0;
            char* c = GbkToUtf8(reinterpret_cast<const uint8_t*>(line_bytes.data()),
                                line_bytes.size(), &ol);
            if (c) {
                utf8.assign(c, ol);
                heap_caps_free(c);
            }
        } else {
            utf8 = line_bytes;
        }
        if (MatchChapterTitle(utf8, title)) {
            heads.emplace_back(off, std::string(title));
        }
    };

    size_t rd;
    while ((rd = fread(chunk, 1, kScanChunk, f)) > 0) {
        for (size_t i = 0; i < rd; i++) {
            if (chunk[i] == '\n') {
                handle_line(carry, line_off);
                carry.clear();
                line_off = pos + static_cast<uint32_t>(i) + 1;
            } else {
                carry.push_back(chunk[i]);
            }
        }
        pos += static_cast<uint32_t>(rd);
    }
    if (!carry.empty()) handle_line(carry, line_off);
    heap_caps_free(chunk);
    fclose(f);

    uint32_t fsize = book.size;
    chapters.clear();

    if (heads.size() < 2) {
        // 无有效章节：按块切分
        has_real = false;
        uint32_t off = bom_skip;
        int n = 0;
        while (off < fsize) {
            uint32_t end = off + kBlockBytes;
            if (end > fsize) end = fsize;
            ChapterEntry ce;
            ce.byte_start = off;
            ce.byte_end = end;
            snprintf(ce.title, sizeof(ce.title), "片段 %d", ++n);
            chapters.push_back(ce);
            off = end;
        }
        if (chapters.empty()) {  // 空文件
            ChapterEntry ce;
            ce.byte_start = bom_skip;
            ce.byte_end = fsize;
            snprintf(ce.title, sizeof(ce.title), "正文");
            chapters.push_back(ce);
        }
        return true;
    }

    has_real = true;
    // 卷首（第一章前的非空前言）
    if (heads[0].first > bom_skip + 64) {
        ChapterEntry ce;
        ce.byte_start = bom_skip;
        ce.byte_end = heads[0].first;
        snprintf(ce.title, sizeof(ce.title), "卷首");
        chapters.push_back(ce);
    }
    for (size_t i = 0; i < heads.size(); i++) {
        uint32_t start = heads[i].first;
        uint32_t end = (i + 1 < heads.size()) ? heads[i + 1].first : fsize;
        if (end <= start) continue;
        // 超大章按块细分
        if (end - start > kMaxChapterBytes) {
            uint32_t off = start;
            int part = 0;
            while (off < end) {
                uint32_t e2 = off + kBlockBytes;
                if (e2 > end) e2 = end;
                ChapterEntry ce;
                ce.byte_start = off;
                ce.byte_end = e2;
                if (part == 0) {
                    strlcpy(ce.title, heads[i].second.c_str(), sizeof(ce.title));
                } else {
                    snprintf(ce.title, sizeof(ce.title), "%.48s (%d)", heads[i].second.c_str(),
                             part + 1);
                }
                chapters.push_back(ce);
                off = e2;
                part++;
            }
        } else {
            ChapterEntry ce;
            ce.byte_start = start;
            ce.byte_end = end;
            strlcpy(ce.title, heads[i].second.c_str(), sizeof(ce.title));
            chapters.push_back(ce);
        }
    }
    return true;
}

}  // namespace

bool LoadIndexCache(const BookInfo& book, ChapterIndex& out, uint8_t extractor_ver) {
    std::string path = SidecarPath(book);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    SidecarHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return false;
    }
    if (h.magic[0] != 'M' || h.magic[1] != 'E' || h.magic[2] != 'B' || h.magic[3] != 'K' ||
        h.version != kSidecarVersion || h.file_size != book.size ||
        h.format != static_cast<uint8_t>(book.format) || h.extractor_ver != extractor_ver) {
        fclose(f);
        return false;
    }
    out.encoding = static_cast<TextEncoding>(h.encoding);
    out.file_size = h.file_size;
    out.virt_size = h.virt_size;
    out.name_hash = h.name_hash;
    out.has_real_chapters = (h.flags & 0x01) != 0;
    out.chapters.resize(h.chapter_count);
    if (h.chapter_count > 0 &&
        fread(out.chapters.data(), sizeof(ChapterEntry), h.chapter_count, f) != h.chapter_count) {
        fclose(f);
        out.chapters.clear();
        return false;
    }
    fclose(f);
    ESP_LOGI(TAG, "cache hit: %s (%d chapters)", book.name.c_str(),
             static_cast<int>(out.chapters.size()));
    return true;
}

void SaveIndexCache(const BookInfo& book, const ChapterIndex& idx, uint8_t extractor_ver) {
    std::string path = SidecarPath(book);
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "sidecar write open failed: %s", path.c_str());
        return;
    }
    SidecarHeader h = {};
    h.magic[0] = 'M';
    h.magic[1] = 'E';
    h.magic[2] = 'B';
    h.magic[3] = 'K';
    h.version = kSidecarVersion;
    h.encoding = static_cast<uint8_t>(idx.encoding);
    h.flags = idx.has_real_chapters ? 0x01 : 0x00;
    h.file_size = idx.file_size;
    h.name_hash = idx.name_hash;
    h.chapter_count = static_cast<uint32_t>(idx.chapters.size());
    h.virt_size = idx.virt_size;
    h.format = static_cast<uint8_t>(book.format);
    h.extractor_ver = extractor_ver;
    fwrite(&h, sizeof(h), 1, f);
    if (!idx.chapters.empty())
        fwrite(idx.chapters.data(), sizeof(ChapterEntry), idx.chapters.size(), f);
    fclose(f);
}

bool OpenBook(const BookInfo& book, ChapterIndex& out) {
    if (book.format == BookFormat::kEpub) return epub_source::OpenBook(book, out);

    if (LoadIndexCache(book, out)) return true;

    uint32_t bom_skip = 0;
    TextEncoding enc = DetectEncoding(book, &bom_skip);
    std::vector<ChapterEntry> chapters;
    bool has_real = false;
    if (!ScanChapters(book, enc, bom_skip, chapters, has_real)) {
        ESP_LOGW(TAG, "scan failed: %s", book.name.c_str());
        return false;
    }
    out.encoding = enc;
    out.file_size = book.size;
    out.virt_size = book.size;  // TXT：进度坐标系即文件字节
    out.name_hash = book_store::BookKeyHash(book.name, 0);  // 仅取文件名部分做二次校验
    out.has_real_chapters = has_real;
    out.chapters = std::move(chapters);
    ESP_LOGI(TAG, "scanned %s: %d chapters (real=%d)", book.name.c_str(),
             static_cast<int>(out.chapters.size()), has_real ? 1 : 0);
    SaveIndexCache(book, out);
    return true;
}

bool LoadChapter(const BookInfo& book, const ChapterIndex& idx, int chapter_idx,
                 PaginatedChapter& out) {
    if (book.format == BookFormat::kEpub)
        return epub_source::LoadChapter(book, idx, chapter_idx, out);

    if (chapter_idx < 0 || chapter_idx >= static_cast<int>(idx.chapters.size())) return false;
    const ChapterEntry& ch = idx.chapters[chapter_idx];
    uint32_t start = ch.byte_start;
    uint32_t len = (ch.byte_end > start) ? (ch.byte_end - start) : 0;
    if (len == 0) return false;

    FILE* f = fopen(book.path.c_str(), "rb");
    if (f == nullptr) return false;
    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint8_t* raw = static_cast<uint8_t*>(PsramAlloc(len));
    if (raw == nullptr) {
        fclose(f);
        return false;
    }
    size_t got = fread(raw, 1, len, f);
    fclose(f);

    out.FreeBuf();
    out.chapter_idx = chapter_idx;
    out.encoding = idx.encoding;
    out.chapter_file_start = start;

    if (idx.encoding == TextEncoding::kGbk) {
        size_t ol = 0;
        char* u = GbkToUtf8(raw, got, &ol);
        heap_caps_free(raw);
        if (u == nullptr) return false;
        out.utf8_buf = u;
        out.utf8_len = ol;
    } else {
        // UTF-8：raw 即正文切片（buf 字节偏移 == 文件偏移 - start）。
        out.utf8_buf = reinterpret_cast<char*>(raw);
        out.utf8_len = got;
    }
    return true;
}

}  // namespace book_source
