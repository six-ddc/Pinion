// book_store.h
// 书架扫描 + 阅读进度 / 设置持久化。纯逻辑，无 lv_obj。
//
// 目录约定：书籍放 /sdcard/books/*.{txt,epub}；章节索引/封面缓存放 /sdcard/books/.metalio/。
// 进度与设置存 NVS namespace "ebook"（复用项目的 Settings 封装）。

#ifndef BOOK_STORE_H
#define BOOK_STORE_H

#include "ebook_models.h"

#include <cstdint>
#include <string>
#include <vector>

namespace book_store {

constexpr const char* kBooksDir = "/sdcard/books";
constexpr const char* kSidecarDir = "/sdcard/books/.metalio";

// FNV1a-32(name + ":" + size)：既作 NVS 进度 key，也作 sidecar 文件名。
// 文件被同名不同大小的内容替换时 key 变化 → 进度自然重置。
uint32_t BookKeyHash(const std::string& name, uint32_t size);

// 扫描 /sdcard/books/*.{txt,epub}（目录不存在则创建）。SD 未挂载返回空。
// 每本填好 name/path/size/hash/format 并回读进度百分比。按书名升序。
std::vector<BookInfo> ScanBooks();

// 阅读进度：偏移按书的进度坐标系（TXT=文件字节 / EPUB=虚拟线性偏移）记录，
// 另存百分比供书架直接展示（EPUB 的偏移无法用 off/文件大小 推算）。未读过返回 0。
uint32_t LoadProgress(uint32_t hash);
void SaveProgress(uint32_t hash, uint32_t off, int pct);
// 书架用：存过 pct 直接返回；老数据无 pct 时 TXT 回退 off/size 估算；均无返回 -1。
int LoadProgressPct(uint32_t hash, BookFormat format, uint32_t file_size);

// 全局阅读设置。
ReaderSettings LoadSettings();
void SaveSettings(const ReaderSettings& s);

// 上次打开的书路径（用于将来「继续阅读」快捷入口；M1 只写不用）。
std::string LoadLastBook();
void SaveLastBook(const std::string& path);

}  // namespace book_store

#endif  // BOOK_STORE_H
