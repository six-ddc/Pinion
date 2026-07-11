// reader_menu_view.cc — 阅读菜单遮罩 + 目录抽屉。见头文件说明。

#include "reader_menu_view.h"

#include "ebook_font.h"
#include "ebook_ui_theme.h"
#include "screen_util.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace reader_menu_view {

namespace {

using ebook_ui::Hex;

constexpr int16_t kPanel = ebook_ui::kPanelW;
constexpr int16_t kTopH = 84;
// 底部面板高度 = 上下内边距 + 7 行内容 + 6 个行距（较原布局新增「字体」行）：
//   padV(20)*2 + [进度56 + 百分比20 + 主题56 + 字体56 + 字号56 + 行距56 + 边距56] + gap(14)*6
//   = 40 + 356 + 84 = 480
constexpr int16_t kSheetPadH = 24;
constexpr int16_t kSheetPadV = 20;
constexpr int16_t kRowH = 56;
constexpr int16_t kRowGap = 14;
constexpr int16_t kSheetH = 480;
constexpr int16_t kTocW = kPanel / 3;  // 目录抽屉宽 = 屏宽 1/3（240）；章节字号小，够显示
constexpr int16_t kTocHeaderH = 76;
constexpr int16_t kTocItemH = 60;  // 每章行高（固定，虚拟化用）
constexpr int kTocPool = 14;       // 复用控件数 ≥ 可见行数(≈11) + 余量
constexpr uint32_t kAnimMs = 220;

Callbacks s_cb;
uint8_t s_theme_idx = 1;
bool s_open = false;

lv_obj_t* s_root = nullptr;    // 全屏透明容器（隐藏时不拦截）
lv_obj_t* s_scrim = nullptr;   // 半透明遮罩
lv_obj_t* s_topbar = nullptr;
lv_obj_t* s_sheet = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_toc_entry_btn = nullptr;  // 顶栏「目录」按钮（返回键右侧）
lv_obj_t* s_toc_entry_lbl = nullptr;
lv_obj_t* s_slider = nullptr;
lv_obj_t* s_pct_lbl = nullptr;

// 分段控件（字号 4 段 / 行距 3 段 / 边距 3 段）。
constexpr int kSegMax = 4;
struct Seg {
    lv_obj_t* btn[kSegMax] = {};
    int count = 0;
};
Seg s_seg_font, s_seg_ls, s_seg_margin;
lv_obj_t* s_swatch[3] = {nullptr, nullptr, nullptr};

// 字体循环选择器（◀ 名称 ▶）。files[i]=""表示内置；与 names[i] 平行。
lv_obj_t* s_font_name_lbl = nullptr;
std::vector<std::string> s_face_files;
std::vector<std::string> s_face_names;
int s_face_sel = 0;

// 目录抽屉（虚拟化：只保留 kTocPool 个可见项控件，滚动时复用 + 改文字，
// 对象数与章节数无关，几千章也不卡）。
lv_obj_t* s_toc = nullptr;
lv_obj_t* s_toc_list = nullptr;      // 滚动容器
lv_obj_t* s_toc_content = nullptr;   // 高 = N*kTocItemH 的虚拟画布，提供滚动范围
lv_obj_t* s_toc_item[kTocPool] = {};
lv_obj_t* s_toc_item_lbl[kTocPool] = {};
std::vector<ChapterEntry> s_chapters;
int s_toc_current = 0;
int s_toc_last_first = -1;  // 上次绑定的首个可见章下标（避免同行重复重绑）
bool s_toc_open = false;

lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---- 主题着色 --------------------------------------------------------------
void PaintSeg(Seg& seg, int sel) {
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);
    for (int i = 0; i < seg.count; i++) {
        if (seg.btn[i] == nullptr) continue;
        bool on = (i == sel);
        lv_obj_set_style_bg_color(seg.btn[i], on ? Hex(th.accent) : Hex(th.bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(seg.btn[i], on ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        lv_obj_t* lbl = lv_obj_get_child(seg.btn[i], 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl, on ? Hex(0xFFFFFF) : Hex(th.text), LV_PART_MAIN);
    }
}

// "特大"档仅 FreeType 用户字体可用（内置点阵无 50px）。据当前 FT 激活态启用/置灰该按钮。
void UpdateFontSizeAvail() {
    if (s_seg_font.count < 4 || s_seg_font.btn[3] == nullptr) return;
    bool ft = ebook_font::Active();
    lv_obj_t* btn = s_seg_font.btn[3];
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (ft) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        if (lbl) lv_obj_set_style_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_40, LV_PART_MAIN);
        if (lbl) lv_obj_set_style_opa(lbl, LV_OPA_40, LV_PART_MAIN);
    }
}

void ApplyMenuTheme(uint8_t idx) {
    s_theme_idx = idx;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(idx);
    if (s_topbar) lv_obj_set_style_bg_color(s_topbar, Hex(th.card_bg), LV_PART_MAIN);
    if (s_sheet) lv_obj_set_style_bg_color(s_sheet, Hex(th.card_bg), LV_PART_MAIN);
    if (s_toc) lv_obj_set_style_bg_color(s_toc, Hex(th.card_bg), LV_PART_MAIN);
    if (s_title) lv_obj_set_style_text_color(s_title, Hex(th.text), LV_PART_MAIN);
    if (s_toc_entry_btn) lv_obj_set_style_bg_color(s_toc_entry_btn, Hex(th.bg), LV_PART_MAIN);
    if (s_toc_entry_lbl) lv_obj_set_style_text_color(s_toc_entry_lbl, Hex(th.text), LV_PART_MAIN);
    if (s_pct_lbl) lv_obj_set_style_text_color(s_pct_lbl, Hex(th.dim), LV_PART_MAIN);
    if (s_slider) {
        lv_obj_set_style_bg_color(s_slider, Hex(th.accent), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider, Hex(th.accent), LV_PART_KNOB);
    }
    // 主题色板选中环
    for (int i = 0; i < 3; i++) {
        if (!s_swatch[i]) continue;
        bool on = (i == idx);
        lv_obj_set_style_border_width(s_swatch[i], on ? 4 : 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_swatch[i], on ? Hex(th.accent) : Hex(th.dim), LV_PART_MAIN);
    }
}

// ---- 动画（滑入/滑出 + 遮罩淡入）------------------------------------------
void ScrimOpaExec(void* /*v*/, int32_t val) {
    if (s_scrim) lv_obj_set_style_bg_opa(s_scrim, static_cast<lv_opa_t>(val), LV_PART_MAIN);
}
void TopYExec(void* /*v*/, int32_t val) {
    if (s_topbar) lv_obj_set_y(s_topbar, val);
}
void SheetYExec(void* /*v*/, int32_t val) {
    if (s_sheet) lv_obj_set_y(s_sheet, val);
}
void OnHideDone(lv_anim_t* /*a*/) {
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void AnimTo(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, lv_anim_completed_cb_t done) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, kAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

void CloseInternal() {
    if (!s_open) return;
    s_open = false;
    s_toc_open = false;
    if (s_toc) lv_obj_add_flag(s_toc, LV_OBJ_FLAG_HIDDEN);
    AnimTo(ScrimOpaExec, LV_OPA_50, LV_OPA_TRANSP, OnHideDone);
    AnimTo(TopYExec, 0, -kTopH, nullptr);
    AnimTo(SheetYExec, kPanel - kSheetH, kPanel, nullptr);
}

// ---- 目录抽屉（虚拟化列表）------------------------------------------------
void OnTocItemClick(lv_event_t* e) {
    lv_obj_t* it = lv_event_get_target_obj(e);
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(it)));
    if (idx < 0 || idx >= static_cast<int>(s_chapters.size())) return;
    if (s_cb.on_select_chapter) s_cb.on_select_chapter(idx);
    CloseInternal();
    if (s_cb.on_close) s_cb.on_close();
}

// 依当前滚动位置，把 kTocPool 个复用控件绑定到对应章节（O(池大小)，与章数无关）。
void RebindToc(bool force) {
    if (s_toc_content == nullptr) return;
    int n = static_cast<int>(s_chapters.size());
    int scroll_y = lv_obj_get_scroll_y(s_toc_list);
    if (scroll_y < 0) scroll_y = 0;
    int first = scroll_y / kTocItemH - 1;  // 上方多绑一行做缓冲
    if (first < 0) first = 0;
    if (!force && first == s_toc_last_first) return;  // 同一批可见项，跳过
    s_toc_last_first = first;

    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);
    for (int k = 0; k < kTocPool; k++) {
        int ci = first + k;
        lv_obj_t* it = s_toc_item[k];
        if (it == nullptr) continue;
        if (ci >= 0 && ci < n) {
            lv_obj_set_y(it, ci * kTocItemH);
            lv_obj_set_user_data(it, reinterpret_cast<void*>(static_cast<intptr_t>(ci)));
            lv_label_set_text(s_toc_item_lbl[k], s_chapters[ci].title);
            bool cur = (ci == s_toc_current);
            lv_obj_set_style_text_color(s_toc_item_lbl[k], cur ? Hex(th.accent) : Hex(th.text),
                                        LV_PART_MAIN);
            lv_obj_remove_flag(it, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(it, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OnTocScroll(lv_event_t* /*e*/) { RebindToc(false); }

void OpenToc() {
    if (s_toc == nullptr) return;
    s_toc_open = true;
    int n = static_cast<int>(s_chapters.size());
    lv_obj_set_height(s_toc_content, n > 0 ? n * kTocItemH : kTocItemH);
    lv_obj_remove_flag(s_toc, LV_OBJ_FLAG_HIDDEN);

    // 滚到当前章附近（居中）
    int list_h = ebook_ui::kPanelH - kTocHeaderH;
    int target = s_toc_current * kTocItemH - list_h / 2 + kTocItemH / 2;
    int maxs = n * kTocItemH - list_h;
    if (maxs < 0) maxs = 0;
    if (target < 0) target = 0;
    if (target > maxs) target = maxs;
    lv_obj_scroll_to_y(s_toc_list, target, LV_ANIM_OFF);
    s_toc_last_first = -1;
    RebindToc(true);

    // 抽屉从左滑入
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toc);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t val) { lv_obj_set_x(static_cast<lv_obj_t*>(v), val); });
    lv_anim_set_values(&a, -kTocW, 0);
    lv_anim_set_duration(&a, kAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// ---- 控件构建 --------------------------------------------------------------
// 统一设置行：整宽（撑满面板内容区）、固定高，标签左对齐、控件右对齐。
lv_obj_t* MakeRow(lv_obj_t* parent, const char* label) {
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, Hex(th.text), LV_PART_MAIN);
    return row;
}

// 分段选择器（2~4 段）。tag: 0=font 1=ls 2=margin（用于回调分发）。
void BuildSeg(lv_obj_t* row, Seg& seg, const char* const* labels, int count, int tag) {
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);
    if (count > kSegMax) count = kSegMax;
    seg.count = count;
    lv_obj_t* box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 300, 56);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(box, 8, LV_PART_MAIN);
    for (int i = 0; i < count; i++) {
        lv_obj_t* btn = lv_button_create(box);
        lv_obj_remove_style_all(btn);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 56);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, Hex(th.bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_PART_MAIN);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, Hex(th.text), LV_PART_MAIN);
        lv_obj_center(lbl);
        intptr_t packed = (static_cast<intptr_t>(tag) << 4) | i;
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                intptr_t p = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
                int t = static_cast<int>(p >> 4);
                int i = static_cast<int>(p & 0xF);
                if (t == 0 && s_cb.on_set_font) s_cb.on_set_font(i);
                else if (t == 1 && s_cb.on_set_line_space) s_cb.on_set_line_space(i);
                else if (t == 2 && s_cb.on_set_margin) s_cb.on_set_margin(i);
                Seg& sg = (t == 0) ? s_seg_font : (t == 1) ? s_seg_ls : s_seg_margin;
                PaintSeg(sg, i);
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(packed));
        seg.btn[i] = btn;
    }
}

// 字体循环选择：dir=+1/-1 在 [内置, 扫描到的字体...] 间循环。
void CycleFace(int dir) {
    int n = static_cast<int>(s_face_files.size());
    if (n <= 1) return;  // 只有"内置"，无可循环项
    s_face_sel = (s_face_sel + dir % n + n) % n;
    if (s_font_name_lbl) lv_label_set_text(s_font_name_lbl, s_face_names[s_face_sel].c_str());
    if (s_cb.on_set_font_face) s_cb.on_set_font_face(s_face_files[s_face_sel].c_str());
    UpdateFontSizeAvail();  // 切到/切出 FT 字体会改变"特大"可用性
}

// 扫描字体、重建循环列表，并把选中项对齐到当前 font_face。
void RebuildFaceList(const char* cur_face) {
    std::vector<ebook_font::FontFile> fonts;
    ebook_font::ScanFonts(fonts);
    s_face_files.clear();
    s_face_names.clear();
    s_face_files.push_back("");     // 0 = 内置
    s_face_names.push_back("内置");
    for (auto& f : fonts) {
        s_face_files.push_back(f.filename);
        s_face_names.push_back(f.display);
    }
    s_face_sel = 0;
    if (cur_face != nullptr && cur_face[0] != '\0') {
        for (size_t i = 1; i < s_face_files.size(); i++) {
            if (s_face_files[i] == cur_face) {
                s_face_sel = static_cast<int>(i);
                break;
            }
        }
    }
    if (s_font_name_lbl) lv_label_set_text(s_font_name_lbl, s_face_names[s_face_sel].c_str());
}

void OnSwatch(lv_event_t* e) {
    int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    ApplyMenuTheme(static_cast<uint8_t>(i));
    if (s_cb.on_set_theme) s_cb.on_set_theme(i);
}

void OnSliderReleased(lv_event_t* e) {
    lv_obj_t* sl = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(sl);
    if (s_pct_lbl) {
        char b[16];
        snprintf(b, sizeof(b), "%d%%", v);
        lv_label_set_text(s_pct_lbl, b);
    }
    if (s_cb.on_seek) s_cb.on_seek(v);
    CloseInternal();
    if (s_cb.on_close) s_cb.on_close();
}

}  // namespace

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb) {
    s_cb = cb;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);

    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_set_size(s_root, kPanel, ebook_ui::kPanelH);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(s_root, false);  // 菜单拥有自身手势，不触发右滑返回

    // 遮罩：点击关闭
    s_scrim = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_scrim);
    lv_obj_set_size(s_scrim, kPanel, ebook_ui::kPanelH);
    lv_obj_set_style_bg_color(s_scrim, Hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        s_scrim,
        [](lv_event_t*) {
            if (s_toc_open) {  // 先关抽屉
                s_toc_open = false;
                if (s_toc) lv_obj_add_flag(s_toc, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            CloseInternal();
            if (s_cb.on_close) s_cb.on_close();
        },
        LV_EVENT_CLICKED, nullptr);

    // 顶栏
    s_topbar = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_topbar);
    lv_obj_set_size(s_topbar, kPanel, kTopH);
    lv_obj_set_pos(s_topbar, 0, -kTopH);
    lv_obj_set_style_bg_color(s_topbar, Hex(th.card_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_topbar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_topbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(s_topbar);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(
        back, [](lv_event_t*) { if (s_cb.on_back_to_shelf) s_cb.on_back_to_shelf(); },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);

    // 目录入口：紧挨返回按钮右侧的「目录」按钮（点击打开章节抽屉）
    s_toc_entry_btn = lv_button_create(s_topbar);
    lv_obj_remove_style_all(s_toc_entry_btn);
    lv_obj_set_size(s_toc_entry_btn, 92, 52);
    lv_obj_align(s_toc_entry_btn, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_radius(s_toc_entry_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toc_entry_btn, Hex(th.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toc_entry_btn, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toc_entry_btn, Hex(th.accent), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(s_toc_entry_btn, LV_OPA_30, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(s_toc_entry_btn, [](lv_event_t*) { OpenToc(); }, LV_EVENT_CLICKED, nullptr);
    s_toc_entry_lbl = lv_label_create(s_toc_entry_btn);
    lv_label_set_text(s_toc_entry_lbl, "目录");
    lv_obj_set_style_text_font(s_toc_entry_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_toc_entry_lbl, Hex(th.text), LV_PART_MAIN);
    lv_obj_center(s_toc_entry_lbl);

    // 书名：左对齐排在目录按钮右侧，避免与两个左侧按钮重叠
    s_title = lv_label_create(s_topbar);
    lv_label_set_text(s_title, "");
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title, kPanel - 210);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 196, 0);

    // 底部设置面板（顶部圆角，高度按内容精算 kSheetH，flex 从顶排布避免溢出错位）
    s_sheet = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_sheet);
    lv_obj_set_size(s_sheet, kPanel, kSheetH);
    lv_obj_set_pos(s_sheet, 0, kPanel);
    lv_obj_set_style_bg_color(s_sheet, Hex(th.card_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sheet, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_sheet, kSheetPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_sheet, kSheetPadV, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_sheet, kRowGap, LV_PART_MAIN);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);

    // ① 进度行：上一章 / slider / 下一章（整宽）
    lv_obj_t* prow = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(prow);
    lv_obj_set_width(prow, lv_pct(100));
    lv_obj_set_height(prow, kRowH);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prow, 14, LV_PART_MAIN);
    lv_obj_remove_flag(prow, LV_OBJ_FLAG_SCROLLABLE);

    auto make_navbtn = [&](const char* txt, lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(prow);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 92, 48);
        lv_obj_set_style_radius(b, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, Hex(th.bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_PART_MAIN);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, Hex(th.text), LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        return b;
    };
    make_navbtn("上一章", [](lv_event_t*) {
        if (s_cb.on_prev_chapter) s_cb.on_prev_chapter();
        CloseInternal();
        if (s_cb.on_close) s_cb.on_close();
    });
    s_slider = lv_slider_create(prow);
    lv_obj_set_flex_grow(s_slider, 1);
    lv_obj_set_height(s_slider, 10);
    lv_slider_set_range(s_slider, 0, 100);
    lv_obj_add_event_cb(s_slider, OnSliderReleased, LV_EVENT_RELEASED, nullptr);
    make_navbtn("下一章", [](lv_event_t*) {
        if (s_cb.on_next_chapter) s_cb.on_next_chapter();
        CloseInternal();
        if (s_cb.on_close) s_cb.on_close();
    });

    // ② 进度百分比（整行居中）
    s_pct_lbl = lv_label_create(s_sheet);
    lv_obj_set_width(s_pct_lbl, lv_pct(100));
    lv_label_set_text(s_pct_lbl, "0%");
    lv_obj_set_style_text_align(s_pct_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_pct_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pct_lbl, Hex(th.dim), LV_PART_MAIN);

    // ③ 主题（整行：标签左 + 3 色板右）。目录入口已移到顶栏，此处不再有目录行。
    {
        lv_obj_t* trow = MakeRow(s_sheet, "主题");
        lv_obj_t* box = lv_obj_create(trow);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 188, kRowH);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(box, 14, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 3; i++) {
            lv_obj_t* sw = lv_button_create(box);
            lv_obj_remove_style_all(sw);
            lv_obj_set_size(sw, 48, 48);
            lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(sw, Hex(ebook_ui::ThemeAt(i).bg), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(sw, 2, LV_PART_MAIN);
            lv_obj_add_event_cb(sw, OnSwatch, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            s_swatch[i] = sw;
        }
    }

    // ④ 字体（用户 FreeType 字体：◀ 名称 ▶ 循环选择；无字体时仅"内置"）
    {
        lv_obj_t* frow = MakeRow(s_sheet, "字体");
        lv_obj_t* box = lv_obj_create(frow);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 300, kRowH);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(box, 8, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        auto make_arrow = [&](const char* txt, int dir) {
            lv_obj_t* b = lv_button_create(box);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, 52, 48);
            lv_obj_set_style_radius(b, 10, LV_PART_MAIN);
            lv_obj_set_style_bg_color(b, Hex(th.bg), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_PART_MAIN);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, Hex(th.text), LV_PART_MAIN);
            lv_obj_center(l);
            lv_obj_add_event_cb(b, [](lv_event_t* e) {
                CycleFace(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
            }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(dir)));
            return b;
        };
        make_arrow("<", -1);
        s_font_name_lbl = lv_label_create(box);
        lv_obj_set_flex_grow(s_font_name_lbl, 1);
        lv_label_set_text(s_font_name_lbl, "内置");
        lv_label_set_long_mode(s_font_name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_font_name_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_font_name_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_font_name_lbl, Hex(th.text), LV_PART_MAIN);
        make_arrow(">", +1);
    }

    // ⑤ 字号（4 段，"特大"仅 FT 可用）/ 行距 / 边距
    {
        const char* fs[4] = {"小", "中", "大", "特大"};
        const char* ls[3] = {"紧", "适中", "松"};
        const char* mg[3] = {"窄", "中", "宽"};
        BuildSeg(MakeRow(s_sheet, "字号"), s_seg_font, fs, 4, 0);
        BuildSeg(MakeRow(s_sheet, "行距"), s_seg_ls, ls, 3, 1);
        BuildSeg(MakeRow(s_sheet, "边距"), s_seg_margin, mg, 3, 2);
    }

    // 目录抽屉（隐藏，左侧）：滚动容器 + 虚拟画布 + 复用控件池
    s_toc = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_toc);
    lv_obj_set_size(s_toc, kTocW, ebook_ui::kPanelH);
    lv_obj_set_pos(s_toc, -kTocW, 0);
    lv_obj_set_style_bg_color(s_toc, Hex(th.card_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_toc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toc, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* toc_title = lv_label_create(s_toc);
    lv_label_set_text(toc_title, "目录");
    lv_obj_set_style_text_font(toc_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(toc_title, Hex(th.text), LV_PART_MAIN);
    lv_obj_align(toc_title, LV_ALIGN_TOP_LEFT, 20, 22);

    s_toc_list = lv_obj_create(s_toc);
    screen_strip_obj_chrome(s_toc_list);
    lv_obj_set_size(s_toc_list, kTocW, ebook_ui::kPanelH - kTocHeaderH);
    lv_obj_set_pos(s_toc_list, 0, kTocHeaderH);
    lv_obj_set_style_bg_opa(s_toc_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_toc_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_toc_list, LV_DIR_VER);
    lv_obj_add_event_cb(s_toc_list, OnTocScroll, LV_EVENT_SCROLL, nullptr);
    screen_swipe_back_ignore(s_toc_list, false);

    s_toc_content = lv_obj_create(s_toc_list);  // 高度在 OpenToc 按章数设置
    lv_obj_remove_style_all(s_toc_content);
    lv_obj_set_width(s_toc_content, lv_pct(100));
    lv_obj_set_height(s_toc_content, kTocItemH);
    lv_obj_remove_flag(s_toc_content, LV_OBJ_FLAG_SCROLLABLE);

    for (int k = 0; k < kTocPool; k++) {
        lv_obj_t* it = lv_button_create(s_toc_content);
        lv_obj_remove_style_all(it);
        lv_obj_set_size(it, kTocW - 24, kTocItemH);
        lv_obj_set_x(it, 12);
        lv_obj_set_style_bg_opa(it, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(it, Hex(th.accent), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_bg_opa(it, LV_OPA_30, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_radius(it, 8, LV_PART_MAIN);
        lv_obj_add_flag(it, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(it, OnTocItemClick, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(it);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kTocW - 56);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        s_toc_item[k] = it;
        s_toc_item_lbl[k] = lbl;
    }

    ApplyMenuTheme(s_theme_idx);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    return s_root;
}

void Show(const ReaderSettings& s, const char* book_name, int progress_pct) {
    if (s_root == nullptr) return;
    ApplyMenuTheme(s.theme_idx);
    RebuildFaceList(s.font_face);   // 每次呼出重扫字体，反映 Web 上传/删除
    UpdateFontSizeAvail();          // 据 FT 激活态启用/置灰"特大"
    PaintSeg(s_seg_font, s.font_idx);
    PaintSeg(s_seg_ls, s.line_space_idx);
    PaintSeg(s_seg_margin, s.margin_idx);
    if (s_title) lv_label_set_text(s_title, book_name ? book_name : "");
    if (s_slider) lv_slider_set_value(s_slider, progress_pct, LV_ANIM_OFF);
    if (s_pct_lbl) {
        char b[16];
        snprintf(b, sizeof(b), "%d%%", progress_pct);
        lv_label_set_text(s_pct_lbl, b);
    }
    s_open = true;
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    AnimTo(ScrimOpaExec, LV_OPA_TRANSP, LV_OPA_50, nullptr);
    AnimTo(TopYExec, -kTopH, 0, nullptr);
    AnimTo(SheetYExec, kPanel, kPanel - kSheetH, nullptr);
}

void Hide() {
    CloseInternal();
}

bool IsOpen() { return s_open; }

void SetChapters(const std::vector<ChapterEntry>& chapters, int current_idx) {
    s_chapters = chapters;
    s_toc_current = current_idx;
    s_toc_last_first = -1;
    if (s_toc_list) lv_obj_scroll_to_y(s_toc_list, 0, LV_ANIM_OFF);
}

void SetThemeIdx(uint8_t idx) { ApplyMenuTheme(idx); }

void Reset() {
    s_root = s_scrim = s_topbar = s_sheet = s_title = s_slider = s_pct_lbl = nullptr;
    s_toc_entry_btn = s_toc_entry_lbl = nullptr;
    s_toc = s_toc_list = s_toc_content = nullptr;
    s_font_name_lbl = nullptr;
    for (int k = 0; k < kTocPool; k++) {
        s_toc_item[k] = nullptr;
        s_toc_item_lbl[k] = nullptr;
    }
    s_seg_font = s_seg_ls = s_seg_margin = Seg{};
    s_swatch[0] = s_swatch[1] = s_swatch[2] = nullptr;
    s_chapters.clear();
    s_face_files.clear();
    s_face_names.clear();
    s_face_sel = 0;
    s_toc_last_first = -1;
    s_toc_open = false;
    s_open = false;
}

}  // namespace reader_menu_view
