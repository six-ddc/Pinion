// zip_reader.cc — 极简只读 ZIP（EPUB 用）：文件尾扫 EOCD → 读中央目录 → 按项解压
// （stored / raw deflate）。不支持 ZIP64 / 加密 / 分卷。所有多字节字段小端，逐字节
// 组装读取（对齐安全）。大缓冲一律堆分配（PSRAM 优先）。

#include "zip_reader.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace zip_reader {

namespace {

constexpr char TAG[] = "zip_reader";

// EOCD 固定 22 字节 + 最长 65535 注释，从文件尾扫这么多字节足够找到它。
constexpr size_t kEocdMaxScan = 22 + 65535;
constexpr size_t kInBuf = 32 * 1024;  // deflate 分块输入缓冲

constexpr uint32_t kSigEocd = 0x06054b50;     // End Of Central Directory
constexpr uint32_t kSigCentral = 0x02014b50;  // 中央目录记录
constexpr uint32_t kSigLocal = 0x04034b50;    // 局部文件头

// PSRAM 分配：优先 SPIRAM，失败回退内部 RAM（照抄 book_source.cc 的写法）。
void* PsramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

// 逐字节小端读取（不做指针强转，避免未对齐访问）。
uint16_t ReadU16(const uint8_t* p, size_t off) {
    return static_cast<uint16_t>(p[off] | (static_cast<uint16_t>(p[off + 1]) << 8));
}
uint32_t ReadU32(const uint8_t* p, size_t off) {
    return static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8) |
           (static_cast<uint32_t>(p[off + 2]) << 16) | (static_cast<uint32_t>(p[off + 3]) << 24);
}

}  // namespace

bool ZipReader::Open(const std::string& path) {
    path_ = path;
    entries_.clear();

    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "open failed: %s", path.c_str());
        return false;
    }

    // 取文件大小
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long fsize_l = ftell(f);
    if (fsize_l < 22) {  // 连一个 EOCD 都放不下
        ESP_LOGW(TAG, "too small (%ld): %s", fsize_l, path.c_str());
        fclose(f);
        return false;
    }
    uint64_t fsize = static_cast<uint64_t>(fsize_l);

    // 读文件尾部若干字节，从后向前扫 EOCD
    size_t tail = kEocdMaxScan;
    if (tail > fsize) tail = static_cast<size_t>(fsize);
    uint8_t* buf = static_cast<uint8_t*>(PsramAlloc(tail));
    if (buf == nullptr) {
        fclose(f);
        return false;
    }
    if (fseek(f, static_cast<long>(fsize - tail), SEEK_SET) != 0 || fread(buf, 1, tail, f) != tail) {
        ESP_LOGW(TAG, "tail read failed: %s", path.c_str());
        heap_caps_free(buf);
        fclose(f);
        return false;
    }

    // 从 tail-22 向前找签名；注释长度须与文件尾对齐，避免误命中数据里的签名字节。
    long eocd = -1;
    for (long i = static_cast<long>(tail) - 22; i >= 0; i--) {
        if (ReadU32(buf, i) != kSigEocd) continue;
        uint16_t comment_len = ReadU16(buf, i + 20);
        if (static_cast<size_t>(i) + 22 + comment_len == tail) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        ESP_LOGW(TAG, "EOCD not found: %s", path.c_str());
        heap_caps_free(buf);
        fclose(f);
        return false;
    }

    uint16_t total_entries = ReadU16(buf, eocd + 10);
    uint32_t cd_size = ReadU32(buf, eocd + 12);
    uint32_t cd_off = ReadU32(buf, eocd + 16);
    heap_caps_free(buf);

    // ZIP64 标志：不支持
    if (total_entries == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF) {
        ESP_LOGW(TAG, "ZIP64 unsupported: %s", path.c_str());
        fclose(f);
        return false;
    }
    if (cd_size == 0 || static_cast<uint64_t>(cd_off) + cd_size > fsize) {
        ESP_LOGW(TAG, "bad central dir (off=%lu size=%lu file=%llu): %s",
                 static_cast<unsigned long>(cd_off), static_cast<unsigned long>(cd_size),
                 static_cast<unsigned long long>(fsize), path.c_str());
        fclose(f);
        return false;
    }

    // 读中央目录进内存后即关闭文件（不长持 FILE*）
    uint8_t* cd = static_cast<uint8_t*>(PsramAlloc(cd_size));
    if (cd == nullptr) {
        fclose(f);
        return false;
    }
    if (fseek(f, static_cast<long>(cd_off), SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size) {
        ESP_LOGW(TAG, "central dir read failed: %s", path.c_str());
        heap_caps_free(cd);
        fclose(f);
        return false;
    }
    fclose(f);

    // 逐条解析中央目录记录（固定部分 46 字节）
    entries_.reserve(total_entries);
    size_t p = 0;
    for (uint32_t n = 0; n < total_entries; n++) {
        if (p + 46 > cd_size || ReadU32(cd, p) != kSigCentral) break;  // 越界 / 损坏
        uint16_t flag = ReadU16(cd, p + 8);
        uint16_t method = ReadU16(cd, p + 10);
        uint32_t crc = ReadU32(cd, p + 16);
        uint32_t comp = ReadU32(cd, p + 20);
        uint32_t uncomp = ReadU32(cd, p + 24);
        uint16_t name_len = ReadU16(cd, p + 28);
        uint16_t extra_len = ReadU16(cd, p + 30);
        uint16_t comment_len = ReadU16(cd, p + 32);
        uint32_t local_off = ReadU32(cd, p + 42);
        size_t name_off = p + 46;
        if (name_off + name_len > cd_size) break;  // 损坏

        std::string name(reinterpret_cast<const char*>(cd + name_off), name_len);
        p = name_off + name_len + extra_len + comment_len;  // 下一条

        // 过滤：加密位（bit0）/ 不支持的压缩法 / 目录条目 / 单条 ZIP64
        if (flag & 0x0001) continue;
        if (method != 0 && method != 8) continue;
        if (!name.empty() && name.back() == '/') continue;
        if (comp == 0xFFFFFFFF || uncomp == 0xFFFFFFFF || local_off == 0xFFFFFFFF) continue;

        Entry e;
        e.name = std::move(name);
        e.comp_size = comp;
        e.uncomp_size = uncomp;
        e.local_hdr_off = local_off;
        e.crc32 = crc;
        e.method = method;
        entries_.push_back(std::move(e));
    }
    heap_caps_free(cd);

    ESP_LOGI(TAG, "opened %s: %d entries", path.c_str(), static_cast<int>(entries_.size()));
    return true;
}

int ZipReader::Find(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

uint8_t* ZipReader::Extract(int idx, size_t* out_len, size_t max_bytes) const {
    if (out_len) *out_len = 0;
    if (idx < 0 || idx >= static_cast<int>(entries_.size())) return nullptr;
    const Entry& e = entries_[idx];
    if (max_bytes > 0 && e.uncomp_size > max_bytes) {
        ESP_LOGW(TAG, "entry too large: %s %lu > %lu", e.name.c_str(),
                 static_cast<unsigned long>(e.uncomp_size), static_cast<unsigned long>(max_bytes));
        return nullptr;
    }

    FILE* f = fopen(path_.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "reopen failed: %s", path_.c_str());
        return nullptr;
    }

    // 局部头固定 30 字节；name/extra 长度可能与中央目录不同，须按局部头的值跳过。
    uint8_t lh[30];
    if (fseek(f, static_cast<long>(e.local_hdr_off), SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 ||
        ReadU32(lh, 0) != kSigLocal) {
        ESP_LOGW(TAG, "bad local header: %s", e.name.c_str());
        fclose(f);
        return nullptr;
    }
    uint16_t l_name = ReadU16(lh, 26);
    uint16_t l_extra = ReadU16(lh, 28);
    uint64_t data_off = static_cast<uint64_t>(e.local_hdr_off) + 30 + l_name + l_extra;
    if (fseek(f, static_cast<long>(data_off), SEEK_SET) != 0) {
        fclose(f);
        return nullptr;
    }

    // 输出 buffer（uncomp_size 为 0 时也分配 1 字节保证非空指针）
    size_t alloc = e.uncomp_size ? e.uncomp_size : 1;
    uint8_t* out = static_cast<uint8_t*>(PsramAlloc(alloc));
    if (out == nullptr) {
        fclose(f);
        return nullptr;
    }

    bool ok = false;
    size_t produced = 0;

    if (e.method == 0) {
        // stored：直读 uncomp_size 字节
        produced = fread(out, 1, e.uncomp_size, f);
        ok = (produced == e.uncomp_size);
        if (!ok) ESP_LOGW(TAG, "stored short read: %s", e.name.c_str());
    } else {
        // deflate：raw（-MAX_WBITS），分块 fread + inflate 到整块输出
        z_stream strm = {};
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
            ESP_LOGW(TAG, "inflateInit2 failed: %s", e.name.c_str());
            heap_caps_free(out);
            fclose(f);
            return nullptr;
        }
        uint8_t* inbuf = static_cast<uint8_t*>(PsramAlloc(kInBuf));
        if (inbuf == nullptr) {
            inflateEnd(&strm);
            heap_caps_free(out);
            fclose(f);
            return nullptr;
        }
        strm.next_out = out;
        strm.avail_out = static_cast<uInt>(alloc);
        uint32_t remaining = e.comp_size;
        int ret = Z_OK;
        do {
            if (strm.avail_in == 0 && remaining > 0) {
                size_t chunk = remaining < kInBuf ? remaining : kInBuf;
                size_t got = fread(inbuf, 1, chunk, f);
                if (got == 0) break;  // 意外 EOF
                remaining -= static_cast<uint32_t>(got);
                strm.next_in = inbuf;
                strm.avail_in = static_cast<uInt>(got);
            }
            ret = inflate(&strm, Z_NO_FLUSH);
        } while (ret == Z_OK);
        produced = alloc - strm.avail_out;
        inflateEnd(&strm);
        heap_caps_free(inbuf);
        ok = (ret == Z_STREAM_END);
        if (!ok) ESP_LOGW(TAG, "inflate failed (%d): %s", ret, e.name.c_str());
    }
    fclose(f);

    // 校验长度 + crc32
    if (ok && produced != e.uncomp_size) {
        ESP_LOGW(TAG, "size mismatch: %s %lu != %lu", e.name.c_str(),
                 static_cast<unsigned long>(produced), static_cast<unsigned long>(e.uncomp_size));
        ok = false;
    }
    if (ok) {
        uLong c = crc32(0L, Z_NULL, 0);
        c = crc32(c, out, static_cast<uInt>(produced));
        if (static_cast<uint32_t>(c) != e.crc32) {
            ESP_LOGW(TAG, "crc mismatch: %s %08lx != %08lx", e.name.c_str(),
                     static_cast<unsigned long>(c), static_cast<unsigned long>(e.crc32));
            ok = false;
        }
    }
    if (!ok) {
        heap_caps_free(out);
        return nullptr;
    }

    if (out_len) *out_len = produced;
    return out;
}

uint8_t* ZipReader::ExtractByName(const std::string& name, size_t* out_len, size_t max_bytes) const {
    return Extract(Find(name), out_len, max_bytes);
}

}  // namespace zip_reader
