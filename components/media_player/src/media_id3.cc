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
        // 孤立/未配对代理项（高位代理无合法低位跟随，或独立出现的低位代理）：
        // 输出替换符 U+FFFD，绝不把 D800–DFFF 直接编成 ill-formed 的 3 字节 UTF-8。
        if (u >= 0xD800 && u <= 0xDFFF) {
            AppendUtf8Cp(out, 0xFFFD);
        } else {
            AppendUtf8Cp(out, u);  // 合法 BMP 码点直译
        }
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
                TrimTrailingNul(out);  // enc=0 单字节 NUL 终止符会被转码带出，去掉
            } else {
                out = Latin1ToUtf8(text, len);
                TrimTrailingNul(out);
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

// 判定 offset 处是否像"下一帧起点"：正好落在 tag 尾（remaining==0）、是 padding
// （首字节 0x00）、或是合法帧头（4 个大写字母/数字）。只读 4 字节、无循环。供
// v2.4 非 syncsafe 帧长启发式选边用。会移动 f 的读位置——调用方随后自行重定位。
bool BoundaryLooksValid(FILE* f, long offset, long remaining) {
    if (remaining < 0) return false;
    if (remaining == 0) return true;  // 该帧正好铺满 tag 区
    if (std::fseek(f, offset, SEEK_SET) != 0) return false;
    uint8_t b[4];
    size_t g = std::fread(b, 1, 4, f);
    if (g >= 1 && b[0] == 0x00) return true;  // padding 起点
    if (g < 4) return false;
    return IsFrameIdByte(b[0]) && IsFrameIdByte(b[1]) && IsFrameIdByte(b[2]) && IsFrameIdByte(b[3]);
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

        long frame_body_offset = std::ftell(f);
        if (frame_body_offset < 0) break;

        // 帧长：v2.3 为普通 32 位大端；v2.4 名义 syncsafe，但相当多编码器（iTunes
        // 时代等）实际写成普通大端。两种解释不一致时做"落点校验"选边——syncsafe
        // 落点非法而 raw 落点合法（下一帧头 / padding / 正好 tag 尾）才改用 raw，
        // 默认仍取 spec 规定的 syncsafe。BoundaryLooksValid 会移动读位置，下面各
        // 解析分支都自行绝对重定位，不依赖当前位置。
        uint32_t frame_size;
        if (major == 4) {
            uint32_t ss = SyncsafeDecode(fh + 4);
            uint32_t raw = Be32(fh + 4);
            frame_size = ss;
            if (raw != ss && raw <= static_cast<uint32_t>(budget)) {
                bool ss_ok = ss <= static_cast<uint32_t>(budget) &&
                            BoundaryLooksValid(f, frame_body_offset + static_cast<long>(ss),
                                              budget - static_cast<long>(ss));
                bool raw_ok = BoundaryLooksValid(f, frame_body_offset + static_cast<long>(raw),
                                                budget - static_cast<long>(raw));
                if (raw_ok && !ss_ok) frame_size = raw;
            }
        } else {
            frame_size = Be32(fh + 4);
        }
        if (frame_size > static_cast<uint32_t>(budget)) break;  // 声明尺寸越界：畸形，停止

        // 帧格式标志（fh[9]）：压缩/加密/帧级 unsync 的帧无法当明文解，跳过不解析
        //（仍按 frame_size 正常推进，不误当 padding）；grouping 帧体前有 1 字节
        // group id；v2.4 data-length-indicator 帧体前有 4 字节原始长度——都要偏移帧体。
        bool skip_parse = false;
        long body_skip = 0;
        uint8_t fflags = fh[9];
        if (major == 4) {
            if (fflags & 0x0C) skip_parse = true;  // 0x08 压缩 | 0x04 加密
            if (fflags & 0x02) skip_parse = true;  // 帧级 unsynchronisation（本解析器不去同步）
            if (fflags & 0x40) body_skip += 1;     // grouping identity
            if (fflags & 0x01) body_skip += 4;     // data length indicator（同步安全原始长度）
        } else {                                   // major == 3
            if (fflags & 0xC0) skip_parse = true;  // 0x80 压缩 | 0x40 加密
            if (fflags & 0x20) body_skip += 1;     // grouping identity
        }
        long content_off = frame_body_offset + body_skip;
        uint32_t content_size = (!skip_parse && body_skip <= static_cast<long>(frame_size))
                                    ? frame_size - static_cast<uint32_t>(body_skip)
                                    : 0;

        bool is_text = tags != nullptr && !want_cover && content_size > 0 &&
                      (std::strcmp(id, "TIT2") == 0 || std::strcmp(id, "TALB") == 0 ||
                       std::strcmp(id, "TPE1") == 0);
        bool is_apic = want_cover && cover != nullptr && !found_cover && content_size > 0 &&
                      std::strcmp(id, "APIC") == 0;

        if (is_text) {
            size_t to_read = content_size < kMaxTextFrame ? content_size : kMaxTextFrame;
            std::vector<uint8_t> buf(to_read);
            size_t got = 0;
            if (to_read > 0 && std::fseek(f, content_off, SEEK_SET) == 0) {
                got = std::fread(buf.data(), 1, to_read, f);
            }
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
            if (ParseApicHeader(f, content_off, content_size, &loc)) {
                *cover = loc;
                found_cover = true;
            }
        }

        // 统一跳到下一帧起点（各解析分支的 fread/fseek 可能已把文件位置挪到别处，
        // 这里不依赖它，直接算绝对偏移跳转）。始终用原始 frame_size 推进。
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

// ---------------------------------------------------------------------------
// MP3 时长探测（见头文件契约）
// ---------------------------------------------------------------------------
namespace {

// MPEG Layer III 帧头解析。b 指向 4 字节候选帧头；合法返回 true 并填出参。
struct MpegHdr {
    bool v1;           // true = MPEG1（1152 样本/帧），false = MPEG2/2.5（576）
    int bitrate_kbps;  // 0 = free format（不支持，判非法）
    int hz;
    bool mono;
};

bool ParseMpegHeader(const uint8_t* b, MpegHdr* out) {
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
    int ver = (b[1] >> 3) & 3;    // 0=2.5 1=保留 2=2 3=1
    int layer = (b[1] >> 1) & 3;  // 1 = Layer III
    if (ver == 1 || layer != 1) return false;
    int bidx = (b[2] >> 4) & 0xF;
    int sidx = (b[2] >> 2) & 3;
    if (bidx == 0 || bidx == 15 || sidx == 3) return false;  // free format / 非法
    static const int kBrV1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int kBrV2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const int kSr[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    out->v1 = (ver == 3);
    out->bitrate_kbps = out->v1 ? kBrV1[bidx] : kBrV2[bidx];
    out->hz = kSr[out->v1 ? 0 : (ver == 2 ? 1 : 2)][sidx];
    out->mono = ((b[3] >> 6) & 3) == 3;
    return true;
}

// Layer III 帧字节长（含 padding）。
int MpegFrameLen(const MpegHdr& h, const uint8_t* b) {
    int pad = (b[2] >> 1) & 1;
    int coef = h.v1 ? 144000 : 72000;
    return coef * h.bitrate_kbps / h.hz + pad;
}

}  // namespace

int ProbeDurationS(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return 0;

    long fsize = 0;
    if (std::fseek(f, 0, SEEK_END) != 0 || (fsize = std::ftell(f)) <= 0) {
        std::fclose(f);
        return 0;
    }

    // 音频区起点：跳过 ID3v2 tag（含 v2.4 footer）。
    long audio_start = 0;
    uint8_t hdr[10];
    if (std::fseek(f, 0, SEEK_SET) == 0 && std::fread(hdr, 1, 10, f) == 10 &&
        std::memcmp(hdr, "ID3", 3) == 0) {
        uint32_t tag_size = SyncsafeDecode(hdr + 6);
        if (tag_size > kMaxTagBudget) tag_size = kMaxTagBudget;
        audio_start = 10 + static_cast<long>(tag_size) + ((hdr[5] & 0x10) ? 10 : 0);
    }
    // 音频区终点：去掉 ID3v1 尾（末尾 128 字节 "TAG"）。
    long audio_end = fsize;
    if (fsize >= 128) {
        uint8_t t[3];
        if (std::fseek(f, fsize - 128, SEEK_SET) == 0 && std::fread(t, 1, 3, f) == 3 &&
            std::memcmp(t, "TAG", 3) == 0)
            audio_end -= 128;
    }
    if (audio_start >= audio_end) {
        std::fclose(f);
        return 0;
    }

    // 在音频区前 64KB 内找首个"本帧头合法且推算的下一帧头也合法（或已出窗）"的同步点。
    constexpr size_t kScanMax = 64 * 1024;
    size_t scan = static_cast<size_t>(audio_end - audio_start);
    if (scan > kScanMax) scan = kScanMax;
    std::vector<uint8_t> buf(scan);
    if (std::fseek(f, audio_start, SEEK_SET) != 0 || std::fread(buf.data(), 1, scan, f) != scan) {
        std::fclose(f);
        return 0;
    }
    std::fclose(f);

    for (size_t i = 0; i + 4 <= scan; i++) {
        MpegHdr h;
        if (!ParseMpegHeader(buf.data() + i, &h)) continue;
        int flen = MpegFrameLen(h, buf.data() + i);
        if (flen < 24) continue;  // 病态小帧：当伪同步跳过
        if (i + flen + 4 <= scan) {
            MpegHdr next;
            if (!ParseMpegHeader(buf.data() + i + flen, &next)) continue;  // 伪同步，继续扫
        }
        const int spf = h.v1 ? 1152 : 576;

        // VBR：Xing/Info 帧（帧头 + side info 之后）带总帧数。
        size_t side = h.v1 ? (h.mono ? 17 : 32) : (h.mono ? 9 : 17);
        size_t xing = i + 4 + side;
        if (xing + 8 <= scan && (std::memcmp(buf.data() + xing, "Xing", 4) == 0 ||
                                 std::memcmp(buf.data() + xing, "Info", 4) == 0)) {
            uint32_t flags = Be32(buf.data() + xing + 4);
            if ((flags & 1) != 0 && xing + 12 <= scan) {
                uint64_t frames = Be32(buf.data() + xing + 8);
                return static_cast<int>(frames * spf / static_cast<uint32_t>(h.hz));
            }
        }
        // VBRI 帧（Fraunhofer，帧头后固定偏移 32）：+14 处总帧数。
        size_t vbri = i + 4 + 32;
        if (vbri + 18 <= scan && std::memcmp(buf.data() + vbri, "VBRI", 4) == 0) {
            uint64_t frames = Be32(buf.data() + vbri + 14);
            return static_cast<int>(frames * spf / static_cast<uint32_t>(h.hz));
        }
        // CBR 估算：音频区字节 × 8 / 码率。
        int64_t audio_bytes = audio_end - audio_start - static_cast<long>(i);
        return static_cast<int>(audio_bytes * 8 / (h.bitrate_kbps * 1000));
    }
    return 0;
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

uint8_t* NormalizeJpegHeader(const uint8_t* data, size_t len, size_t* out_len) {
    if (data == nullptr || len < 4) return nullptr;
    if (!(data[0] == 0xFF && data[1] == 0xD8)) return nullptr;  // 非 JPEG SOI（PNG 等不动）

    // LVGL tjpgd 包装层 is_jpg() 要求的精确签名：SOI + JFIF APP0（长度恰 0x0010）。
    static const uint8_t kJfifSig[10] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46};
    if (len >= 10 && std::memcmp(data, kJfifSig, sizeof(kJfifSig)) == 0) return nullptr;  // 已合规

    // 合成标准 18 字节 JFIF APP0（version 1.1、units=0、密度 1x1、无缩略图），插到
    // SOI 之后。前 10 字节即 kJfifSig，使 is_jpg() 放行；底层 jd_prepare 会把随后
    // 原有的 APP1/Exif 等未知段按 default 跳过，正常解 baseline JPEG。
    static const uint8_t kApp0[18] = {0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00,
                                      0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    size_t nlen = len + sizeof(kApp0);
    uint8_t* out = static_cast<uint8_t*>(std::malloc(nlen));
    if (out == nullptr) return nullptr;
    out[0] = 0xFF;
    out[1] = 0xD8;  // SOI
    std::memcpy(out + 2, kApp0, sizeof(kApp0));
    std::memcpy(out + 2 + sizeof(kApp0), data + 2, len - 2);  // 原 SOI 之后的全部字节
    if (out_len != nullptr) *out_len = nlen;
    return out;
}

}  // namespace media_id3
