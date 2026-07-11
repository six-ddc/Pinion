// reader_menu_view.h
// 阅读菜单遮罩（微信读书式）：中区轻点呼出。顶栏（返回书架 + 书名）+ 底部设置面板
// （进度条 + 上/下章 + 目录 + 主题三选 + 字号 A-/A+ + 行距/边距三档）+ 左侧目录抽屉。
// 纯 UI，所有决策通过 Callbacks 交给宿主；设置以档位下标回传。

#ifndef READER_MENU_VIEW_H
#define READER_MENU_VIEW_H

#include "ebook_models.h"
#include "lvgl.h"

#include <vector>

namespace reader_menu_view {

struct Callbacks {
    void (*on_close)() = nullptr;
    void (*on_back_to_shelf)() = nullptr;
    void (*on_seek)(int pct) = nullptr;          // 拖动进度条跳转（0~100）
    void (*on_prev_chapter)() = nullptr;
    void (*on_next_chapter)() = nullptr;
    void (*on_select_chapter)(int idx) = nullptr;  // 目录点选
    void (*on_set_theme)(int idx) = nullptr;
    void (*on_set_font)(int idx) = nullptr;          // 字号档 0/1/2/3
    void (*on_set_font_face)(const char* filename) = nullptr;  // 用户字体文件名（""=内置）
    void (*on_set_line_space)(int idx) = nullptr;
    void (*on_set_margin)(int idx) = nullptr;
};

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb);

// 呼出菜单：注入当前设置 / 书名 / 全书进度%。
void Show(const ReaderSettings& s, const char* book_name, int progress_pct);
void Hide();
bool IsOpen();

// 目录数据（供抽屉懒构建）。
void SetChapters(const std::vector<ChapterEntry>& chapters, int current_idx);

void SetThemeIdx(uint8_t idx);
void Reset();

}  // namespace reader_menu_view

#endif  // READER_MENU_VIEW_H
