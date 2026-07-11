// bookshelf_view.h
// 书架视图：顶栏 + 可滚动 3 列封面网格。纯渲染，暴露 Build/SetBooks/SetCover/Reset，
// 由宿主 ebook_screen 驱动。点击卡片回调 on_open_book(idx)。
// 封面：SetBooks 先给每本书画"生成式封面"（主题色卡 + 书名首字），宿主异步取到
// EPUB 封面缩略后用 SetCover 就地换成真图。

#ifndef BOOKSHELF_VIEW_H
#define BOOKSHELF_VIEW_H

#include "ebook_models.h"
#include "lvgl.h"

#include <cstddef>
#include <vector>

namespace bookshelf_view {

struct Callbacks {
    void (*on_open_book)(size_t idx) = nullptr;
    void (*on_back)() = nullptr;
    void (*on_manage)() = nullptr;  // 顶栏「管理」→ Web 后台入口
};

// 在 parent 下构建书架根容器，返回它。theme_idx 决定配色。
lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb, uint8_t theme_idx);

// 用扫描结果重建封面网格（空列表显示占位文案）。
void SetBooks(const std::vector<BookInfo>& books, bool sd_mounted);

// 把第 idx 本书的封面换成真图。dsc（含其 data 像素）由宿主持有并保证生命周期：
// 在下一次 SetBooks/Clear 之前必须有效。
void SetCover(size_t idx, const lv_image_dsc_t* dsc);

// 清空网格（删除卡片 lv 对象）。宿主释放封面像素前必须先调（同 reader 的释放纪律）。
void Clear();

// 卸载：清空 widget 引用。
void Reset();

}  // namespace bookshelf_view

#endif  // BOOKSHELF_VIEW_H
