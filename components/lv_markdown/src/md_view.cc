#include "lv_markdown/md_view.h"

#include <cstdio>

#include "lv_markdown/md_inline.h"

namespace lvmd {
namespace {

void StripChrome(lv_obj_t* o) {
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* MakeTextLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int32_t line_space) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(lbl, line_space, LV_PART_MAIN);
    return lbl;
}

// ---------------------------------------------------------------------------
// recolor encoding. LVGL 9 recolor markup is `#RRGGBB text#`; a literal '#'
// outside a span is `##`. Two engine quirks force the rules below (verified
// against lv_draw_label.c / lv_text.c in LVGL 9.3):
//  - inside a span a '#' terminates it and the next word is eaten as a color
//    parameter, so a styled span containing '#' must be split around it;
//  - `##` escapes are drawn correctly but the line-width measurement skips
//    the following word, so recolor is only enabled on labels that actually
//    carry styled spans (plain text never gets escaped/measured wrong).
// ---------------------------------------------------------------------------

void AppendEscaped(std::string* out, std::string_view text, bool escape_hash) {
    for (char c : text) {
        if (c == '#' && escape_hash) *out += '#';
        *out += c;
    }
}

void AppendColorSpan(std::string* out, uint32_t color, std::string_view text) {
    char cmd[10];
    std::snprintf(cmd, sizeof(cmd), "#%06X ", static_cast<unsigned>(color & 0xFFFFFF));
    bool open = false;
    for (char c : text) {
        if (c == '#') {
            if (open) {
                *out += '#';  // close the span before the literal '#'
                open = false;
            }
            *out += "##";
        } else {
            if (!open) {
                *out += cmd;
                open = true;
            }
            *out += c;
        }
    }
    if (open) *out += '#';
}

uint32_t SpanColor(uint8_t flags, const MdTheme& th) {
    if (flags & kSpanBold) return th.bold;
    if (flags & kSpanCode) return th.inline_code;
    if (flags & kSpanItalic) return th.italic;
    if (flags & kSpanStrike) return th.strike;
    return th.link;
}

// Inline-scan every line of a block and encode to recolor markup. Returns the
// display text; *used_markup tells the caller whether to enable recolor.
std::string EncodeInlineText(const std::string& text, const MdTheme& th, bool* used_markup) {
    // Fast path: no inline marker characters at all -> no spans, no escaping.
    // The typical CJK body tick re-renders a plain paragraph, so this skips
    // all span/vector construction on the hot path.
    if (text.find_first_of("`*~[\\") == std::string::npos) {
        *used_markup = false;
        return text;
    }
    std::vector<std::vector<Span>> lines;
    bool used = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        lines.push_back(ParseInline(std::string_view(text).substr(start, end - start)));
        for (const Span& s : lines.back()) {
            if (s.flags != 0) used = true;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    *used_markup = used;
    if (!used) return text;

    std::string out;
    out.reserve(text.size() + 32);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) out += '\n';
        for (const Span& s : lines[i]) {
            if (s.flags != 0) {
                AppendColorSpan(&out, SpanColor(s.flags, th), s.text);
            } else {
                AppendEscaped(&out, s.text, true);
            }
        }
    }
    return out;
}

// "Header value · Header value" markup for one expanded table row.
std::string EncodeTableRow(const Block& blk, const MdTheme& th) {
    auto split = [](const std::string& s) {
        std::vector<std::string_view> out;
        size_t start = 0;
        while (start <= s.size()) {
            size_t sep = s.find(kCellSep, start);
            size_t end = (sep == std::string::npos) ? s.size() : sep;
            out.push_back(std::string_view(s).substr(start, end - start));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        return out;
    };
    auto headers = split(blk.info);
    auto cells = split(blk.text);
    std::string out;
    for (size_t i = 0; i < cells.size(); i++) {
        bool has_header = i < headers.size() && !headers[i].empty();
        if (cells[i].empty() && !has_header) continue;
        if (!out.empty()) out += "  \xc2\xb7  ";
        if (has_header) {
            AppendColorSpan(&out, th.table_header, headers[i]);
            out += ' ';
        }
        for (const Span& s : ParseInline(cells[i])) {
            if (s.flags != 0) {
                AppendColorSpan(&out, SpanColor(s.flags, th), s.text);
            } else {
                AppendEscaped(&out, s.text, true);
            }
        }
    }
    return out;
}

bool HasNonAscii(const std::string& text) {
    for (char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// default block renderers
// ---------------------------------------------------------------------------

BlockWidget RenderParagraph(lv_obj_t* parent, const Block&, const MdTheme& th) {
    lv_obj_t* lbl = MakeTextLabel(parent, th.body, th.text, th.body_line_space);
    return BlockWidget{lbl, lbl};
}

int HeadingStyleIndex(int level) {  // H4-H6 clamp to the H3 tier
    if (level <= 1) return 0;
    return level == 2 ? 1 : 2;
}

BlockWidget RenderHeading(lv_obj_t* parent, const Block& blk, const MdTheme& th) {
    lv_obj_t* lbl =
        MakeTextLabel(parent, th.heading, th.heading_color[HeadingStyleIndex(blk.level)], th.body_line_space);
    return BlockWidget{lbl, lbl};
}

lv_obj_t* MakeListRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    StripChrome(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    return row;
}

lv_obj_t* MakeRowTextLabel(lv_obj_t* row, const MdTheme& th) {
    lv_obj_t* lbl = MakeTextLabel(row, th.body, th.text, th.body_line_space);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);  // flex grow decides the real width
    lv_obj_set_flex_grow(lbl, 1);
    return lbl;
}

BlockWidget RenderBullet(lv_obj_t* parent, const Block&, const MdTheme& th) {
    lv_obj_t* row = MakeListRow(parent);
    lv_obj_t* dot = lv_obj_create(row);
    StripChrome(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(th.marker), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    int32_t lh = lv_font_get_line_height(th.body);
    lv_obj_set_style_margin_top(dot, lh > 8 ? (lh - 8) / 2 : 0, LV_PART_MAIN);
    return BlockWidget{row, MakeRowTextLabel(row, th)};
}

BlockWidget RenderOrdered(lv_obj_t* parent, const Block& blk, const MdTheme& th) {
    lv_obj_t* row = MakeListRow(parent);
    lv_obj_t* num = lv_label_create(row);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.", blk.level);
    lv_label_set_text(num, buf);
    lv_obj_set_width(num, 44);
    lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(num, th.mono, LV_PART_MAIN);
    lv_obj_set_style_text_color(num, lv_color_hex(th.marker), LV_PART_MAIN);
    int32_t body_lh = lv_font_get_line_height(th.body);
    int32_t num_lh = lv_font_get_line_height(th.mono);
    lv_obj_set_style_margin_top(num, body_lh > num_lh ? (body_lh - num_lh) / 2 : 0, LV_PART_MAIN);
    return BlockWidget{row, MakeRowTextLabel(row, th)};
}

BlockWidget RenderTask(lv_obj_t* parent, const Block& blk, const MdTheme& th) {
    lv_obj_t* row = MakeListRow(parent);
    lv_obj_t* box = lv_obj_create(row);
    StripChrome(box);
    lv_obj_set_size(box, 20, 20);
    lv_obj_set_style_radius(box, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(th.marker), LV_PART_MAIN);
    if (blk.level != 0) {  // checked: filled box + faded text
        lv_obj_set_style_bg_color(box, lv_color_hex(th.marker), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    }
    int32_t lh = lv_font_get_line_height(th.body);
    lv_obj_set_style_margin_top(box, lh > 20 ? (lh - 20) / 2 : 0, LV_PART_MAIN);
    lv_obj_t* lbl = MakeRowTextLabel(row, th);
    if (blk.level != 0) lv_obj_set_style_text_color(lbl, lv_color_hex(th.task_done_text), LV_PART_MAIN);
    return BlockWidget{row, lbl};
}

BlockWidget RenderFence(lv_obj_t* parent, const Block& blk, const MdTheme& th) {
    lv_obj_t* box = lv_obj_create(parent);
    StripChrome(box);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(th.code_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(th.code_border), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(box, th.code_pad_h, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(box, th.code_pad_v, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, LV_PART_MAIN);
    if (!blk.info.empty()) {
        lv_obj_t* info = lv_label_create(box);
        lv_label_set_text(info, blk.info.c_str());
        lv_obj_set_style_text_font(info, th.code_info_font != nullptr ? th.code_info_font : th.mono, LV_PART_MAIN);
        lv_obj_set_style_text_color(info, lv_color_hex(th.code_info), LV_PART_MAIN);
    }
    // recolor stays off on this label: code is full of literal '#'
    lv_obj_t* lbl = MakeTextLabel(box, th.mono, th.code_text, th.code_line_space);
    return BlockWidget{box, lbl};
}

BlockWidget RenderQuote(lv_obj_t* parent, const Block&, const MdTheme& th) {
    lv_obj_t* box = lv_obj_create(parent);
    StripChrome(box);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(box, lv_color_hex(th.quote_bar), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, th.quote_bar_w, LV_PART_MAIN);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_left(box, th.quote_pad_l, LV_PART_MAIN);
    lv_obj_t* lbl = MakeTextLabel(box, th.body, th.quote_text, th.body_line_space);
    return BlockWidget{box, lbl};
}

// Tables render expanded -- one row per block, "Header value · Header value"
// behind a small square marker (a real grid doesn't fit 656px of CJK text).
BlockWidget RenderTableRow(lv_obj_t* parent, const Block&, const MdTheme& th) {
    lv_obj_t* row = MakeListRow(parent);
    lv_obj_t* sq = lv_obj_create(row);
    StripChrome(sq);
    lv_obj_set_size(sq, 8, 8);
    lv_obj_set_style_bg_color(sq, lv_color_hex(th.table_header), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, LV_PART_MAIN);
    int32_t lh = lv_font_get_line_height(th.body);
    lv_obj_set_style_margin_top(sq, lh > 8 ? (lh - 8) / 2 : 0, LV_PART_MAIN);
    return BlockWidget{row, MakeRowTextLabel(row, th)};
}

BlockWidget RenderRule(lv_obj_t* parent, const Block&, const MdTheme& th) {
    lv_obj_t* line = lv_obj_create(parent);
    StripChrome(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(th.rule), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    return BlockWidget{line, nullptr};
}

}  // namespace

// ---------------------------------------------------------------------------
// MdView
// ---------------------------------------------------------------------------

MdView::MdView(const MdTheme& theme, int32_t width) : theme_(theme), width_(width) {
    renderers_[static_cast<size_t>(BlockType::kParagraph)] = RenderParagraph;
    renderers_[static_cast<size_t>(BlockType::kHeading)] = RenderHeading;
    renderers_[static_cast<size_t>(BlockType::kBullet)] = RenderBullet;
    renderers_[static_cast<size_t>(BlockType::kOrdered)] = RenderOrdered;
    renderers_[static_cast<size_t>(BlockType::kTask)] = RenderTask;
    renderers_[static_cast<size_t>(BlockType::kFence)] = RenderFence;
    renderers_[static_cast<size_t>(BlockType::kQuote)] = RenderQuote;
    renderers_[static_cast<size_t>(BlockType::kRule)] = RenderRule;
    renderers_[static_cast<size_t>(BlockType::kTableRow)] = RenderTableRow;
}

MdView* MdView::Create(lv_obj_t* parent, const MdTheme& theme, int32_t width) {
    MdView* v = new MdView(theme, width);
    v->root_ = lv_obj_create(parent);
    StripChrome(v->root_);
    lv_obj_set_width(v->root_, width);
    lv_obj_set_height(v->root_, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->root_, 0, LV_PART_MAIN);  // per-block margin_top spaces the column
    lv_obj_add_event_cb(v->root_, OnRootDeleted, LV_EVENT_DELETE, v);

    lv_obj_t* cur = lv_obj_create(v->root_);
    StripChrome(cur);
    lv_obj_set_size(cur, theme.cursor_w, theme.cursor_h);
    lv_obj_set_style_bg_color(cur, lv_color_hex(theme.marker), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(cur, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(cur, LV_OBJ_FLAG_HIDDEN);
    v->cursor_ = cur;
    return v;
}

void MdView::OnRootDeleted(lv_event_t* e) {
    delete static_cast<MdView*>(lv_event_get_user_data(e));
}

void MdView::SetBlockRenderer(BlockType t, BlockRenderer r) {
    if (t < BlockType::kCount && r != nullptr) renderers_[static_cast<size_t>(t)] = r;
}

void MdView::Append(const char* delta) {
    if (finished_ || delta == nullptr || *delta == '\0') return;
    parser_.Feed(delta);
    for (Block& blk : parser_.TakeClosed()) FinalizeBlock(blk);
    RenderTail();
    if (cursor_ != nullptr && last_label_ != nullptr) lv_obj_remove_flag(cursor_, LV_OBJ_FLAG_HIDDEN);
}

void MdView::SyncCursor() {
    if (finished_) return;
    UpdateCursorPos();
}

void MdView::Finish() {
    if (finished_) return;
    for (Block& blk : parser_.Finish()) FinalizeBlock(blk);
    if (tail_.root != nullptr) {  // tentative-only tail that never produced a block
        lv_obj_delete(tail_.root);
        tail_ = BlockWidget{};
    }
    if (cursor_ != nullptr) {
        lv_obj_delete(cursor_);
        cursor_ = nullptr;
    }
    finished_ = true;
}

void MdView::Retheme(const MdTheme& theme) {
    theme_ = theme;
    for (int32_t i = static_cast<int32_t>(lv_obj_get_child_count(root_)) - 1; i >= 0; --i) {
        lv_obj_t* child = lv_obj_get_child(root_, i);
        if (child != cursor_) lv_obj_delete(child);
    }
    if (cursor_ != nullptr) lv_obj_set_style_bg_color(cursor_, lv_color_hex(theme_.marker), LV_PART_MAIN);
    tail_ = BlockWidget{};
    finalized_count_ = 0;  // margins recompute exactly as on first render
    last_finalized_label_ = nullptr;
    last_label_ = nullptr;
    for (const Block& blk : finalized_blocks_) {
        BlockWidget w = CreateBlockWidget(blk);
        ApplyBlockText(w, blk);
        if (w.label != nullptr) last_finalized_label_ = w.label;
        finalized_count_++;
    }
    last_label_ = last_finalized_label_;
    if (!finished_) RenderTail();  // rebuilds the open-tail widget under the new theme
}

void MdView::BlinkCursor() {
    if (cursor_ == nullptr || last_label_ == nullptr) return;
    if (lv_obj_has_flag(cursor_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(cursor_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(cursor_, LV_OBJ_FLAG_HIDDEN);
    }
}

// The first closed block of a drain always corresponds to the current tail
// widget (the open block it grew from); later ones are freshly created.
void MdView::FinalizeBlock(const Block& blk) {
    if (tail_.root != nullptr) {
        if (tail_type_ == blk.type) {
            ApplyBlockText(tail_, blk);
        } else {
            lv_obj_delete(tail_.root);
            tail_ = CreateBlockWidget(blk);
            ApplyBlockText(tail_, blk);
        }
        if (tail_.label != nullptr) last_finalized_label_ = tail_.label;
        tail_ = BlockWidget{};
    } else {
        BlockWidget w = CreateBlockWidget(blk);
        ApplyBlockText(w, blk);
        if (w.label != nullptr) last_finalized_label_ = w.label;
    }
    finalized_count_++;
    finalized_blocks_.push_back(blk);  // retained so Retheme can re-render
}

void MdView::RenderTail() {
    const Block* open = parser_.Open();
    if (open == nullptr) {
        if (tail_.root != nullptr) {
            lv_obj_delete(tail_.root);
            tail_ = BlockWidget{};
        }
        last_label_ = last_finalized_label_;
        return;
    }
    // level feeds the widget at creation time (heading color/margin, ordinal)
    bool rebuild = tail_.root == nullptr || tail_type_ != open->type;
    if (rebuild) {
        if (tail_.root != nullptr) lv_obj_delete(tail_.root);
        tail_ = CreateBlockWidget(*open);
        tail_type_ = open->type;
        tail_rev_ = parser_.OpenRevision() - 1;  // force the text apply below
    }
    if (tail_rev_ != parser_.OpenRevision()) {
        ApplyBlockText(tail_, *open);
        tail_rev_ = parser_.OpenRevision();
    }
    last_label_ = tail_.label != nullptr ? tail_.label : last_finalized_label_;
}

BlockWidget MdView::CreateBlockWidget(const Block& blk) {
    BlockWidget w = renderers_[static_cast<size_t>(blk.type)](root_, blk, theme_);
    int32_t margin = theme_.gap_default;
    switch (blk.type) {
        case BlockType::kHeading:
            margin = theme_.gap_heading_above[HeadingStyleIndex(blk.level)];
            break;
        case BlockType::kBullet:
        case BlockType::kOrdered:
        case BlockType::kTask:
        case BlockType::kTableRow:
            margin = theme_.gap_list_item;
            break;
        case BlockType::kRule:
            margin = theme_.rule_gap;
            break;
        default:
            break;
    }
    if (finalized_count_ == 0) margin = 0;
    lv_obj_set_style_margin_top(w.root, margin, LV_PART_MAIN);
    return w;
}

void MdView::ApplyBlockText(const BlockWidget& w, const Block& blk) {
    if (w.label == nullptr) return;
    if (blk.type == BlockType::kFence) {
        // blk.text is the fence's full accumulated body (not a delta), so this
        // is monotonic across a streaming tail: once any CJK byte appears the
        // check stays true on every later call without needing extra state.
        bool cjk = HasNonAscii(blk.text);
        lv_obj_set_style_text_font(w.label, cjk ? theme_.mono_cjk : theme_.mono, LV_PART_MAIN);
        lv_label_set_text(w.label, blk.text.c_str());
        return;
    }
    if (blk.type == BlockType::kTableRow) {  // headers are always colored -> markup always on
        std::string encoded = EncodeTableRow(blk, theme_);
        lv_label_set_recolor(w.label, true);
        lv_label_set_text(w.label, encoded.c_str());
        return;
    }
    bool used_markup = false;
    std::string encoded = EncodeInlineText(blk.text, theme_, &used_markup);
    lv_label_set_recolor(w.label, used_markup);
    lv_label_set_text(w.label, encoded.c_str());
}

// Assumes layout is already fresh (see SyncCursor) -- forcing a layout pass
// here would be the tick's second full-tree traversal.
void MdView::UpdateCursorPos() {
    if (cursor_ == nullptr) return;
    if (last_label_ == nullptr) {
        lv_obj_add_flag(cursor_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_point_t pos;
    lv_label_get_letter_pos(last_label_, LV_LABEL_POS_LAST, &pos);
    lv_area_t la, ra;
    lv_obj_get_coords(last_label_, &la);
    lv_obj_get_coords(root_, &ra);
    lv_obj_set_pos(cursor_, la.x1 - ra.x1 + pos.x + 2, la.y1 - ra.y1 + pos.y - 4);
}

}  // namespace lvmd
