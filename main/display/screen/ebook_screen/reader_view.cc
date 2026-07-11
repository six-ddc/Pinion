// reader_view.cc — 3 页滑窗 + 跟手翻页 + 惯性动画。见头文件说明。

#include "reader_view.h"

#include "ebook_ui_theme.h"
#include "screen_util.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace reader_view {

namespace {

using ebook_ui::Hex;

constexpr int16_t kPanel = ebook_ui::kPanelW;  // 720
constexpr size_t kLineBufMax = 640;

// 手势阈值
constexpr int kMoveSlop = 8;      // 判定进入拖动的最小位移
constexpr int kCommitPx = 200;    // 位移超过即提交
constexpr int kFlickMs = 240;     // 快速轻扫时限
constexpr int kFlickPx = 60;      // 快速轻扫最小位移
constexpr uint32_t kAnimMs = 220;
constexpr uint32_t kTapAnimMs = 150;
constexpr float kRubber = 0.35f;

Callbacks s_cb;
uint8_t s_theme_idx = 1;
PageMetrics s_metrics;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_slot[3] = {nullptr, nullptr, nullptr};  // 三格容器
int s_center = 1;                                   // 当前中页所在容器下标
lv_obj_t* s_loading = nullptr;

// 拖动会话
struct Drag {
    bool active = false;
    bool horizontal = false;
    bool decided = false;
    int start_x = 0;
    int start_y = 0;
    uint32_t tick = 0;
    bool animating = false;
} s_drag;

int SlotLeft() { return (s_center + 2) % 3; }
int SlotRight() { return (s_center + 1) % 3; }

// 按拖动偏移 dx 摆放三格（中=dx，右=720+dx，左=-720+dx）。
void PositionSlots(int dx) {
    if (s_slot[0] == nullptr) return;
    lv_obj_set_x(s_slot[s_center], dx);
    lv_obj_set_x(s_slot[SlotRight()], kPanel + dx);
    lv_obj_set_x(s_slot[SlotLeft()], -kPanel + dx);
}

// 把一页内容渲染进指定容器（清空重建）。pc=null → 空白。
void MountPage(lv_obj_t* slot, const SlotContent& sc) {
    lv_obj_clean(slot);
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);
    lv_obj_set_style_bg_color(slot, Hex(th.bg), LV_PART_MAIN);
    if (sc.pc == nullptr) return;
    const PaginatedChapter& pc = *sc.pc;
    if (sc.page_idx < 0 || sc.page_idx >= static_cast<int>(pc.pages.size())) return;

    // 页眉：章节名（居中）+ 进度%（右上角），构成状态栏
    lv_obj_t* header = lv_label_create(slot);
    lv_label_set_text(header, pc.title);
    lv_label_set_long_mode(header, LV_LABEL_LONG_DOT);
    lv_obj_set_width(header, kPanel - 200);  // 两侧收窄，给右上角进度让位、避免重叠
    lv_obj_set_style_text_align(header, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(header, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(header, Hex(th.dim), LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 22);

    // 进度%：移到右上角（原底部页脚区腾出给正文）
    lv_obj_t* footer = lv_label_create(slot);
    lv_label_set_text(footer, sc.footer);
    lv_obj_set_style_text_font(footer, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer, Hex(th.dim), LV_PART_MAIN);
    lv_obj_align(footer, LV_ALIGN_TOP_RIGHT, -24, 22);

    // 正文（三种行：文本 / 标题 / 图片）
    int16_t margin_h = (kPanel - s_metrics.content_w) / 2;
    const PageSpan& pg = pc.pages[sc.page_idx];
    int y = ebook_ui::kHeaderH;
    char tmp[kLineBufMax];
    for (uint16_t k = 0; k < pg.line_count; k++) {
        const LineSpan& ls = pc.lines[pg.first_line + k];
        bool para_first = (ls.flags & kLineFlagParaFirst) != 0;
        if (k > 0 && para_first) y += s_metrics.para_space;

        // ---- 图片行 ----
        if (ls.flags & kLineFlagImage) {
            const ImageRef* ir = pc.FindImage(ls.off);
            int iw = (ir != nullptr && ir->disp_w > 0) ? ir->disp_w : 320;
            int ih = (ir != nullptr && ir->disp_h > 0) ? ir->disp_h : 180;
            y += kImageVMargin;
            if (ir != nullptr && ir->px != nullptr) {
                lv_obj_t* img = lv_image_create(slot);
                lv_image_set_src(img, &ir->dsc);
                lv_obj_set_pos(img, (kPanel - iw) / 2, y);
            } else {
                // placeholder：细描边圆角框 + 弱化文字（解码失败/格式不支持/超预算）
                lv_obj_t* ph = lv_obj_create(slot);
                screen_strip_obj_chrome(ph);
                lv_obj_set_size(ph, iw, ih);
                lv_obj_set_pos(ph, (kPanel - iw) / 2, y);
                lv_obj_set_style_radius(ph, 10, LV_PART_MAIN);
                lv_obj_set_style_border_width(ph, 1, LV_PART_MAIN);
                lv_obj_set_style_border_color(ph, Hex(th.dim), LV_PART_MAIN);
                lv_obj_set_style_border_opa(ph, LV_OPA_40, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(ph, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_t* pl = lv_label_create(ph);
                lv_label_set_text(pl, "图片");
                lv_obj_set_style_text_font(pl, &font_puhui_16_4, LV_PART_MAIN);
                lv_obj_set_style_text_color(pl, Hex(th.dim), LV_PART_MAIN);
                lv_obj_center(pl);
            }
            y += ih + kImageVMargin;
            continue;
        }

        // ---- 文本 / 标题行 ----
        bool heading = (ls.flags & kLineFlagHeading) != 0;
        const lv_font_t* font =
            (heading && s_metrics.heading_font != nullptr) ? s_metrics.heading_font : s_metrics.font;
        int16_t lh = (heading && s_metrics.heading_line_height > 0) ? s_metrics.heading_line_height
                                                                    : s_metrics.line_height;
        // 段首缩进：字库里 U+3000 是零宽空字形，缩进不能靠字符实现，改用真实 x 偏移。
        // 偏移量 = paginator 为该行预留的 indent_w，两侧同源 → 首行右边界正好落在 content_w。
        int16_t indent = (ls.flags & kLineFlagIndent) ? s_metrics.indent_w : 0;
        bool center = (ls.flags & kLineFlagCenter) != 0;

        // 建一个文本片段 label：[off,off+len) 一段，颜色 color，行首 x = xpos。
        // need_w=true 才测并返回渲染宽度（仅强调拆段衔接 x 时需要；FT 下整行度量不便宜，
        // 单 label 行 / 末段不白测）。
        auto emit_seg = [&](uint32_t off, uint32_t len, lv_color_t color, int16_t xpos,
                            bool need_w) -> int16_t {
            size_t n = len;
            if (n > kLineBufMax - 1) n = kLineBufMax - 1;
            std::memcpy(tmp, pc.utf8_buf + off, n);
            tmp[n] = '\0';
            lv_obj_t* lbl = lv_label_create(slot);
            lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
            lv_label_set_text(lbl, tmp);
            if (center) {
                lv_obj_set_width(lbl, s_metrics.content_w);
                lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            }
            lv_obj_set_pos(lbl, xpos, y);
            return need_w ? static_cast<int16_t>(lv_text_get_width(tmp, n, font, 0)) : 0;
        };

        int16_t x0 = margin_h + indent;
        // 居中行（标题）或无强调：整行一个 label（强调换色在标题上不拆，保持居中简单）。
        if (center || pc.emphasis.empty()) {
            emit_seg(ls.off, ls.len, Hex(th.text), x0, false);
        } else {
            // 正文行：按强调旁表把行拆成「常规/强调」片段，强调段用 accent 色。emphasis 按 off
            // 升序、不重叠 → 二分定位首个与本行相交的区间，再顺序推进。
            uint32_t lo = ls.off, hi = ls.off + ls.len;
            int loi = 0, hii = static_cast<int>(pc.emphasis.size()) - 1;
            size_t si = pc.emphasis.size();
            while (loi <= hii) {  // 首个 end>lo 的区间
                int mid = (loi + hii) / 2;
                if (pc.emphasis[mid].off + pc.emphasis[mid].len > lo) {
                    si = mid;
                    hii = mid - 1;
                } else {
                    loi = mid + 1;
                }
            }
            uint32_t cur = lo;
            int16_t x = x0;
            while (cur < hi) {
                uint32_t seg_end;
                lv_color_t col;
                if (si < pc.emphasis.size() && pc.emphasis[si].off <= cur) {
                    uint32_t se = pc.emphasis[si].off + pc.emphasis[si].len;  // 强调段内
                    seg_end = se < hi ? se : hi;
                    col = Hex(th.accent);
                } else if (si < pc.emphasis.size() && pc.emphasis[si].off < hi) {
                    seg_end = pc.emphasis[si].off;  // 强调段前的常规段
                    col = Hex(th.text);
                } else {
                    seg_end = hi;  // 本行剩余无强调
                    col = Hex(th.text);
                }
                if (seg_end > cur) x += emit_seg(cur, seg_end - cur, col, x, seg_end < hi);
                cur = seg_end;
                if (si < pc.emphasis.size() && cur >= pc.emphasis[si].off + pc.emphasis[si].len) si++;
            }
        }
        y += lh + s_metrics.line_space;
    }
    // 新建的子对象默认可点击会拦截手势；整格设为输入穿透，事件落到 s_root。
    screen_make_input_passive(slot);
}

// ---- 动画 ------------------------------------------------------------------
int s_anim_commit_dir = 0;  // 动画结束后要提交的方向（0=取消/回弹）

void AnimExec(void* /*var*/, int32_t v) { PositionSlots(v); }

void OnAnimDone(lv_anim_t* /*a*/) {
    s_drag.animating = false;
    int dir = s_anim_commit_dir;
    s_anim_commit_dir = 0;
    if (dir == 0) {
        PositionSlots(0);
        return;
    }
    SlotContent incoming;
    if (s_cb.on_commit && s_cb.on_commit(dir, &incoming)) {
        if (dir > 0) {
            s_center = SlotRight();          // 老右 → 新中
            MountPage(s_slot[SlotRight()], incoming);  // 老左 → 新右，换入
        } else {
            s_center = SlotLeft();           // 老左 → 新中
            MountPage(s_slot[SlotLeft()], incoming);   // 老右 → 新左，换入
        }
    }
    PositionSlots(0);
}

void AnimateTo(int target, int commit_dir, uint32_t ms) {
    s_anim_commit_dir = commit_dir;
    s_drag.animating = true;
    int cur_x = lv_obj_get_x(s_slot[s_center]);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, AnimExec);
    lv_anim_set_values(&a, cur_x, target);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, OnAnimDone);
    lv_anim_start(&a);
}

// ---- 手势 ------------------------------------------------------------------
void GetPoint(lv_event_t* e, int* x, int* y) {
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        *x = *y = 0;
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    *x = p.x;
    *y = p.y;
}

void OnPressed(lv_event_t* e) {
    if (s_drag.animating) return;
    int x, y;
    GetPoint(e, &x, &y);
    s_drag.active = true;
    s_drag.horizontal = false;
    s_drag.decided = false;
    s_drag.start_x = x;
    s_drag.start_y = y;
    s_drag.tick = lv_tick_get();
}

void OnPressing(lv_event_t* e) {
    if (!s_drag.active || s_drag.animating) return;
    int x, y;
    GetPoint(e, &x, &y);
    int dx = x - s_drag.start_x;
    int dy = y - s_drag.start_y;
    if (!s_drag.decided) {
        if (std::abs(dx) < kMoveSlop && std::abs(dy) < kMoveSlop) return;
        s_drag.decided = true;
        s_drag.horizontal = std::abs(dx) >= std::abs(dy);
    }
    if (!s_drag.horizontal) return;
    // 目标方向不可翻 → 阻尼
    int dir = dx < 0 ? 1 : -1;
    int eff = dx;
    if (s_cb.can_turn && !s_cb.can_turn(dir)) eff = static_cast<int>(dx * kRubber);
    PositionSlots(eff);
}

void OnReleased(lv_event_t* e) {
    if (!s_drag.active) return;
    s_drag.active = false;
    if (s_drag.animating) return;
    int x, y;
    GetPoint(e, &x, &y);
    int dx = x - s_drag.start_x;
    int dy = y - s_drag.start_y;
    uint32_t elapsed = lv_tick_elaps(s_drag.tick);

    // 轻点：按 x 分区
    if (!s_drag.horizontal && std::abs(dx) < kMoveSlop && std::abs(dy) < kMoveSlop) {
        if (x < 180) {
            if (s_cb.can_turn && s_cb.can_turn(-1)) AnimateTo(kPanel, -1, kTapAnimMs);
        } else if (x >= kPanel - 180) {
            if (s_cb.can_turn && s_cb.can_turn(1)) AnimateTo(-kPanel, 1, kTapAnimMs);
        } else {
            if (s_cb.on_menu) s_cb.on_menu();
        }
        return;
    }
    if (!s_drag.horizontal) return;

    int dir = dx < 0 ? 1 : -1;
    bool can = s_cb.can_turn && s_cb.can_turn(dir);
    bool commit = can && (std::abs(dx) > kCommitPx ||
                          (elapsed < kFlickMs && std::abs(dx) > kFlickPx));
    if (commit) {
        AnimateTo(dir > 0 ? -kPanel : kPanel, dir, kAnimMs);
    } else {
        AnimateTo(0, 0, kAnimMs);  // 回弹
    }
}

}  // namespace

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb, uint8_t theme_idx) {
    s_cb = cb;
    s_theme_idx = theme_idx;
    s_center = 1;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(theme_idx);

    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_set_size(s_root, kPanel, ebook_ui::kPanelH);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, Hex(th.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    // reader 拥有水平拖动语义：整棵子树不参与 screen 级右滑返回。
    screen_swipe_back_ignore(s_root, true);
    // 但 s_root 本身要可点击，才能作为命中目标收到 PRESSED/PRESSING/RELEASED。
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, OnPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_root, OnPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_root, OnReleased, LV_EVENT_RELEASED, nullptr);

    for (int i = 0; i < 3; i++) {
        s_slot[i] = lv_obj_create(s_root);
        screen_strip_obj_chrome(s_slot[i]);
        lv_obj_set_size(s_slot[i], kPanel, ebook_ui::kPanelH);
        lv_obj_set_style_bg_color(s_slot[i], Hex(th.bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_slot[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(s_slot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_slot[i], LV_OBJ_FLAG_CLICKABLE);
    }
    PositionSlots(0);

    s_loading = lv_label_create(s_root);
    lv_label_set_text(s_loading, "");
    lv_obj_set_style_text_font(s_loading, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_loading, Hex(th.dim), LV_PART_MAIN);
    lv_obj_center(s_loading);
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏，宿主切换
    return s_root;
}

void SetInitial(const SlotContent& left, const SlotContent& cur, const SlotContent& right,
                const PageMetrics& m) {
    s_metrics = m;
    s_center = 1;
    HideLoading();
    MountPage(s_slot[SlotLeft()], left);
    MountPage(s_slot[s_center], cur);
    MountPage(s_slot[SlotRight()], right);
    PositionSlots(0);
}

void SetNeighbor(int dir, const SlotContent& sc) {
    if (s_slot[0] == nullptr) return;
    int idx = (dir > 0) ? SlotRight() : SlotLeft();
    MountPage(s_slot[idx], sc);
    PositionSlots(lv_obj_get_x(s_slot[s_center]));  // 保持当前偏移
}

void ShowLoading(const char* text) {
    if (s_loading == nullptr) return;
    lv_label_set_text(s_loading, text ? text : "");
    lv_obj_remove_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
}

void HideLoading() {
    if (s_loading) lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
}

void ClearSlots() {
    for (int i = 0; i < 3; i++) {
        if (s_slot[i] != nullptr) lv_obj_clean(s_slot[i]);
    }
}

void SetThemeIdx(uint8_t idx) {
    s_theme_idx = idx;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(idx);
    if (s_root) lv_obj_set_style_bg_color(s_root, Hex(th.bg), LV_PART_MAIN);
    if (s_loading) lv_obj_set_style_text_color(s_loading, Hex(th.dim), LV_PART_MAIN);
}

void Reset() {
    s_root = nullptr;
    s_slot[0] = s_slot[1] = s_slot[2] = nullptr;
    s_loading = nullptr;
    s_drag = Drag{};
    s_anim_commit_dir = 0;
}

}  // namespace reader_view
