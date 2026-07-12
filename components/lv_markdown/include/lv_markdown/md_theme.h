#pragma once

#include <cstdint>

#include "lvgl.h"

namespace lvmd {

// All fonts/colors/spacing the default block renderers use. Colors are plain
// 0xRRGGBB. MdThemeDefaultDark() fills every field with the dark palette but
// points all fonts at lv_font_default() -- the app must supply its own fonts
// (this library ships none).
struct MdTheme {
    // fonts
    const lv_font_t* body = nullptr;      // paragraphs / lists / quotes / headings
    const lv_font_t* heading = nullptr;   // headings (may equal body)
    const lv_font_t* mono = nullptr;      // fenced code (ASCII coverage is enough)
    const lv_font_t* mono_cjk = nullptr;  // whole-block fallback when code contains non-ASCII
    const lv_font_t* code_info_font = nullptr;  // fence info tag; nullptr -> use `mono`

    // colors
    uint32_t text = 0xEDE6D6;
    uint32_t bold = 0xFFD584;        // light amber -- plain white was too close to `text`
    uint32_t italic = 0xFFFFFF;      // no italic glyphs exist; emphasis-by-color
    uint32_t strike = 0x5F5849;      // no per-span strikethrough; deleted = faded
    uint32_t inline_code = 0xFFAE1F;
    uint32_t link = 0xFFAE1F;
    uint32_t heading_color[3] = {0xFFAE1F, 0xFFAE1F, 0x8A6420};  // H3+ clamp to [2]
    uint32_t task_done_text = 0x97907E;
    uint32_t table_header = 0x8A6420;  // header name labels in expanded table rows
    uint32_t quote_text = 0x97907E;
    uint32_t quote_bar = 0x8A6420;
    uint32_t rule = 0x2A251C;
    uint32_t marker = 0xFFAE1F;  // list bullets / ordinals, streaming cursor
    uint32_t code_text = 0xEDE6D6;
    uint32_t code_bg = 0x12100C;
    uint32_t code_border = 0x2A251C;
    uint32_t code_info = 0x5F5849;

    // layout
    int32_t body_line_space = 12;
    int32_t code_line_space = 6;
    int32_t gap_default = 16;             // margin above paragraph/code/quote blocks
    int32_t gap_heading_above[3] = {24, 18, 14};
    int32_t gap_list_item = 8;
    int32_t code_pad_h = 18;
    int32_t code_pad_v = 14;
    int32_t quote_pad_l = 20;
    int32_t quote_bar_w = 4;
    int32_t rule_gap = 18;
    int32_t cursor_w = 14;
    int32_t cursor_h = 32;
};

const MdTheme& MdThemeDefaultDark();

}  // namespace lvmd
