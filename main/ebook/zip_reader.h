// zip_reader.h
// 极简只读 ZIP（EPUB 用）：文件尾扫 EOCD → 读中央目录 → 按项解压（stored / raw
// deflate，用 zlib inflateInit2(-MAX_WBITS)）。不支持 ZIP64 / 加密 / 分卷。
// 解压输出 PSRAM（PsramAlloc 语义：优先 SPIRAM，失败回退内部 RAM）。
// 纯逻辑，无 lv_obj；在 ebook_worker 线程使用。不长持 FILE*（Open/Extract 各自开关文件，
// 避免 SD 拔卡后握着失效句柄）。

#ifndef ZIP_READER_H
#define ZIP_READER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zip_reader {

struct Entry {
    std::string name;        // 原样存储（EPUB 内路径分隔符本就是 '/'）
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint32_t local_hdr_off = 0;  // 局部文件头偏移（解压时再解析局部头跳过 name/extra）
    uint32_t crc32 = 0;
    uint16_t method = 0;  // 0=stored 8=deflate；其余不支持
};

class ZipReader {
   public:
    // 解析中央目录。失败（非 ZIP / 损坏 / ZIP64）返回 false。可重复调用（重置状态）。
    bool Open(const std::string& path);

    const std::vector<Entry>& entries() const { return entries_; }

    // 按名字精确查找（大小写敏感）；找不到返回 -1。
    int Find(const std::string& name) const;

    // 解压第 idx 项到新分配的 PSRAM buffer，*out_len = 实际字节数（= uncomp_size）。
    // 校验 crc32，不符视为失败。max_bytes > 0 时超限拒绝（解压炸弹保险丝）。
    // 失败返回 nullptr。调用方 heap_caps_free 释放。
    uint8_t* Extract(int idx, size_t* out_len, size_t max_bytes = 0) const;

    // Extract(Find(name), ...) 便捷形态。
    uint8_t* ExtractByName(const std::string& name, size_t* out_len, size_t max_bytes = 0) const;

   private:
    std::string path_;
    std::vector<Entry> entries_;
};

}  // namespace zip_reader

#endif  // ZIP_READER_H
