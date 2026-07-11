// ebook_ui_theme.h
// 电子书 app 的配色主题（白 / 米黄 / 夜间）、字号档、行距/边距档，
// 以及从 ReaderSettings 展开为 PageMetrics 的辅助。UI 与 paginator 共用。

#ifndef EBOOK_UI_THEME_H
#define EBOOK_UI_THEME_H

#include "ebook_font.h"
#include "ebook_models.h"
#include "lvgl.h"

#include <cstdint>

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_40_4);  // 大号：普惠体常用字子集（40px，供阅读"大"档）

namespace ebook_ui {

// 面板尺寸与阅读区固定布局。
constexpr int16_t kPanelW = 720;
constexpr int16_t kPanelH = 720;
constexpr int16_t kHeaderH = 60;    // 页眉：章节名（居中）+ 进度%（右上角），即状态栏
constexpr int16_t kBottomPad = 16;  // 正文底部留白（进度已移到页眉，原页脚区腾给正文渲染）

// ---- 字号档（下标 = ReaderSettings.font_idx）----
// px 为 FreeType 渲染像素；builtin_font/builtin_lh 为内置点阵字体（缺 px 字库时的兜底）。
// 4 档 小/中/大/特大 = 20/30/40/50。内置点阵只到 40，故"特大"档内置回退到 40（仅 FT 字体
// 真正 50px；菜单在内置模式下会置灰"特大"）。
constexpr uint8_t kFontTierCount = 4;
struct FontTier {
    const lv_font_t* builtin_font;
    int16_t builtin_lh;
    int16_t px;
};
inline const FontTier& FontTierAt(uint8_t idx) {
    static const FontTier kTiers[kFontTierCount] = {
        {&font_puhui_20_4, 25, 20},
        {&font_puhui_30_4, 38, 30},
        {&font_puhui_40_4, 49, 40},
        {&font_puhui_40_4, 49, 50},  // 特大：内置回退 40，FT 用 50px
    };
    return kTiers[idx >= kFontTierCount ? kFontTierCount - 1 : idx];
}

// ---- 行距档（叠加在 line_height 上的额外像素）----
inline int16_t LineSpaceAt(uint8_t idx) {
    static const int16_t kTiers[3] = {4, 12, 20};
    return kTiers[idx > 2 ? 2 : idx];
}

// ---- 边距档（水平内边距，px）----
inline int16_t MarginAt(uint8_t idx) {
    static const int16_t kTiers[3] = {28, 44, 62};
    return kTiers[idx > 2 ? 2 : idx];
}

// ---- 三主题配色 ----
struct Theme {
    uint32_t bg;           // 阅读区背景
    uint32_t text;         // 正文
    uint32_t dim;          // 页眉/页脚弱化文字
    uint32_t card_bg;      // 书架卡片底
    uint32_t card_border;  // 书架卡片描边
    uint32_t accent;       // 进度条/选中态
};

inline const Theme& ThemeAt(uint8_t idx) {
    static const Theme kThemes[3] = {
        // 白
        {0xFFFFFF, 0x1A1A1A, 0x9AA0A6, 0xF4F5F7, 0xE3E5E8, 0x3D6FE0},
        // 米黄（护眼）
        {0xEFE3C8, 0x5B4B35, 0xA6926B, 0xE7D9B8, 0xD6C39A, 0xB07A3C},
        // 夜间
        {0x1A1A1A, 0xB2B2B2, 0x666666, 0x242424, 0x333333, 0x5A7FB0},
    };
    return kThemes[idx > 2 ? 2 : idx];
}

inline lv_color_t Hex(uint32_t c) { return lv_color_hex(c); }

// 由阅读设置展开出 paginator 需要的排版参数。
inline PageMetrics BuildMetrics(const ReaderSettings& s) {
    const FontTier& ft = FontTierAt(s.font_idx);
    const FontTier& hf = FontTierAt(static_cast<uint8_t>(s.font_idx + 1));  // 标题 = 正文 +1 档（封顶）
    int16_t margin_h = MarginAt(s.margin_idx);
    PageMetrics m;
    // FreeType 用户字体已激活 → 用 FT 字体，行高动态取；否则内置点阵档（现网行为不变）。
    if (ebook_font::Active()) {
        m.ft_font = true;
        m.font = ebook_font::BodyFont();
        m.heading_font = ebook_font::HeadingFont();
        m.line_height = static_cast<int16_t>(lv_font_get_line_height(m.font));
        m.heading_line_height = static_cast<int16_t>(lv_font_get_line_height(m.heading_font));
    } else {
        m.ft_font = false;
        m.font = ft.builtin_font;
        m.heading_font = hf.builtin_font;
        m.line_height = ft.builtin_lh;
        m.heading_line_height = hf.builtin_lh;
    }
    m.line_space = LineSpaceAt(s.line_space_idx);
    m.para_space = m.line_space;  // 段间距 = 一个行距档
    m.content_w = kPanelW - 2 * margin_h;
    m.content_h = kPanelH - kHeaderH - kBottomPad;
    // 段首缩进 = 1 个全角字宽（小屏 2 字偏宽，1 字够用）。字库里 U+3000（全角空格）是零宽空字形
    // （步进=0），不能用它测宽，改用最常用汉字"一"(U+4E00) 的步进作全角基准；兜底用 line_height。
    // paginator 与 reader 同取此值。
    uint16_t em = lv_font_get_glyph_width(m.font, 0x4E00, 0);
    if (em == 0) em = static_cast<uint16_t>(m.line_height);
    m.indent_w = static_cast<int16_t>(em);
    m.bg888 = ThemeAt(s.theme_idx).bg;
    return m;
}

}  // namespace ebook_ui

#endif  // EBOOK_UI_THEME_H
