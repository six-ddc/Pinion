// font_mem_vfs.h
// 只读内存文件 VFS（挂载点 "/fmem"）：把已读入 PSRAM 的字体数据暴露成普通文件路径，供
// FreeType 的 ANSI ftsystem（fopen/fseek/fread）零 IO 访问。
//
// 为什么需要它：v9 的 esp_lv_adapter 不支持内存字体（只认文件路径），而 SD 上的字体文件被
// FreeType 按需随机读（每个新字形几次 fseek+fread → FatFS → SD），是外挂字体卡顿的根因。
// 把整份字体读进 PSRAM 后注册到本 VFS，FreeType 的所有读操作退化为 PSRAM memcpy。
//
// ⚠️ 依赖 FreeType 走 newlib ANSI ftsystem —— 仅当 CONFIG_LV_FREETYPE_USE_LVGL_PORT 关闭时成立；
// 若将来打开该选项，FreeType 会改用 LVGL FS 层（drive letter）绕开 newlib VFS，本模块失效。
//
// 数据所有权：Add 只登记外部指针（由 ebook_font 的 blob 持有并负责 free）；Remove 只解除登记。
// 生命周期由调用方（fence/graveyard）保证：Remove 前该文件的所有 fd 必已 close（FT_Done_Face）。

#ifndef FONT_MEM_VFS_H
#define FONT_MEM_VFS_H

#include <cstddef>
#include <cstdint>

namespace font_mem_vfs {

// 幂等注册 "/fmem" VFS。成功（或已注册）返回 true。
bool Register();

// 登记一个内存文件（name 如 "g1.ttf"，data 生命周期由调用方保证 ≥ 对应字体存活期）。
// 表满 / 重名 / 未注册 → false。
bool Add(const char* name, const uint8_t* data, size_t size);

// 解除登记（无此项则忽略）。调用前须保证无未关闭 fd 引用它。
void Remove(const char* name);

// 拼出可传给 FreeType 的完整路径（"/fmem/<name>"）。
void PathFor(const char* name, char* out, size_t out_size);

}  // namespace font_mem_vfs

#endif  // FONT_MEM_VFS_H
