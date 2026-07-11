// bookshelf_view.cc — 书架视图（顶栏 + 3 列封面网格）。

#include "bookshelf_view.h"

#include "ebook_ui_theme.h"
#include "screen_util.h"

#include <cstdio>
#include <cstring>

namespace bookshelf_view {

namespace {

using ebook_ui::Hex;

constexpr int16_t kPad = 20;
constexpr int16_t kHeaderH = 96;
constexpr int16_t kCardW = 210;   // 与 ebook_worker::kCoverBoxW 一致
constexpr int16_t kCoverH = 280;  // 与 ebook_worker::kCoverBoxH 一致
constexpr int16_t kColGap = (720 - 2 * kPad - 3 * kCardW) / 2;  // 3 列均布

// 生成式封面底色盘（按书名 hash 取色，主题无关的柔和深色，白字可读）。
constexpr uint32_t kCoverPalette[] = {0x4A6FA5, 0x8C5E58, 0x5B7F6B, 0x7A6491, 0xA5824A, 0x50757D};
constexpr size_t kPaletteN = sizeof(kCoverPalette) / sizeof(kCoverPalette[0]);

Callbacks s_cb;
uint8_t s_theme_idx = 1;
lv_obj_t* s_root = nullptr;
lv_obj_t* s_list = nullptr;
lv_obj_t* s_empty_lbl = nullptr;
lv_obj_t* s_count_lbl = nullptr;
std::vector<lv_obj_t*> s_cover_boxes;  // 每张卡的封面容器（SetCover 换图用）

lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void OnCardClicked(lv_event_t* e) {
    size_t idx = reinterpret_cast<size_t>(lv_event_get_user_data(e));
    if (s_cb.on_open_book) s_cb.on_open_book(idx);
}

void ClearList() {
    if (s_list) lv_obj_clean(s_list);
    s_cover_boxes.clear();
}

// 取书名前 2 个码点（跳过 ASCII 空格），生成式封面的大字。
void TitleInitials(const std::string& name, char* out, size_t cap) {
    size_t w = 0;
    int taken = 0;
    for (size_t i = 0; i < name.size() && taken < 2;) {
        uint8_t c = static_cast<uint8_t>(name[i]);
        size_t adv = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (i + adv > name.size()) break;
        if (!(adv == 1 && name[i] == ' ')) {
            if (w + adv >= cap) break;
            std::memcpy(out + w, name.data() + i, adv);
            w += adv;
            taken++;
        }
        i += adv;
    }
    out[w] = '\0';
}

void BuildCard(const BookInfo& b, size_t idx) {
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);

    lv_obj_t* card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, OnCardClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(idx));

    // 封面容器：默认生成式封面，SetCover 后换真图
    lv_obj_t* cover = lv_obj_create(card);
    screen_strip_obj_chrome(cover);
    lv_obj_set_size(cover, kCardW, kCoverH);
    uint32_t base = kCoverPalette[b.hash % kPaletteN];
    lv_obj_set_style_bg_color(cover, Hex(base), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cover, 12, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cover, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cover, Hex(th.card_border), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    s_cover_boxes.push_back(cover);

    // 生成式封面：书名首 2 字大号居中偏上 + 底部格式徽标
    char initials[16];
    TitleInitials(b.name, initials, sizeof(initials));
    lv_obj_t* big = lv_label_create(cover);
    lv_label_set_text(big, initials);
    lv_obj_set_style_text_font(big, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(big, Hex(0xF5F2EA), LV_PART_MAIN);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t* tag = lv_label_create(cover);
    lv_label_set_text(tag, b.format == BookFormat::kEpub ? "EPUB" : "TXT");
    lv_obj_set_style_text_font(tag, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(tag, Hex(0xF5F2EA), LV_PART_MAIN);
    lv_obj_set_style_text_opa(tag, LV_OPA_60, LV_PART_MAIN);
    lv_obj_align(tag, LV_ALIGN_BOTTOM_MID, 0, -14);

    // 书名（一行，超长省略）
    lv_obj_t* name = lv_label_create(card);
    lv_label_set_text(name, b.name.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, kCardW);
    lv_obj_set_style_text_color(name, Hex(th.text), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    // 进度
    char meta[32];
    if (b.progress_pct > 0) {
        snprintf(meta, sizeof(meta), "已读 %d%%", b.progress_pct);
    } else if (b.progress_pct == 0) {
        snprintf(meta, sizeof(meta), "刚开始读");
    } else {
        snprintf(meta, sizeof(meta), "未读");
    }
    lv_obj_t* meta_lbl = lv_label_create(card);
    lv_label_set_text(meta_lbl, meta);
    lv_obj_set_style_text_color(meta_lbl, Hex(th.dim), LV_PART_MAIN);
    lv_obj_set_style_text_font(meta_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_remove_flag(meta_lbl, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb, uint8_t theme_idx) {
    s_cb = cb;
    s_theme_idx = theme_idx;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(theme_idx);

    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_set_size(s_root, ebook_ui::kPanelW, ebook_ui::kPanelH);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, Hex(th.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏：返回按钮 + 标题「书架」+ 书目计数
    lv_obj_t* back_btn = lv_button_create(s_root);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, Hex(th.card_border), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kPad - 4, 16);
    screen_swipe_back_ignore(back_btn, true);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t*) { if (s_cb.on_back) s_cb.on_back(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(s_root);
    lv_label_set_text(title, "书架");
    lv_obj_set_style_text_color(title, Hex(th.text), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // 顶栏右上「管理」按钮 → Web 后台
    lv_obj_t* manage_btn = lv_button_create(s_root);
    lv_obj_remove_style_all(manage_btn);
    lv_obj_set_size(manage_btn, 104, 56);
    lv_obj_align(manage_btn, LV_ALIGN_TOP_RIGHT, -kPad + 4, 20);
    lv_obj_set_style_radius(manage_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(manage_btn, Hex(th.card_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(manage_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(manage_btn, Hex(th.card_border), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(manage_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(manage_btn, Hex(th.card_border), LV_PART_MAIN);
    screen_swipe_back_ignore(manage_btn, true);
    lv_obj_add_event_cb(
        manage_btn, [](lv_event_t*) { if (s_cb.on_manage) s_cb.on_manage(); }, LV_EVENT_CLICKED,
        nullptr);
    lv_obj_t* manage_lbl = lv_label_create(manage_btn);
    lv_label_set_text(manage_lbl, "管理");
    lv_obj_set_style_text_font(manage_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(manage_lbl, Hex(th.text), LV_PART_MAIN);
    lv_obj_center(manage_lbl);

    s_count_lbl = lv_label_create(s_root);
    lv_label_set_text(s_count_lbl, "");
    lv_obj_set_style_text_color(s_count_lbl, Hex(th.dim), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_count_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_align(s_count_lbl, LV_ALIGN_TOP_MID, 0, 64);

    // 空态占位
    s_empty_lbl = lv_label_create(s_root);
    lv_label_set_text(s_empty_lbl, "");
    lv_obj_set_style_text_color(s_empty_lbl, Hex(th.dim), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_empty_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_empty_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    // 封面网格（3 列 flex wrap，垂直滚动）
    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, ebook_ui::kPanelW, ebook_ui::kPanelH - kHeaderH);
    lv_obj_set_pos(s_list, 0, kHeaderH);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_list, kPad, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_list, kColGap, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    // 列表内部滚动拥有垂直拖动语义，不参与右滑返回（水平方向仍可触发返回）。
    screen_swipe_back_ignore(s_list, false);

    return s_root;
}

void SetBooks(const std::vector<BookInfo>& books, bool sd_mounted) {
    ClearList();
    if (s_count_lbl) {
        if (sd_mounted) {
            char c[32];
            snprintf(c, sizeof(c), "共 %d 本", static_cast<int>(books.size()));
            lv_label_set_text(s_count_lbl, c);
        } else {
            lv_label_set_text(s_count_lbl, "");
        }
    }

    if (!sd_mounted) {
        lv_label_set_text(s_empty_lbl, "未检测到 SD 卡");
        lv_obj_remove_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (books.empty()) {
        lv_label_set_text(s_empty_lbl, "把 .txt / .epub 书籍放入\nSD 卡 /books 目录即可阅读");
        lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_remove_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    s_cover_boxes.reserve(books.size());
    for (size_t i = 0; i < books.size(); i++) BuildCard(books[i], i);
}

void SetCover(size_t idx, const lv_image_dsc_t* dsc) {
    if (idx >= s_cover_boxes.size() || s_cover_boxes[idx] == nullptr || dsc == nullptr) return;
    lv_obj_t* box = s_cover_boxes[idx];
    lv_obj_clean(box);  // 移除生成式封面元素
    lv_obj_t* img = lv_image_create(box);
    lv_image_set_src(img, dsc);
    lv_obj_center(img);  // 等比缩略 ≤ 盒尺寸，居中（余边露出底色）
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void Clear() {
    ClearList();
}

void Reset() {
    s_root = nullptr;
    s_list = nullptr;
    s_empty_lbl = nullptr;
    s_count_lbl = nullptr;
    s_cover_boxes.clear();
}

}  // namespace bookshelf_view
