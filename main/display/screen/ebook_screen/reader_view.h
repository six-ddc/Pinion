// reader_view.h
// 阅读视图（M3）：3 页容器滑窗（左/中/右 @ x=-720/0/+720）+ 跟手平移翻页 + 惯性动画。
// 每个容器是一整页（页眉章节名 + 正文 + 页脚进度），拖动时整体跟手，松手按位移/速度
// 决定补齐或回弹；提交后 3 容器角色轮转、仅重渲染换入的一格（carousel 复用）。
// 纯渲染 + 手势；翻页/跨章决策在宿主（通过 Callbacks 回调）。

#ifndef READER_VIEW_H
#define READER_VIEW_H

#include "ebook_models.h"
#include "lvgl.h"

namespace reader_view {

// 一格待渲染内容。pc=null 表示空格（书首/书尾之外，或相邻章未载入）。
struct SlotContent {
    const PaginatedChapter* pc = nullptr;
    int page_idx = 0;
    char footer[48] = {0};  // 宿主预格式化的页脚（进度%、页码）
};

struct Callbacks {
    // 用户提交一次翻页（dir=+1 下一页 / -1 上一页）。宿主据此推进内部定位，并把该方向
    // 「新换入侧」的页填入 *out_incoming（+1→新最右页，-1→新最左页）。返回 false 表示
    // 已到书首/书尾无法翻（reader 回弹）。
    bool (*on_commit)(int dir, SlotContent* out_incoming) = nullptr;
    // 查询某方向是否可翻（rubber-band 判定）。
    bool (*can_turn)(int dir) = nullptr;
    // 中区轻点：呼出菜单（M4）。
    void (*on_menu)() = nullptr;
};

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb, uint8_t theme_idx);

// 首次载入：设定三格内容并按排版参数渲染，复位到中页。
void SetInitial(const SlotContent& left, const SlotContent& cur, const SlotContent& right,
                const PageMetrics& m);

// 相邻章异步载入完成后，就地补渲一侧邻页（dir=-1 左 / +1 右）。
void SetNeighbor(int dir, const SlotContent& sc);

// 加载/错误态（居中文案，盖住三格）。
void ShowLoading(const char* text);
void HideLoading();

// 清空三格内容（删除其中的 lv 对象）。⚠️ 释放顺序硬约束：插图页的 lv_image 直接引用
// PaginatedChapter 拥有的像素/dsc，宿主在 FreeBuf 任何**可能已挂载**的章节前，必须先
// 调本函数（或确保对应格已被 MountPage 重挂载），否则 lv 对象析构/渲染读到已释放内存。
void ClearSlots();

void SetThemeIdx(uint8_t idx);
void Reset();

}  // namespace reader_view

#endif  // READER_VIEW_H
