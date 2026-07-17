#include "media_player/media_id3.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "media_player/media_textconv.h"

namespace media_id3 {

namespace {

constexpr size_t kMaxTextFrame = 4096;       // 文本帧只读前 N 字节用于解码
constexpr size_t kMaxTagBudget = 4 * 1024 * 1024;  // 声明的 tag_size 防御性上限
constexpr size_t kMaxCoverBytes = 4 * 1024 * 1024;  // ReadCover 成功返回的上限
constexpr size_t kMaxHeaderScan = 300;  // APIC 帧头（mime+desc）扫描上限

bool IsFrameIdByte(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

uint32_t SyncsafeDecode(const uint8_t b[4]) {
    return (static_cast<uint32_t>(b[0] & 0x7F) << 21) | (static_cast<uint32_t>(b[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(b[2] & 0x7F) << 7) | static_cast<uint32_t>(b[3] & 0x7F);
}

uint32_t Be32(const uint8_t b[4]) {
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

// ---- UTF-8 合法性校验（宽松：够用于"是不是已经是 UTF-8"的判定） -----------
bool IsValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) {
            i++;
            continue;
        }
        int extra;
        if ((b & 0xE0) == 0xC0) extra = 1;
        else if ((b & 0xF0) == 0xE0) extra = 2;
        else if ((b & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + static_cast<size_t>(extra) >= n) return false;  // 尾部截断：续字节不够
        for (int k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

// ---- GBK 双字节判定：高位字节对是否落在合法 GBK 区间 ----------------------
bool LooksLikeGbk(const uint8_t* p, size_t n) {
    if (n < 2) return false;
    size_t i = 0;
    bool found_pair = false;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) {
            i++;
            continue;
        }
        if (b < 0x81 || b > 0xFE) return false;  // 非法前导字节
        if (i + 1 >= n) return false;             // 落单高位字节
        uint8_t t = p[i + 1];
        if (t < 0x40 || t > 0xFE || t == 0x7F) return false;
        found_pair = true;
        i += 2;
    }
    return found_pair;
}

void AppendUtf8Cp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string Latin1ToUtf8(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) AppendUtf8Cp(out, p[i]);
    return out;
}

// UTF-16（BOM 已消费/大小端已定）-> UTF-8，处理代理对；奇数尾字节丢弃。
std::string Utf16ToUtf8(const uint8_t* p, size_t n, bool big_endian) {
    std::string out;
    size_t i = 0;
    while (i + 1 < n) {
        uint16_t u = big_endian ? static_cast<uint16_t>((p[i] << 8) | p[i + 1])
                                : static_cast<uint16_t>((p[i + 1] << 8) | p[i]);
        i += 2;
        if (u == 0x0000) break;  // 字符串终止符
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            uint16_t lo = big_endian ? static_cast<uint16_t>((p[i] << 8) | p[i + 1])
                                     : static_cast<uint16_t>((p[i + 1] << 8) | p[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                uint32_t cp = 0x10000 + ((static_cast<uint32_t>(u - 0xD800) << 10) + (lo - 0xDC00));
                AppendUtf8Cp(out, cp);
                i += 2;
                continue;
            }
        }
        AppendUtf8Cp(out, u);  // BMP 直译（含孤立代理项的退化处理，不崩即可）
    }
    return out;
}

// 去掉尾部 NUL（编码 0/3 单字节 NUL；1/2 双字节 NUL 已在 Utf16ToUtf8 内部按
// 字符串终止符处理，这里只需再兜底单字节情形）。
void TrimTrailingNul(std::string& s) {
    while (!s.empty() && s.back() == '\0') s.pop_back();
}

// 文本帧体（encoding 字节 + 内容）解码为 UTF-8。
std::string DecodeTextFrame(const uint8_t* buf, size_t n) {
    if (n == 0) return {};
    uint8_t enc = buf[0];
    const uint8_t* text = buf + 1;
    size_t len = n - 1;
    std::string out;
    switch (enc) {
        case 1: {  // UTF-16 + BOM
            bool big_endian = false;
            if (len >= 2 && text[0] == 0xFE && text[1] == 0xFF) {
                big_endian = true;
                text += 2;
                len -= 2;
            } else if (len >= 2 && text[0] == 0xFF && text[1] == 0xFE) {
                big_endian = false;
                text += 2;
                len -= 2;
            }
            out = Utf16ToUtf8(text, len, big_endian);
            break;
        }
        case 2:  // UTF-16BE，无 BOM
            out = Utf16ToUtf8(text, len, true);
            break;
        case 3:  // UTF-8
            out.assign(reinterpret_cast<const char*>(text), len);
            TrimTrailingNul(out);
            break;
        case 0:
        default:
            // ISO-8859-1 标称，实际常是 GBK（国内标签工具惯例）：UTF-8 合法性优先，
            // 否则按 GBK 字节对启发式判定，都不是则退化 Latin-1 升码点。
            if (IsValidUtf8(text, len)) {
                out.assign(reinterpret_cast<const char*>(text), len);
                TrimTrailingNul(out);
            } else if (LooksLikeGbk(text, len)) {
                out = media_textconv::GbkToUtf8(text, len);
            } else {
                out = Latin1ToUtf8(text, len);
            }
            break;
    }
    return out;
}

struct CoverLoc {
    long offset = -1;  // 文件内绝对偏移（图片数据起点）
    size_t size = 0;
    std::string mime;
};

// APIC 帧体（frame_body_offset 处，长度 frame_size，文件当前位置已在
// frame_body_offset）解析出 mime + 图片数据的偏移/长度。解析失败返回 false
// （不设 out）。f 的文件位置在返回时不保证——调用方随后统一 fseek 到下一帧。
bool ParseApicHeader(FILE* f, long frame_body_offset, uint32_t frame_size, CoverLoc* out) {
    if (frame_size < 2) return false;  // 至少 encoding(1)+终止符(1)
    uint8_t hdr[kMaxHeaderScan];
    size_t to_read = frame_size < kMaxHeaderScan ? frame_size : kMaxHeaderScan;
    if (std::fseek(f, frame_body_offset, SEEK_SET) != 0) return false;
    size_t got = std::fread(hdr, 1, to_read, f);
    if (got < 2) return false;

    uint8_t enc = hdr[0];
    size_t i = 1;
    // mime：null 结尾 ASCII，最长到 got-1
    size_t mime_start = i;
    while (i < got && hdr[i] != 0x00) i++;
    if (i >= got) return false;  // 没找到终止符（在扫描窗口内）
    std::string mime(reinterpret_cast<const char*>(hdr + mime_start), i - mime_start);
    i++;  // 跳过 mime 的 NUL

    if (i >= got) return false;  // picture type 字节
    i++;                         // 跳过 picture type

    // description：encoding 1/2 为双字节 NUL 终止，否则单字节。双字节扫描必须
    // 按 UTF-16 code unit 边界（步进 2）比对，否则形如 "r\x00\x00\x00"（'r' 的
    // 高位零字节 + 真正终止符的首字节）会在字节级扫描中产生假的零字节对，把
    // 终止符判早 1 字节（Stage E fuzz/夹具测试中曾用真实 APIC 帧复现过）。
    bool wide_nul = (enc == 1 || enc == 2);
    if (wide_nul) {
        while (i + 1 < got && !(hdr[i] == 0x00 && hdr[i + 1] == 0x00)) i += 2;
        if (i + 1 >= got) return false;
        i += 2;
    } else {
        while (i < got && hdr[i] != 0x00) i++;
        if (i >= got) return false;
        i++;
    }

    if (i > frame_size) return false;  // 消耗超过帧体本身（畸形）
    out->offset = frame_body_offset + static_cast<long>(i);
    out->size = frame_size - i;
    out->mime = mime;
    return true;
}

// 共享帧遍历：want_cover=false 只找 TIT2/TALB/TPE1；true 只找 APIC（两次调用
// 各自独立 fopen，逻辑简单、频率低（ReadCover 只在换曲时调一次），不值得为
// 省一次文件打开合并成一趟多回调遍历。
bool WalkFrames(const std::string& path, bool want_cover, Tags* tags, CoverLoc* cover) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;

    uint8_t hdr[10];
    bool ok = std::fread(hdr, 1, 10, f) == 10 && std::memcmp(hdr, "ID3", 3) == 0;
    if (!ok) {
        std::fclose(f);
        return false;
    }
    uint8_t major = hdr[3];
    uint8_t flags = hdr[5];
    if (major < 3 || major > 4) {  // v2.2 不支持；未来版本不猜
        std::fclose(f);
        return false;
    }
    uint32_t tag_size = SyncsafeDecode(hdr + 6);
    if (tag_size > kMaxTagBudget) tag_size = kMaxTagBudget;  // 防御性钳制
    long budget = static_cast<long>(tag_size);

    if (flags & 0x40) {  // extended header：读 4 字节 size，跳过
        uint8_t eb[4];
        if (std::fread(eb, 1, 4, f) != 4) {
            std::fclose(f);
            return false;
        }
        budget -= 4;
        long ext_skip = (major == 4) ? static_cast<long>(SyncsafeDecode(eb)) - 4
                                     : static_cast<long>(Be32(eb));
        if (ext_skip < 0 || ext_skip > budget) {
            std::fclose(f);
            return false;
        }
        if (ext_skip > 0 && std::fseek(f, ext_skip, SEEK_CUR) != 0) {
            std::fclose(f);
            return false;
        }
        budget -= ext_skip;
    }

    bool found_cover = false;
    while (budget >= 10) {
        uint8_t fh[10];
        if (std::fread(fh, 1, 10, f) != 10) break;
        budget -= 10;
        if (fh[0] == 0x00) break;  // padding 起点
        if (!IsFrameIdByte(fh[0]) || !IsFrameIdByte(fh[1]) || !IsFrameIdByte(fh[2]) ||
            !IsFrameIdByte(fh[3])) {
            break;  // 非法帧 id：判定进入 padding/损坏区，停止（保留已解出内容）
        }
        char id[5] = {static_cast<char>(fh[0]), static_cast<char>(fh[1]), static_cast<char>(fh[2]),
                     static_cast<char>(fh[3]), '\0'};
        uint32_t frame_size = (major == 4) ? SyncsafeDecode(fh + 4) : Be32(fh + 4);
        if (frame_size > static_cast<uint32_t>(budget)) break;  // 声明尺寸越界：畸形，停止

        long frame_body_offset = std::ftell(f);
        if (frame_body_offset < 0) break;

        bool is_text = tags != nullptr && !want_cover &&
                      (std::strcmp(id, "TIT2") == 0 || std::strcmp(id, "TALB") == 0 ||
                       std::strcmp(id, "TPE1") == 0);
        bool is_apic = want_cover && cover != nullptr && !found_cover && std::strcmp(id, "APIC") == 0;

        if (is_text) {
            size_t to_read = frame_size < kMaxTextFrame ? frame_size : kMaxTextFrame;
            std::vector<uint8_t> buf(to_read);
            size_t got = to_read > 0 ? std::fread(buf.data(), 1, to_read, f) : 0;
            if (got == to_read) {
                std::string decoded = DecodeTextFrame(buf.data(), got);
                if (!decoded.empty()) {
                    if (std::strcmp(id, "TIT2") == 0 && tags->title.empty()) tags->title = decoded;
                    else if (std::strcmp(id, "TALB") == 0 && tags->album.empty()) tags->album = decoded;
                    else if (std::strcmp(id, "TPE1") == 0 && tags->artist.empty()) tags->artist = decoded;
                    tags->has_any = true;
                }
            }
        } else if (is_apic) {
            CoverLoc loc;
            if (ParseApicHeader(f, frame_body_offset, frame_size, &loc)) {
                *cover = loc;
                found_cover = true;
            }
        }

        // 统一跳到下一帧起点（is_text/is_apic 分支里的 fread/fseek 可能已经把
        // 文件位置挪到别处，这里不依赖它，直接算绝对偏移跳转）。
        if (std::fseek(f, frame_body_offset + static_cast<long>(frame_size), SEEK_SET) != 0) break;
        budget -= static_cast<long>(frame_size);
    }

    std::fclose(f);
    return true;
}

}  // namespace

Tags ReadTags(const std::string& path) {
    Tags tags;
    WalkFrames(path, /*want_cover=*/false, &tags, nullptr);
    return tags;
}

uint8_t* ReadCover(const std::string& path, size_t* out_size, std::string* out_mime) {
    if (out_size != nullptr) *out_size = 0;
    CoverLoc loc;
    bool ok = WalkFrames(path, /*want_cover=*/true, nullptr, &loc);
    if (!ok || loc.offset < 0 || loc.size == 0 || loc.size > kMaxCoverBytes) return nullptr;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return nullptr;
    if (std::fseek(f, loc.offset, SEEK_SET) != 0) {
        std::fclose(f);
        return nullptr;
    }
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(loc.size));
    if (buf == nullptr) {
        std::fclose(f);
        return nullptr;
    }
    size_t got = std::fread(buf, 1, loc.size, f);
    std::fclose(f);
    if (got != loc.size) {
        std::free(buf);
        return nullptr;
    }
    if (out_size != nullptr) *out_size = loc.size;
    if (out_mime != nullptr) *out_mime = loc.mime;
    return buf;
}

bool PeekImageSize(const uint8_t* data, size_t len, int* out_w, int* out_h) {
    if (data == nullptr || len < 8) return false;

    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        if (len < 24) return false;
        // 签名 8B + 长度 4B + "IHDR" 4B + width(4BE) + height(4BE)
        if (std::memcmp(data + 12, "IHDR", 4) != 0) return false;
        uint32_t w = Be32(data + 16);
        uint32_t h = Be32(data + 20);
        if (w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF) return false;
        *out_w = static_cast<int>(w);
        *out_h = static_cast<int>(h);
        return true;
    }

    if (data[0] == 0xFF && data[1] == 0xD8) {  // JPEG SOI
        size_t pos = 2;
        int guard = 0;
        while (pos + 4 <= len && guard++ < 200) {
            if (data[pos] != 0xFF) {  // 失同步：不再是合法 marker 序列
                pos++;
                continue;
            }
            uint8_t marker = data[pos + 1];
            if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
                (marker >= 0xD0 && marker <= 0xD7)) {
                pos += 2;
                continue;
            }
            uint32_t seglen = (static_cast<uint32_t>(data[pos + 2]) << 8) | data[pos + 3];
            if (seglen < 2) return false;
            bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                         marker != 0xCC;
            if (is_sof) {
                if (pos + 4 + 5 > len) return false;
                uint32_t h = (static_cast<uint32_t>(data[pos + 5]) << 8) | data[pos + 6];
                uint32_t w = (static_cast<uint32_t>(data[pos + 7]) << 8) | data[pos + 8];
                if (w == 0 || h == 0) return false;
                *out_w = static_cast<int>(w);
                *out_h = static_cast<int>(h);
                return true;
            }
            pos += 2 + seglen;
        }
        return false;
    }

    return false;
}

}  // namespace media_id3
