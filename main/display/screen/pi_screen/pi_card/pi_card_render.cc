#include "pi_card_render.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "esp_log.h"

#include "pi_card_actions.h"
#include "pi_card_cmd.h"  // CommandRegistry（invoke 的 Lint 提示）
#include "pi_card_data.h"
#include "pi_card_icons.h"
#include "pi_card_solver.h"  // CARD V2：grid-only 布局求解器（docs/CARD_V2.md §2）
#include "pi_card_stock.h"
#include "pi_fonts.h"
#include "pi_theme.h"
#include "screen_util.h"

#define TAG "pi_card_render"

namespace pi_card {

using pi_theme::Tok;

namespace {

// ------------------------------- cJSON 取值 --------------------------------
const cJSON* GetItem(const cJSON* n, const char* k) { return cJSON_GetObjectItem(n, k); }
const char* GetStr(const cJSON* n, const char* k, const char* dflt = nullptr) {
    const cJSON* it = GetItem(n, k);
    return (it && cJSON_IsString(it)) ? it->valuestring : dflt;
}
int GetInt(const cJSON* n, const char* k, int dflt) {
    const cJSON* it = GetItem(n, k);
    return (it && cJSON_IsNumber(it)) ? it->valueint : dflt;
}
bool HasKey(const cJSON* n, const char* k) { return GetItem(n, k) != nullptr; }
bool GetBool(const cJSON* n, const char* k) {
    const cJSON* it = GetItem(n, k);
    return it && cJSON_IsBool(it) && cJSON_IsTrue(it);
}

// ------------------------------- 主题 / 字体 --------------------------------
// tone/role 语义色 → pi_theme 令牌（跟随双主题）。未知名回落 fallback。
bool ToneTok(const char* name, Tok& out) {
    if (!name) return false;
    struct M { const char* k; Tok v; };
    static const M kMap[] = {
        {"accent", Tok::Accent},   {"accent_dim", Tok::AccentDim}, {"ok", Tok::Ok},
        {"err", Tok::Err},         {"error", Tok::Err},            {"tx", Tok::Tx},
        {"text", Tok::Tx},         {"dim", Tok::Dim},              {"faint", Tok::Faint},
        {"card", Tok::Card},       {"card2", Tok::Card2},          {"line", Tok::Line},
        {"line2", Tok::Line2},     {"bg", Tok::Bg},
    };
    for (auto& m : kMap) {
        if (std::strcmp(m.k, name) == 0) {
            out = m.v;
            return true;
        }
    }
    return false;
}

// "#RRGGBB" → lv_color_t。
bool ParseHex(const char* s, lv_color_t& out) {
    if (!s || s[0] != '#' || std::strlen(s) < 7) return false;
    out = lv_color_hex(static_cast<uint32_t>(std::strtoul(s + 1, nullptr, 16)));
    return true;
}

// LLM 下发的 label fmt 会原样喂给 newlib vsnprintf（本固件 CONFIG_LV_USE_CLIB_SPRINTF=y），
// 且绑定 label 只带 1 个变参：数值(Int/Bool)路径传 int、字符串(String)路径传 char*。故合法
// fmt 必须：至多 1 个转换占位符、其类型与路径匹配（数值→d/i/u/o/x/X/c，字符串→s）、禁 %n
// （写内存原语）、禁 * 动态宽度（会多吃一个不存在的变参）。否则 "%s" 套在 int 上就是
// strlen((char*)value)，值为 0（如断网时 net.rssi）时读地址 0 崩溃——正是本次渲染崩溃的根因。
// %% 字面量不计数。校验器与渲染器共用此判定，保证「校验过 → 一定能安全格式化」。
bool FmtSafeForType(const char* fmt, HubType t, std::string& err) {
    if (!fmt) return true;  // 无 fmt：字符串直显、数值用默认 %d，均安全
    int conv = 0;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;  // 字面 %%
        // 跳过 flags / 宽度 / 精度 / 长度修饰，停在转换符
        while (*p && std::strchr("-+ #0123456789.lhLzjt", *p)) ++p;
        if (*p == '*') {
            err = "fmt 不支持 * 动态宽度";
            return false;
        }
        const char c = *p;
        if (c == '\0') {
            err = "fmt 转换占位符不完整";
            return false;
        }
        if (c == 'n') {
            err = "fmt 禁止使用 %n";
            return false;
        }
        if (++conv > 1) {
            err = "fmt 最多只能有一个占位符";
            return false;
        }
        if (t == HubType::String) {
            if (c != 's') {
                err = std::string("字符串绑定的 fmt 只能用 %s（收到 %") + c + "）";
                return false;
            }
        } else if (std::strchr("diouxXc", c) == nullptr) {  // Int / Bool
            err = std::string("数值绑定的 fmt 需用 %d/%x 等，不能用 %") + c + "（如 %s 会崩溃）";
            return false;
        }
    }
    return true;
}

// 中文正文用 puhui（按字号），数值/等宽用 pi_mono。
const lv_font_t* FontFor(int size, bool mono) {
    if (mono) return size >= 20 ? &font_pi_mono_20 : (size >= 17 ? &font_pi_mono_17 : &font_pi_mono_14);
    if (size >= 30) return &font_puhui_30_4;
    if (size >= 24) return &font_puhui_24_4;
    return &font_puhui_20_4;
}

bool IsMonoFont(const lv_font_t* f) {
    return f == &font_pi_mono_14 || f == &font_pi_mono_17 || f == &font_pi_mono_20;
}
bool HasCjk(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
        if (*p >= 0x80) return true;  // 任意非 ASCII 字节 → 含中文/多字节
    return false;
}
// text 里的下一个 UTF-8 码点：*p 指向当前起始字节，返回码点值并把 *p 推进到下一字符起点。
// 不做严格合法性校验——调用方的 text 来自已解析的 JSON 字符串，必为合法 UTF-8；畸形字节按
// 单字节步进兜底，只求不越界/不死循环，不追求处理非法输入的语义正确性。供 SafeFont 的
// puhui_24_4 逐码点覆盖检查用。
uint32_t NextUtf8Codepoint(const char*& p) {
    const unsigned char c0 = static_cast<unsigned char>(*p);
    if (c0 < 0x80) { ++p; return c0; }
    int extra;
    uint32_t cp;
    if ((c0 & 0xE0) == 0xC0) { extra = 1; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { extra = 2; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { extra = 3; cp = c0 & 0x07; }
    else { ++p; return c0; }  // 畸形起始字节：当单字节处理
    ++p;
    for (int i = 0; i < extra; ++i) {
        const unsigned char cc = static_cast<unsigned char>(*p);
        if ((cc & 0xC0) != 0x80) return cp;  // 畸形续字节：提前收尾，已读字节不回退
        cp = (cp << 6) | (cc & 0x3F);
        ++p;
    }
    return cp;
}
// ------------------------------ 共享几何样式 -------------------------------
// 卡片控件的「主题无关」几何属性（圆角/内距/描边宽/阴影）收敛成一组进程级
// 静态 lv_style_t，各控件用 lv_obj_add_style 复用，替代每次渲染逐属性 set_local。
// 颜色/背景全部继续走 pi_theme 令牌（local style），与这些几何属性集不相交——
// 主题切换（改令牌 style 色值 + report_style_change）不受影响，无级联冲突。
// 静态存储期永不销毁；EnsureCardStyles() 惰性初始化一次——RenderNode 和 RenderPreviewNode
// 各自在自己入口顶部独立调用一次（后者是流式预览的入口，二者互不可达，缺一个都会在从未
// 渲染过任何卡片的全新进程里对未初始化的静态 lv_style_t 调 lv_obj_add_style；真机
// LV_USE_ASSERT_STYLE 开着，会硬挂起——这不是假设，是真实踩过的坑）。二者都覆盖后，
// ApplyButtonStyle/ApplyDefaultStyle 等下游任意入口调用都安全。
//
// ⚠️ 边界：容器（column/row/list/spacer/divider/choice-box）都先过
// screen_strip_obj_chrome，后者用 **local** style 把 pad/margin/border_width/radius
// 置 0。LVGL 中 local style 优先级高于 added style，故对这些容器用 add_style 设
// pad/border/radius 会被 local 0 压掉、静默失效。所以本组只放：(a) 作用在未 strip 的
// button/slider（part MAIN/INDICATOR/KNOB）上的几何，(b) screen_strip 不碰的属性
// （bg_opa）。容器的 radius/pad/border 一律保留原地 local set（见 ApplyDefaultStyle
// 卡面、MakeChoice、ApplyFill）。
lv_style_t s_btn_base;     // 按钮基座：radius 12 + pad 20/15 + 无阴影
lv_style_t s_ghost_extra;  // ghost 变体附加：透明底 + 1px 描边
lv_style_t s_transp_bg;    // 透明底容器（column/row/list/spacer/plain 按钮）—— 仅 bg_opa
lv_style_t s_round_track;  // slider/bar 轨道 MAIN：圆头 + 水平 13 内缩
lv_style_t s_track_indic;  // slider/bar 填充 INDICATOR：圆头
lv_style_t s_knob;         // slider 把手 KNOB：圆头 + pad 10 + 微投影（黑为双主题不变量）
lv_style_t s_choice_seg;   // choice 分段按钮（lv_button，未 strip）：radius 10 + 竖内距 10 + 无阴影

void EnsureCardStyles() {
    static bool inited = false;
    if (inited) return;
    inited = true;

    lv_style_init(&s_btn_base);
    lv_style_set_radius(&s_btn_base, 12);
    lv_style_set_pad_hor(&s_btn_base, 20);
    lv_style_set_pad_ver(&s_btn_base, 15);
    lv_style_set_shadow_width(&s_btn_base, 0);

    lv_style_init(&s_ghost_extra);
    lv_style_set_bg_opa(&s_ghost_extra, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_ghost_extra, 1);

    lv_style_init(&s_transp_bg);
    lv_style_set_bg_opa(&s_transp_bg, LV_OPA_TRANSP);

    lv_style_init(&s_round_track);
    lv_style_set_radius(&s_round_track, LV_RADIUS_CIRCLE);
    // 把手超出轨道两端各约 13px；MAIN 水平 padding 把轨道向内缩，0%/100% 端点的把手
    // 停在滑块盒子内、不外伸吃穿 gap；bar 无把手但为跨行左右端对齐须一致。
    lv_style_set_pad_hor(&s_round_track, 13);

    lv_style_init(&s_track_indic);
    lv_style_set_radius(&s_track_indic, LV_RADIUS_CIRCLE);

    lv_style_init(&s_knob);
    lv_style_set_radius(&s_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s_knob, 10);  // ~26px 把手
    lv_style_set_shadow_width(&s_knob, 8);
    lv_style_set_shadow_color(&s_knob, lv_color_hex(0x000000));
    lv_style_set_shadow_opa(&s_knob, LV_OPA_30);

    lv_style_init(&s_choice_seg);
    lv_style_set_radius(&s_choice_seg, 10);
    lv_style_set_pad_ver(&s_choice_seg, 10);
    lv_style_set_shadow_width(&s_choice_seg, 0);
}

// 容器/控件底色：fill(令牌) > bg(hex)。无则不动（透明或默认样式）。
// radius 走 local set 而非共享 add_style：目标容器已过 screen_strip_obj_chrome（local
// radius=0），local 优先级高于 added style 会压掉共享样式的 radius。bg_opa 不受此限
// （screen_strip 不碰 bg_opa），但一并 local set 更直白。
void ApplyFill(lv_obj_t* obj, const cJSON* node) {
    Tok tok;
    lv_color_t c;
    if (ToneTok(GetStr(node, "fill"), tok)) {
        pi_theme::ApplyBg(obj, tok);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
    } else if (ParseHex(GetStr(node, "bg"), c)) {
        lv_obj_set_style_bg_color(obj, c, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
    }
}

// tone/color 覆盖默认字色，否则用给定默认令牌。
void SetTextTone(lv_obj_t* lbl, const cJSON* node, Tok dflt) {
    Tok tok;
    lv_color_t c;
    if (ToneTok(GetStr(node, "tone"), tok)) pi_theme::ApplyText(lbl, tok);
    else if (ParseHex(GetStr(node, "color"), c)) lv_obj_set_style_text_color(lbl, c, LV_PART_MAIN);
    else pi_theme::ApplyText(lbl, dflt);
}

// 排版角色：一个 role 名 = 字体 + 默认色 + 字距。给声明式卡片一套「设计好的」层级
// 词汇（eyebrow/section/title/label/value/caption），比手调 size/mono 更一致、更像
// 刻意设计的界面。pi 身份：mono 眉标/数值 + puhui 标题/正文，字距拉开的小 mono 眉
// 标是招牌。tone/color 仍可覆盖默认色。无 role 时回落 size/mono。
void ApplyLabelStyle(lv_obj_t* lbl, const cJSON* node) {
    const char* role = GetStr(node, "role");
    const lv_font_t* font = nullptr;
    Tok def_tone = Tok::Tx;
    int letter = 0;
    if (role) {
        if (!std::strcmp(role, "eyebrow")) { font = &font_pi_mono_14; def_tone = Tok::Faint; letter = 2; }
        else if (!std::strcmp(role, "kicker")) { font = &font_pi_mono_14; def_tone = Tok::Accent; letter = 2; }
        else if (!std::strcmp(role, "section")) { font = &font_pi_mono_14; def_tone = Tok::Dim; letter = 2; }
        else if (!std::strcmp(role, "title")) { font = &font_puhui_30_4; def_tone = Tok::Tx; }
        else if (!std::strcmp(role, "heading")) { font = &font_puhui_24_4; def_tone = Tok::Tx; }
        else if (!std::strcmp(role, "label")) { font = &font_puhui_20_4; def_tone = Tok::Dim; }
        else if (!std::strcmp(role, "value")) { font = &font_pi_mono_20; def_tone = Tok::Tx; }
        else if (!std::strcmp(role, "caption")) { font = &font_pi_mono_14; def_tone = Tok::Faint; }
    }
    if (!font) font = FontFor(GetInt(node, "size", 20), GetBool(node, "mono"));
    if (SafeFont(font, GetStr(node, "text"))) letter = 0;  // 回退 puhui 后不加字距
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    if (letter) lv_obj_set_style_text_letter_space(lbl, letter, LV_PART_MAIN);
    SetTextTone(lbl, node, def_tone);
}

// 按钮变体：primary(一处 sharp accent) / ghost(描边) / plain(纯文字) / 默认(中性填充)。
// 只有 primary 用琥珀——把强调色收敛到「一处」，其余按钮退成中性，主次层级立现。
// 返回按钮前景令牌（变体决定、node tone 可覆写）——按钮内嵌图标用它与文字同色。
Tok ApplyButtonStyle(lv_obj_t* btn, lv_obj_t* lbl, const cJSON* node) {
    const char* variant = GetStr(node, "variant", "default");
    lv_obj_add_style(btn, &s_btn_base, LV_PART_MAIN);
    const lv_style_selector_t pressed = LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    Tok text_tone = Tok::Tx;
    if (!std::strcmp(variant, "primary")) {
        pi_theme::ApplyBg(btn, Tok::Accent);
        pi_theme::ApplyBg(btn, Tok::AccentDim, pressed);
        text_tone = Tok::Bg;  // 深字压在琥珀上（深/浅主题都够对比）
    } else if (!std::strcmp(variant, "ghost")) {
        lv_obj_add_style(btn, &s_ghost_extra, LV_PART_MAIN);
        pi_theme::ApplyBorder(btn, Tok::Line);
        pi_theme::ApplyBg(btn, Tok::Card2, pressed);
    } else if (!std::strcmp(variant, "plain")) {
        lv_obj_add_style(btn, &s_transp_bg, LV_PART_MAIN);
        text_tone = Tok::Accent;
    } else {  // 中性填充
        pi_theme::ApplyBg(btn, Tok::Card2);
        pi_theme::ApplyBg(btn, Tok::Line, pressed);
    }
    const lv_font_t* font = FontFor(GetInt(node, "size", 20), GetBool(node, "mono"));
    SafeFont(font, GetStr(node, "text"));  // 中文按钮文字兜底 puhui，不出豆腐块
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    SetTextTone(lbl, node, text_tone);
    ToneTok(GetStr(node, "tone"), text_tone);  // 覆写后的最终前景令牌回给调用方
    return text_tone;
}

// ------------------------------ 默认精致样式 -------------------------------
// 呼应 pi_quick_panel / CreateToolCard：轨道 Card2、强调 Accent、卡面 Card。
void ApplyDefaultStyle(lv_obj_t* obj, const char* type, int depth) {
    if (std::strcmp(type, "slider") == 0) {
        lv_obj_set_height(obj, 6);
        pi_theme::ApplyBg(obj, Tok::Line);  // 底轨（整盒宽、可见）—— 把手边缘贴齐它的端点
        lv_obj_add_style(obj, &s_round_track, LV_PART_MAIN);
        pi_theme::ApplyBg(obj, Tok::Accent, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_track_indic, LV_PART_INDICATOR);
        // 把手用中性 Tx（而非又一块琥珀）+ 细描边 + 微投影：强调色只留给填充轨，
        // 把手更精致、层次更清。几何（圆头/pad/投影）走共享 s_knob。
        pi_theme::ApplyBg(obj, Tok::Tx, LV_PART_KNOB);
        lv_obj_add_style(obj, &s_knob, LV_PART_KNOB);
        lv_obj_set_ext_click_area(obj, 20);
    } else if (std::strcmp(type, "bar") == 0) {
        lv_obj_set_height(obj, 6);
        pi_theme::ApplyBg(obj, Tok::Line);  // 底轨可见，与 slider 一致
        lv_obj_add_style(obj, &s_round_track, LV_PART_MAIN);
        pi_theme::ApplyBg(obj, Tok::Accent, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_track_indic, LV_PART_INDICATOR);
    } else if (std::strcmp(type, "switch") == 0) {
        pi_theme::ApplyBg(obj, Tok::Card2);
        pi_theme::ApplyBg(obj, Tok::Accent,
                          LV_PART_INDICATOR | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    } else if (std::strcmp(type, "arc") == 0) {
        // 语义对齐 slider：底轨 Line、填充 Accent、把手中性 Tx——一枚圆形旋钮，用于单个
        // 突出显示的数值（音量/亮度这类）。
        lv_obj_set_size(obj, 132, 132);
        lv_arc_set_rotation(obj, 135);
        lv_arc_set_bg_angles(obj, 0, 270);
        lv_obj_set_style_arc_width(obj, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(obj, 8, LV_PART_INDICATOR);
        pi_theme::ApplyArc(obj, Tok::Line, LV_PART_MAIN);         // 底轨
        pi_theme::ApplyArc(obj, Tok::Accent, LV_PART_INDICATOR);  // 琥珀填充
        pi_theme::ApplyBg(obj, Tok::Tx, LV_PART_KNOB);            // 中性把手，强调色只留给填充弧
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_pad_all(obj, 6, LV_PART_KNOB);
    }
    // button 的样式（含变体）在其分支里由 ApplyButtonStyle 处理，这里不碰。
    // 顶层容器：卡片外观（柔圆角 + 慷慨留白 + 细边框），即便 LLM 只给光秃 column。
    if (depth == 0 && (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0 ||
                       std::strcmp(type, "grid") == 0)) {
        pi_theme::ApplyBg(obj, Tok::Card);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(obj, 18, LV_PART_MAIN);
        pi_theme::ApplyBorder(obj, Tok::Line);
        lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(obj, 24, LV_PART_MAIN);
    }
}

// --------------------------- 硬件回写（用户交互）---------------------------
// LVGL 里程序化改 subject 不触发 VALUE_CHANGED，只有用户拖动/点按才触发；据此把
// 回写挂在控件事件上即天然无回环。slider 拖动 150ms 节流 + 松手终值；switch 直写。
struct HwWriteback {
    std::string path;
    uint32_t last_ms = 0;
};
void HwChangedCb(lv_event_t* e) {
    auto* wb = static_cast<HwWriteback*>(lv_event_get_user_data(e));
    lv_obj_t* w = lv_event_get_target_obj(e);
    if (lv_obj_check_type(w, &lv_slider_class) || lv_obj_check_type(w, &lv_arc_class)) {
        if (!lv_obj_has_state(w, LV_STATE_PRESSED)) return;  // 防外部程序化更新误触
        uint32_t now = lv_tick_get();
        if (now - wb->last_ms < 150) return;
        wb->last_ms = now;
    }
    DataHub::Instance().Write(wb->path, WidgetValue(w));
}
void HwReleasedCb(lv_event_t* e) {
    auto* wb = static_cast<HwWriteback*>(lv_event_get_user_data(e));
    lv_obj_t* w = lv_event_get_target_obj(e);
    DataHub::Instance().Write(wb->path, WidgetValue(w));
}
void HwFreeCb(lv_event_t* e) { delete static_cast<HwWriteback*>(lv_event_get_user_data(e)); }

void AttachWriteback(lv_obj_t* obj, const char* type, const std::string& path) {
    auto* wb = new HwWriteback{path, 0};
    lv_obj_add_event_cb(obj, HwChangedCb, LV_EVENT_VALUE_CHANGED, wb);
    if (std::strcmp(type, "slider") == 0 || std::strcmp(type, "arc") == 0) {
        lv_obj_add_event_cb(obj, HwReleasedCb, LV_EVENT_RELEASED, wb);  // 终值兜底
    }
    lv_obj_add_event_cb(obj, HwFreeCb, LV_EVENT_DELETE, wb);
}

// bar 无内建 bind，用 observer 手动同步。
void BarObserverCb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* bar = lv_observer_get_target_obj(observer);
    if (bar) lv_bar_set_value(bar, lv_subject_get_int(subject), LV_ANIM_ON);
}

// ---- 数值 label 插值动画（非 String 类型 bind 路径专用，见下方 ApplyBind）----
// 原生 lv_label_bind_text 收到 notify 就直接 lv_label_set_text_fmt，是一次性跳变；这里改手动
// observer + 250ms 插值，让数字"滚"过去而不是硬切。anim 的 var 直接是 label 对象本身（不是
// ctx）——LVGL9 删对象时自动清掉挂在它上面的动画（lv_obj_destructor 里的
// lv_anim_delete(obj, NULL)，已读 managed_components/lvgl__lvgl/src/core/lv_obj.c:525 核实），
// 不需要再挂一个 LV_EVENT_DELETE 回调去手动打断在途动画。ctx（fmt+当前显示值，来自
// UiCard::num_anims，地址稳定不因扩容搬迁）借 lv_obj_set_user_data(label, ctx) 挂在 label
// 上，供 exec_cb 反查。
void NumScrollExecCb(void* var, int32_t v) {
    lv_obj_t* label = static_cast<lv_obj_t*>(var);
    auto* ctx = static_cast<LabelNumAnim*>(lv_obj_get_user_data(label));
    if (!ctx) return;
    ctx->shown = v;
    // fmt 的 "%d" 要配 int 实参——int32_t 在本工具链是 long，直传给 -Wformat 报类型不符。
    lv_label_set_text_fmt(label, ctx->fmt ? ctx->fmt : "%d", static_cast<int>(v));
}

void NumScrollObserverCb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* label = lv_observer_get_target_obj(observer);
    auto* ctx = static_cast<LabelNumAnim*>(lv_observer_get_user_data(observer));
    if (!label || !ctx) return;
    int32_t target = lv_subject_get_int(subject);
    if (target == ctx->shown) return;  // 值没变（含注册时的首次立即回调），不必起一轮动画
    // 息屏，或差值 <=1（典型 Bool 0/1、以及大多数整数路径的最小步进）：插值动画在这种小
    // 差值上观感是"卡在 0 一下再跳"而非真正的"滚动"，不如直接跳变干脆（verify-m1 发现的
    // 问题）。250ms 插值只留给差值 >=2 时真正能看出"滚"的场景。
    if (IsScreenOff() || std::abs(target - ctx->shown) <= 1) {
        ctx->shown = target;
        lv_label_set_text_fmt(label, ctx->fmt ? ctx->fmt : "%d", static_cast<int>(target));
        return;
    }
    lv_anim_delete(label, NumScrollExecCb);  // 打断上一轮还没播完的插值，避免两个 anim 抢同一个 var
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, NumScrollExecCb);
    lv_anim_set_values(&a, ctx->shown, target);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// ---- 卡片入场动效 exec_cb（公开入口 PlayCardEntrance 在本文件后段，见 pi_card_render.h）----
void EntranceOpaExecCb(void* var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), LV_PART_MAIN);
}
void EntranceYExecCb(void* var, int32_t v) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(var), v, LV_PART_MAIN);
}

// ------------------------------ choice 复合控件 -----------------------------
// 一行分段选择器：flex-row 容器 + N 个内部按钮。容器打 LV_OBJ_FLAG_USER_1 标记 +
// lv_obj_set_user_data 挂堆上的 ChoiceCtx（当前选中下标/段数/按钮句柄），DELETE 时释放。
// 内部按钮不经 RenderNode——choice 整体只计 1 个节点，不占 64 上限。ChoiceValue/
// ChoiceSetValue（pi_card_render.h 公开）定义在文件末尾（pi_card 命名空间作用域，
// 供 pi_card_actions.cc 跨 TU 调用），这里的辅助函数直接引用其声明。
struct ChoiceCtx {
    int value = 0;
    int count = 0;
    std::vector<lv_obj_t*> btns;
};

void ChoiceRestyle(lv_obj_t* container) {
    auto* ctx = static_cast<ChoiceCtx*>(lv_obj_get_user_data(container));
    if (!ctx) return;
    for (int i = 0; i < ctx->count; i++) {
        lv_obj_t* btn = ctx->btns[i];
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        if (i == ctx->value) {
            pi_theme::ApplyBg(btn, Tok::Accent);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            if (lbl) pi_theme::ApplyText(lbl, Tok::Bg);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            if (lbl) pi_theme::ApplyText(lbl, Tok::Dim);
        }
    }
}

void ChoiceSegClickedCb(lv_event_t* e) {
    lv_obj_t* seg = lv_event_get_target_obj(e);
    auto* container = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    auto* ctx = static_cast<ChoiceCtx*>(lv_obj_get_user_data(container));
    if (!ctx) return;
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->btns[i] == seg) {
            ctx->value = i;
            break;
        }
    }
    ChoiceRestyle(container);
    lv_obj_send_event(container, LV_EVENT_VALUE_CHANGED, nullptr);  // 触发容器上的 on_change + 回写
}

void ChoiceFreeCb(lv_event_t* e) { delete static_cast<ChoiceCtx*>(lv_event_get_user_data(e)); }

// subject（外部程序化改值）→ choice 容器同步。
void ChoiceObserverCb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* container = lv_observer_get_target_obj(observer);
    if (container) ChoiceSetValue(container, lv_subject_get_int(subject));
}

lv_obj_t* MakeChoice(lv_obj_t* parent, const cJSON* node) {
    lv_obj_t* box = lv_obj_create(parent);
    screen_strip_obj_chrome(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    pi_theme::ApplyBg(box, Tok::Card2);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    // radius/pad 走 local set，不能进共享 add_style：box 过了 screen_strip_obj_chrome
    // （local radius/pad=0），而 LVGL local style 优先级高于 added style，会把共享样式的
    // 几何压掉。见 EnsureCardStyles 注释。
    lv_obj_set_style_radius(box, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, 4, LV_PART_MAIN);
    lv_obj_set_height(box, LV_SIZE_CONTENT);

    const cJSON* opts = GetItem(node, "options");
    const int n = (opts && cJSON_IsArray(opts)) ? cJSON_GetArraySize(opts) : 0;
    auto* ctx = new ChoiceCtx();
    ctx->count = n;
    ctx->btns.reserve(n);
    const cJSON* opt = nullptr;
    cJSON_ArrayForEach(opt, opts) {
        lv_obj_t* seg = lv_button_create(box);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_add_style(seg, &s_choice_seg, LV_PART_MAIN);
        lv_obj_t* lbl = lv_label_create(seg);
        const char* text = cJSON_IsString(opt) ? opt->valuestring : "";
        const lv_font_t* font = &font_puhui_20_4;
        SafeFont(font, text);
        lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(seg, ChoiceSegClickedCb, LV_EVENT_CLICKED, box);
        ctx->btns.push_back(seg);
    }
    int value = GetInt(node, "value", 0);
    if (value < 0) value = 0;
    if (n > 0 && value >= n) value = n - 1;
    ctx->value = n > 0 ? value : 0;
    lv_obj_add_flag(box, LV_OBJ_FLAG_USER_1);
    lv_obj_set_user_data(box, ctx);
    lv_obj_add_event_cb(box, ChoiceFreeCb, LV_EVENT_DELETE, ctx);
    ChoiceRestyle(box);
    return box;
}

// 把 bind 路径接到控件（显示同步；可写路径再挂硬件回写）。
void ApplyBind(lv_obj_t* obj, const char* type, const char* path, const cJSON* node, UiCard* card) {
    DataHub& hub = DataHub::Instance();
    lv_subject_t* subj = hub.Acquire(path);
    if (!subj) return;
    card->hub_paths.push_back(path);
    bool writable = hub.Writable(path);

    // 绑定到有量程的数据源时，用 DataHub 量程强制收口控件区间——盖过 JSON 里的
    // min/max，让「亮度滑条拖不到 5% 以下」「音量拖不出 0–100」不依赖 LLM 自觉。
    int lo = 0, hi = 0;
    const bool has_range = hub.RangeOf(path, lo, hi);

    if (std::strcmp(type, "label") == 0) {
        HubType t = HubType::Int;
        hub.TypeOf(path, t);
        // String 绑定的值可能含中文（"1.57万亿" / SSID / "超限"），而 SafeFont 只检查过静态
        // text——mono 字体无 CJK 会渲成豆腐块。绑定期文本未知，按"可能含中文"保守换字体。
        if (t == HubType::String) {
            const lv_font_t* f = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
            if (SafeFont(f, "值")) lv_obj_set_style_text_font(obj, f, LV_PART_MAIN);
        }
        const char* fmt = GetStr(node, "fmt", t == HubType::String ? nullptr : "%d");
        // 兜底网：畸形 fmt 本应被 Validate 挡在工具期，但校验器/渲染器万一不同构时，这里
        // 回落安全默认，绝不让 %s 套 int 之类崩到 newlib vsnprintf 里（见 FmtSafeForType）。
        std::string ferr;
        if (fmt && !FmtSafeForType(fmt, t, ferr)) {
            ESP_LOGW(TAG, "unsafe label fmt '%s' dropped: %s", fmt, ferr.c_str());
            fmt = (t == HubType::String) ? nullptr : "%d";
        }
        // lv_label_bind_text 存的是 fmt 指针（不拷贝），而 fmt 指向即将被 host 的
        // cJSON_Delete 释放的节点树。intern 进 card 的字符串池（地址稳定、随卡片存活）
        // 再绑定，否则后续每次刷新都在读已释放内存 → label 格式化成乱码。
        if (fmt) fmt = card->str_pool.emplace_back(fmt).c_str();
        if (t == HubType::String) {
            // 字符串没有"数值插值"的意义，维持原生 bind：一次性跳变直显新文本。
            lv_label_bind_text(obj, subj, fmt);
        } else {
            // 数值路径（Int/Bool）：手动 observer + 250ms 插值滚动，见上方 NumScrollObserverCb。
            LabelNumAnim& ctx = card->num_anims.emplace_back();
            ctx.fmt = fmt;
            ctx.shown = lv_subject_get_int(subj);
            lv_label_set_text_fmt(obj, fmt ? fmt : "%d", static_cast<int>(ctx.shown));  // 种子初值，不过渡
            lv_obj_set_user_data(obj, &ctx);  // exec_cb 用（NumScrollExecCb 反查 fmt/shown）
            lv_subject_add_observer_obj(subj, NumScrollObserverCb, obj, &ctx);
        }
    } else if (std::strcmp(type, "slider") == 0) {
        if (has_range) lv_slider_set_range(obj, lo, hi);
        lv_slider_bind_value(obj, subj);
        if (writable) AttachWriteback(obj, type, path);
    } else if (std::strcmp(type, "bar") == 0) {
        if (has_range) lv_bar_set_range(obj, lo, hi);
        lv_subject_add_observer_obj(subj, BarObserverCb, obj, nullptr);
        lv_bar_set_value(obj, lv_subject_get_int(subj), LV_ANIM_OFF);
    } else if (std::strcmp(type, "switch") == 0) {
        lv_obj_bind_checked(obj, subj);
        if (writable) AttachWriteback(obj, type, path);
    } else if (std::strcmp(type, "arc") == 0) {
        if (has_range) lv_arc_set_range(obj, lo, hi);
        lv_arc_bind_value(obj, subj);
        if (writable) AttachWriteback(obj, type, path);
    } else if (std::strcmp(type, "choice") == 0) {
        // choice 无内建 bind：种子当前值 + observer 手动同步（subject → choice），
        // 可写时挂硬件回写（choice 走 VALUE_CHANGED，无 PRESSED 态，HwChangedCb 的类型
        // 守卫只判 slider/arc，choice 自然放行即时写）。
        ChoiceSetValue(obj, lv_subject_get_int(subj));
        lv_subject_add_observer_obj(subj, ChoiceObserverCb, obj, nullptr);
        if (writable) AttachWriteback(obj, type, path);
    }
}

// ------------------------------ 通用属性 -----------------------------------
// in_list_row：list 行模板重渲实例——N 行共用同一个模板 id，注册进 card->nodes 会互相
// 覆盖且行删除后指针悬垂，故行内子树一律不进注册表（toggle/show/hide/patch 校验期已拒）。
void ApplyCommonProps(lv_obj_t* obj, const char* type, const cJSON* node, UiCard* card,
                      bool in_list_row) {
    if (const char* id = GetStr(node, "id"); id && !in_list_row) card->nodes[id] = obj;
    if (HasKey(node, "pad")) lv_obj_set_style_pad_all(obj, GetInt(node, "pad", 0), LV_PART_MAIN);
    ApplyFill(obj, node);
    if (GetBool(node, "hidden")) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    // 卡内交互控件不再需要豁免屏级手势：edge-nav 只认屏幕边缘起手的横滑。
}

}  // namespace

// 稳定性护栏：mono 字体只有 ASCII，一旦文本含中文会渲成豆腐块。此时无论角色如何都
// 回退到 puhui，保证「怎么拼都读得出字」。返回是否发生了回退（供去掉字距）。跨 TU
// 导出（见 pi_card_render.h）：list 的 empty 兜底文案在 pi_card_host.cc 也要走它。
bool SafeFont(const lv_font_t*& font, const char* text) {
    if (IsMonoFont(font) && HasCjk(text)) {
        font = &font_puhui_20_4;
        return true;
    }
    // font_puhui_24_4 是「UI chrome 静态文案子集」（pi_fonts.h 头注：静态文案子集，缺字重跑
    // gen_puhui_24.py），不是完整常用字集——role:heading 用它时，动态/未预料到的中文常缺字
    // 渲成豆腐块 □（design_guide 实测：晴/暴/雨在 heading 缺字，在 title/label 正常）。逐码点
    // 用 lv_font_get_glyph_dsc 查真实字形覆盖——与渲染器实际取字形同一 API（含 fallback 链），
    // 结果与实际渲染是否豆腐块完全一致；有一个缺字就整串回退到 puhui_30_4（下一档完整字集，
    // 24→30 略大但不豆腐，唯一代价）。ASCII/已覆盖文本原样留在 24px，不受影响。
    if (font == &font_puhui_24_4 && text && *text) {
        const char* p = text;
        while (*p) {
            const uint32_t cp = NextUtf8Codepoint(p);
            lv_font_glyph_dsc_t dsc;
            if (!lv_font_get_glyph_dsc(&font_puhui_24_4, &dsc, cp, 0)) {
                font = &font_puhui_30_4;
                return true;
            }
        }
    }
    return false;
}

namespace {
constexpr int32_t kEntranceSlideY = 12;
constexpr uint32_t kEntranceDurationMs = 220;
constexpr uint32_t kAdoptedFadeDurationMs = 120;
}  // namespace

void PlayCardEntrance(lv_obj_t* tree, bool adopted) {
    if (!tree || IsScreenOff()) return;  // 息屏时不起动画：黑屏没人看得见，白白占一轮 anim tick

    lv_obj_set_style_opa(tree, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tree);
    lv_anim_set_exec_cb(&a, EntranceOpaExecCb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, adopted ? kAdoptedFadeDurationMs : kEntranceDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    if (!adopted) {
        lv_obj_set_style_translate_y(tree, kEntranceSlideY, LV_PART_MAIN);
        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, tree);
        lv_anim_set_exec_cb(&ay, EntranceYExecCb);
        lv_anim_set_values(&ay, kEntranceSlideY, 0);
        lv_anim_set_duration(&ay, kEntranceDurationMs);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
        lv_anim_start(&ay);
    }
}

// ---------------------------------------------------------------------------
// data 值格式化 / list 行模板替换 —— 声明在 pi_card_render.h，供本文件的 list/data-label
// 渲染分支与 pi_card_host.cc 的 RefreshDataConsumers（ui_update 的 data 变更定向刷新）
// 共用同一套替换语义，不在两个 TU 里各写一份、日后漂移。

// number→%g（去尾 0/小数点，整数打印成整数）；string→原样；null/其他→空串。
std::string Stringify(const cJSON* v) {
    if (!v) return "";
    if (cJSON_IsString(v)) return v->valuestring ? v->valuestring : "";
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == static_cast<double>(v->valueint)) return std::to_string(v->valueint);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", d);
        return buf;
    }
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? "1" : "0";
    return "";
}

// {value}/{v} → Stringify(v)。
std::string SubstDataValue(const std::string& tpl, const cJSON* v) {
    std::string rep = Stringify(v);
    std::string out;
    for (size_t i = 0; i < tpl.size();) {
        if (tpl.compare(i, 7, "{value}") == 0) {
            out += rep;
            i += 7;
        } else if (tpl.compare(i, 3, "{v}") == 0) {
            out += rep;
            i += 3;
        } else {
            out += tpl[i++];
        }
    }
    return out;
}

// list 的 eff_max：max 声明优先，否则=数组实际长度（>0）或缺省 8；夹在 [1,20]（硬顶）。
int EffMax(const cJSON* node, int arr_len) {
    int m = HasKey(node, "max") ? GetInt(node, "max", 0) : (arr_len > 0 ? arr_len : 8);
    if (m < 1) m = 1;
    if (m > 20) m = 20;
    return m;
}

// list 行模板递归替换：字符串值里的 {i}(0基)/{n}(1基)/{item.KEY} 替换成本行记录对应内容；
// 缺失字段替空串；只替字符串**值**，绝不碰 key（否则模板结构被破坏）。
void SubstRecord(cJSON* node, const cJSON* rec, int i) {
    if (!node) return;
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        cJSON* child = node->child;
        while (child) {
            if (cJSON_IsString(child)) {
                const std::string tpl = child->valuestring ? child->valuestring : "";
                std::string out;
                for (size_t p = 0; p < tpl.size();) {
                    if (tpl.compare(p, 3, "{i}") == 0) {
                        out += std::to_string(i);
                        p += 3;
                    } else if (tpl.compare(p, 3, "{n}") == 0) {
                        out += std::to_string(i + 1);
                        p += 3;
                    } else if (tpl.compare(p, 6, "{item.") == 0) {
                        size_t end = tpl.find('}', p);
                        if (end != std::string::npos) {
                            std::string key = tpl.substr(p + 6, end - (p + 6));
                            const cJSON* fv = rec ? GetItem(rec, key.c_str()) : nullptr;
                            out += Stringify(fv);
                            p = end + 1;
                        } else {
                            out += tpl[p++];
                        }
                    } else {
                        out += tpl[p++];
                    }
                }
                cJSON_SetValuestring(child, out.c_str());
            } else if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
                SubstRecord(child, rec, i);
            }
            child = child->next;
        }
    }
}

// ---------------------------------------------------------------------------
// v2 哑翻译器（docs/CARD_V2.md §8 步骤3）：RenderNode 不再自己做任何几何推断——布局决策
// 全部交给 pi_card::solver::Solve（纯函数、零 LVGL），这里只把它吐出的像素几何（x/w/align/
// truncate）翻译成 lv_obj_set_pos/set_size + 12 种叶子 builder（builder 本身照抄 v1，未改）。
// 旧的 column/row/spacer/list/旧grid 分派 + FLOW_*/ApplySizing 几何推断已随改造4（流式预览
// v2）一并清空：RowChildrenAllGrowable/IsRowGrowConditional/FlexJustifyOf/FlexCrossOf/
// GridCellOnAutoTrack/GridHasFrCols 连同它们唯一的调用方（v1 树形 Lint()）已在步骤5一并删除
// （Lint() 已重写为 v2 grid 数组遍历，见文件末尾）。
// ApplySizing/ApplyColCrossLabelSizing/IsContainerType/GridDsc/GridDscFreeCb/FLOW_* 枚举/
// kRowLabelMaxWidthPx 已随 RenderPreviewNode/SyncPreviewNode/PreviewSyncContainer 一起删除
// （它们的唯一调用方就是这套 v1 生长边机器）。
namespace {

// grid 内容宽（不含卡片自身 pad）：chat/overlay 走 solver 设计常量；standby/pin 没有固定常量，
// 用宿主父容器实测宽扣个近似 pad 兜底（§2.1：视口宽由调用方按 display 传入）。
int ViewportWidthFor(UiCard* card, lv_obj_t* parent) {
    if (card->display == Display::Chat) return solver::kCardWChat;
    if (card->display == Display::Overlay) return solver::kCardWOverlay;
    int w = lv_obj_get_width(parent);
    return w > 200 ? w - 48 : solver::kCardWOverlay;
}

// role code → 实际渲染字体（镜像 ApplyLabelStyle 的字号阶梯），R3：过 SafeFont 回退后再测量，
// 不对着不存在字形的字体假测宽度。sim 直链真 LVGL（managed_components/lvgl__lvgl），真机同一份
// LVGL，故一份实现两边都对。
const lv_font_t* SolverFontFor(int role, bool mono) {
    switch (role) {
        case solver::kRoleEyebrow:
        case solver::kRoleKicker:
        case solver::kRoleSection:
        case solver::kRoleCaption:
            return &font_pi_mono_14;
        case solver::kRoleTitle:
            return &font_puhui_30_4;
        case solver::kRoleHeading:
            return &font_puhui_24_4;
        case solver::kRoleLabel:
            return &font_puhui_20_4;
        case solver::kRoleValue:
            // 必须跟 ApplyLabelStyle 严格一致：role:"value" 恒渲染 font_pi_mono_20，不看
            // JSON 的 mono 属性（那是 ApplyLabelStyle 自己的既定设计，非本函数决定）。曾经这里
            // 用 `mono?mono_20:puhui_20_4` 三元——当一个 role:"value" 的 cell 没显式声明
            // mono:true 且契约判定不是"数值"（比如 25_grid_tall.json 的 "v01".."v22"：无
            // mono、无 bind、TextLooksNumeric 判否）时，量出来的是 puhui_20_4 的窄宽度，但
            // 实际渲染永远是更宽的 mono_20——量出来的轨道装不下真实渲染的字形，断词多行。
            (void)mono;
            return &font_pi_mono_20;
        default:
            return mono ? &font_pi_mono_20 : &font_puhui_20_4;
    }
}

int SolverMeasureCb(const char* utf8, int role, bool mono, void* /*ctx*/) {
    if (!utf8 || !*utf8) return 0;
    const lv_font_t* font = SolverFontFor(role, mono);
    SafeFont(font, utf8);  // 拿到真正会被渲染出来的字体再量
    lv_point_t sz{0, 0};
    lv_text_get_size(&sz, utf8, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// bind_rows 展开（渲染器侧，复用 SubstRecord 语义；镜像 solver.cc 内部 BuildRows 的
// 计数/empty 行为，保证 ci = r*100+c 的 r 与本数组下标一一对应，不产生逻辑漂移——见任务
// 说明「渲染器自己需要维护 cell → 实际叶子 JSON 的映射」）。返回值 caller 用 cJSON_Delete 释放。
cJSON* BuildBindRowsForRender(const cJSON* grid, const cJSON* data) {
    cJSON* out = cJSON_CreateArray();
    const char* key = GetStr(grid, "bind_rows");
    const cJSON* item = GetItem(grid, "item");
    const cJSON* arr = (data && key) ? GetItem(data, key) : nullptr;
    int max = HasKey(grid, "max") ? GetInt(grid, "max", 20) : 20;
    if (max < 1) max = 1;
    if (max > 20) max = 20;
    int count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (count > max) count = max;
    if (count == 0) {
        const char* empty = GetStr(grid, "empty");
        cJSON* row = cJSON_CreateArray();
        cJSON* lbl = cJSON_CreateObject();
        cJSON_AddStringToObject(lbl, "type", "label");
        cJSON_AddStringToObject(lbl, "text", empty ? empty : "");
        cJSON_AddItemToArray(row, lbl);
        cJSON_AddItemToArray(out, row);
        return out;
    }
    for (int i = 0; i < count; ++i) {
        const cJSON* el = cJSON_GetArrayItem(arr, i);
        cJSON* row = cJSON_CreateArray();
        if (cJSON_IsArray(item)) {
            const cJSON* leaf = nullptr;
            cJSON_ArrayForEach(leaf, item) {
                cJSON* clone = cJSON_Duplicate(leaf, 1);
                SubstRecord(clone, el, i);
                cJSON_AddItemToArray(row, clone);
            }
        } else if (item) {
            cJSON* clone = cJSON_Duplicate(item, 1);
            SubstRecord(clone, el, i);
            cJSON_AddItemToArray(row, clone);
        }
        cJSON_AddItemToArray(out, row);
    }
    return out;
}

// 12 种叶子 builder：逐类型建 lv_obj，不做任何位置/宽度决策（那是调用方按 solver layout 做的
// 事）。函数体逐字照抄 v1 RenderNode 对应分支，只是去掉了不会再作为叶子出现的容器类型。
lv_obj_t* BuildLeafWidget(lv_obj_t* parent, const cJSON* node, std::string& err) {
    const char* type = GetStr(node, "type");
    lv_obj_t* obj = nullptr;
    if (std::strcmp(type, "label") == 0) {
        obj = lv_label_create(parent);
        lv_label_set_text(obj, GetStr(node, "text", ""));
        ApplyLabelStyle(obj, node);
    } else if (std::strcmp(type, "icon") == 0) {
        Tok tok = Tok::Dim;
        ToneTok(GetStr(node, "tone", "dim"), tok);
        obj = MakeIcon(parent, GetStr(node, "icon", GetStr(node, "name", "dot")),
                       GetInt(node, "size", 22), tok);
    } else if (std::strcmp(type, "button") == 0) {
        obj = lv_button_create(parent);
        lv_obj_t* lbl = lv_label_create(obj);
        const char* text = GetStr(node, "text", "");
        lv_label_set_text(lbl, text);
        Tok fg = ApplyButtonStyle(obj, lbl, node);
        if (const char* icon_name = GetStr(node, "icon")) {
            lv_obj_t* ic = MakeIcon(obj, icon_name, GetInt(node, "size", 20), fg);
            lv_obj_move_to_index(ic, 0);
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(obj, 8, LV_PART_MAIN);
            if (text[0] == '\0') lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_center(lbl);
        }
    } else if (std::strcmp(type, "slider") == 0) {
        obj = lv_slider_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;
        lv_slider_set_range(obj, mn, mx);
        if (HasKey(node, "value")) lv_slider_set_value(obj, GetInt(node, "value", 0), LV_ANIM_OFF);
    } else if (std::strcmp(type, "arc") == 0) {
        obj = lv_arc_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;
        lv_arc_set_range(obj, mn, mx);
        if (HasKey(node, "value")) lv_arc_set_value(obj, GetInt(node, "value", 0));
    } else if (std::strcmp(type, "bar") == 0) {
        obj = lv_bar_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;
        lv_bar_set_range(obj, mn, mx);
        if (HasKey(node, "value")) lv_bar_set_value(obj, GetInt(node, "value", 0), LV_ANIM_OFF);
    } else if (std::strcmp(type, "switch") == 0) {
        obj = lv_switch_create(parent);
        if (GetBool(node, "checked")) lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else if (std::strcmp(type, "qrcode") == 0) {
        obj = lv_qrcode_create(parent);
        int sz = GetInt(node, "size", 160);
        if (sz < 96) sz = 96;
        else if (sz > 320) sz = 320;
        lv_qrcode_set_size(obj, sz);
        lv_qrcode_set_dark_color(obj, pi_theme::PaletteOf(true).tx);
        lv_qrcode_set_light_color(obj, pi_theme::PaletteOf(true).bg);
        const char* txt = GetStr(node, "text", "");
        lv_qrcode_update(obj, txt, static_cast<uint32_t>(std::strlen(txt)));
    } else if (std::strcmp(type, "choice") == 0) {
        obj = MakeChoice(parent, node);
    } else if (std::strcmp(type, "chart") == 0) {
        obj = lv_chart_create(parent);
        lv_chart_set_type(obj, LV_CHART_TYPE_LINE);
        lv_chart_set_update_mode(obj, LV_CHART_UPDATE_MODE_SHIFT);
        int points = GetInt(node, "points", 60);
        points = std::min(120, std::max(8, points));
        lv_chart_set_point_count(obj, static_cast<uint16_t>(points));
        lv_chart_set_div_line_count(obj, 3, 0);
        pi_theme::ApplyBg(obj, Tok::Card2);
        lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
        lv_obj_set_style_line_color(obj, pi_theme::Color(Tok::Line2), LV_PART_MAIN);
        lv_obj_set_height(obj, 120);  // §3 决策C：chart 高度固定档 120px（h 属性已删）
        const char* path = GetStr(node, "bind_history");
        if (path) {
            auto& hub = DataHub::Instance();
            std::vector<int> seed;
            hub.HistorySnapshot(path, seed);
            int lo = 0, hi = 100;
            if (!hub.RangeOf(path, lo, hi)) {
                if (!seed.empty()) {
                    lo = *std::min_element(seed.begin(), seed.end());
                    hi = *std::max_element(seed.begin(), seed.end());
                    if (lo == hi) { lo -= 1; hi += 1; }
                }
            }
            lv_chart_set_range(obj, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
            lv_chart_series_t* ser = lv_chart_add_series(obj, pi_theme::Color(Tok::Accent),
                                                         LV_CHART_AXIS_PRIMARY_Y);
            for (int v : seed) lv_chart_set_next_value(obj, ser, v);
            int handle = hub.AddHistorySink(path, [obj, ser](int v) {
                lv_chart_set_next_value(obj, ser, v);
                lv_chart_refresh(obj);
            });
            auto* handle_box = new int(handle);
            lv_obj_add_event_cb(
                obj,
                [](lv_event_t* e) {
                    auto* h = static_cast<int*>(lv_event_get_user_data(e));
                    DataHub::Instance().RemoveHistorySink(*h);
                    delete h;
                },
                LV_EVENT_DELETE, handle_box);
        }
    } else if (std::strcmp(type, "stock_chart") == 0) {
        obj = pi_card_stock::Create(parent, node);
        if (obj == nullptr) {
            err = "stock_chart alloc failed";
            return nullptr;
        }
    } else if (std::strcmp(type, "divider") == 0) {
        obj = lv_obj_create(parent);
        screen_strip_obj_chrome(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(obj, 1);
        pi_theme::ApplyBg(obj, Tok::Line);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        err = std::string("unknown type: ") + (type ? type : "(null)");
        return nullptr;
    }
    if (!obj) {
        err = std::string("widget create failed: ") + type;
        return nullptr;
    }
    return obj;
}

// 落位 + 通用属性 + bind + 事件 + 死控件兜底：一个真实叶子建好之后的收尾，逐字照抄 v1
// RenderNode 尾段（去掉 parent_flow 相关的 ApplySizing——尺寸现在由 solver 的 x/w 决定）。
// wrap_mode 是 solver 输出的三态字段（"wrap"|"ellipsis"|"nowrap"，见 pi_card_solver.h 头注）：
// 只有 ELLIPSIS 需要渲染器钳单行高——WRAP（独占整行的正文，允许自然长高折行）和 NOWRAP
// （数值列/非文本控件，solver 已保证轨道宽≥真实测量宽，天然单行）都不钳，交给 LVGL 按内容自适应。
void FinishLeafWidget(lv_obj_t* obj, const cJSON* node, UiCard* card, int x, int w,
                     const char* align, const char* wrap_mode) {
    const char* type = GetStr(node, "type");
    lv_obj_set_pos(obj, x, 0);
    if (!std::strcmp(type, "arc") || !std::strcmp(type, "qrcode")) {
        if (w > 0) lv_obj_set_size(obj, w, w);
    } else if (!std::strcmp(type, "divider")) {
        if (w > 0) lv_obj_set_width(obj, w);
    } else if (std::strcmp(type, "icon") != 0 && w > 0) {
        lv_obj_set_width(obj, w);
    }
    if (!std::strcmp(type, "label")) {
        const bool right = align && !std::strcmp(align, "end");
        if (right) lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        const bool dynamic_text = GetStr(node, "bind") || GetStr(node, "bind_data");
        const bool ellipsis = wrap_mode && !std::strcmp(wrap_mode, "ellipsis");
        if (ellipsis) {
            // 动态 label 不能用 DOT：set_text_fmt 不像 set_text 那样先 revert dot 状态，陈旧 dot
            // 备份会写回新文本缓冲（见 v1 坑F/坑2 头注，同一套地雷在 v2 依旧成立）。
            lv_label_set_long_mode(obj, dynamic_text ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_DOT);
            // F4 配套修复：CLIP/DOT 都是靠"对象高度 < 换行后的自然高度"才会真正生效
            // （lv_label.c 的 get_label_flags 不区分 long_mode，只要宽度是定值就总按多行量算
            // 自然高度；DOT 的截断判定 size.y>obj 内容区高度 在 LV_SIZE_CONTENT 高度下永远为
            // false）——不显式钳一行高，宽度即使超出也只会自动长高换行，看起来跟 WRAP 一样，
            // ELLIPSIS 形同虚设。这里把高度钉死成当前字体一行高，才能让二者真正单行截断。
            // 钳高对 WRAP/NOWRAP 是误伤：WRAP 独占整行本意就是允许多行撑高，NOWRAP 数值列轨道
            // 宽已够、天然一行——钳了反而在极端场景下裁掉本该完整显示的内容（本轮修复的两个
            // 回归正是"单行钳制对所有 truncate=true 场景一视同仁"误伤了 WRAP 场景）。
            const lv_font_t* tf = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
            if (tf) lv_obj_set_height(obj, lv_font_get_line_height(tf));
        } else {
            lv_label_set_long_mode(obj, dynamic_text ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_WRAP);
        }
    }

    ApplyDefaultStyle(obj, type, /*depth=*/1);
    ApplyCommonProps(obj, type, node, card, /*in_list_row=*/false);

    if (const char* path = GetStr(node, "bind")) ApplyBind(obj, type, path, node, card);
    if (!std::strcmp(type, "label")) {
        if (const char* dk = GetStr(node, "bind_data"); dk && !GetStr(node, "bind")) {
            const cJSON* v = card->data ? GetItem(card->data, dk) : nullptr;
            std::string tpl = GetStr(node, "text") ? GetStr(node, "text") : "";
            std::string txt = tpl.empty() ? Stringify(v) : SubstDataValue(tpl, v);
            lv_label_set_text(obj, txt.c_str());
            UiCard::DataConsumer dc;
            dc.obj = obj;
            dc.kind = UiCard::DataConsumer::Label;
            dc.key = dk;
            dc.text_tpl = tpl;
            card->consumers.push_back(std::move(dc));
        }
    }

    AttachEvent(obj, LV_EVENT_CLICKED, card, GetItem(node, "on_click"));
    AttachEvent(obj, LV_EVENT_VALUE_CHANGED, card, GetItem(node, "on_change"));
    AttachEvent(obj, LV_EVENT_RELEASED, card, GetItem(node, "on_release"));

    if (std::strcmp(type, "switch") == 0 || std::strcmp(type, "slider") == 0 ||
        std::strcmp(type, "arc") == 0) {
        const char* bind = GetStr(node, "bind");
        const bool live = (bind && DataHub::Instance().Writable(bind)) ||
                          GetItem(node, "on_change") != nullptr ||
                          GetItem(node, "on_release") != nullptr ||
                          (!bind && GetStr(node, "id") != nullptr);
        if (!live) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(obj, LV_OPA_60, LV_PART_MAIN);
        }
    }
}

// 行内交叉轴（竖直）居中：solver 只给 x/w（零 LVGL 依赖，§11 硬约束），不知道叶子控件的真实
// 渲染高度（slider/switch/arc 走 LVGL 主题默认高度，label 走字体行高，都不是 solver 能猜的
// 像素值）——所以居中只能在渲染器这一层、拿控件建完之后的真实几何来做（同 pi_card_host.cc
// ReflowOverlay 的 lv_obj_update_layout 手法："强制布局，读到的高度才是准的"）。单叶子的行
// （独占整行 SPAN_ALL/SQUARE，或折行后就它自己一个）y 恒为 0 == 天然居中，不需要这个函数。
void CenterRowCross(lv_obj_t* rowobj, const std::vector<lv_obj_t*>& leaf_objs) {
    if (leaf_objs.size() < 2) return;
    lv_obj_update_layout(rowobj);
    int row_h = 0;
    for (lv_obj_t* o : leaf_objs) {
        int h = lv_obj_get_height(o);
        if (h > row_h) row_h = h;
    }
    for (lv_obj_t* o : leaf_objs) {
        int h = lv_obj_get_height(o);
        int y = (row_h - h) / 2;
        if (y < 0) y = 0;
        lv_obj_set_y(o, y);
    }
}

// solver 内建的合成表头行（cols[].title 触发）：与真实渲染/预览渲染共用，避免两处各写一份
// 产生行为漂移（正式渲染见 RenderGridBlock，预览见 RenderGridBlockPreview）。
void RenderColsHeaderRow(lv_obj_t* rowobj, const std::vector<const cJSON*>& row_cells,
                         const cJSON* cols_meta) {
    for (const cJSON* cellj : row_cells) {
        int col = GetInt(cellj, "col", 0);
        int x = GetInt(cellj, "x", 0);
        int w = GetInt(cellj, "w", 0);
        const char* align_s = GetStr(cellj, "align", "start");
        const cJSON* cd = (cols_meta && col < cJSON_GetArraySize(cols_meta))
                              ? cJSON_GetArrayItem(cols_meta, col)
                              : nullptr;
        const char* title = cd ? GetStr(cd, "title") : nullptr;
        lv_obj_t* lbl = lv_label_create(rowobj);
        lv_label_set_text(lbl, title ? title : "");
        const lv_font_t* f = &font_pi_mono_14;
        SafeFont(f, title);
        lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(lbl, 2, LV_PART_MAIN);
        pi_theme::ApplyText(lbl, Tok::Dim);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        if (align_s && !std::strcmp(align_s, "end")) {
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        }
        lv_obj_set_pos(lbl, x, 0);
        if (w > 0) lv_obj_set_width(lbl, w);
    }
}

// solver 内建的合成 divider 行（紧随表头行）：同上，正式/预览共用。
void RenderColsDividerRow(lv_obj_t* rowobj, const std::vector<const cJSON*>& row_cells,
                          int viewport_w) {
    const cJSON* cellj = row_cells.empty() ? nullptr : row_cells.front();
    int w = cellj ? GetInt(cellj, "w", viewport_w) : viewport_w;
    lv_obj_t* d = lv_obj_create(rowobj);
    screen_strip_obj_chrome(d);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(d, 0, 0);
    lv_obj_set_size(d, w, 1);
    pi_theme::ApplyBg(d, Tok::Line);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
}

// 一个 grid 块（cells/rows/bind_rows 三形态之一）：拿 solver 已经算好的 layout_grid，按行
// 竖排（flex COLUMN，行高交给 LVGL 用实际内容自适应）、行内按 x 绝对落位（数量已知的定像素
// 几何，不需要 LVGL 参与横向决策）。ci>=0 回指真实叶子 JSON；ci==-1 是 solver 内建的合成
// 表头/divider（cols[].title 触发），按位置（表头恒是这个 grid 输出里的第 1 行、divider 恒
// 紧随其后，见 solver.cc SolveRows 的 out_row 分配顺序）识别，不靠猜 span。
lv_obj_t* RenderGridBlock(lv_obj_t* card_root, const cJSON* grid_json, const cJSON* layout_grid,
                          UiCard* card, int viewport_w, int gap, int& node_count,
                          const RenderLimits& limits, std::string& err) {
    lv_obj_t* gobj = lv_obj_create(card_root);
    screen_strip_obj_chrome(gobj);
    lv_obj_remove_flag(gobj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(gobj, &s_transp_bg, LV_PART_MAIN);
    lv_obj_set_width(gobj, viewport_w);
    lv_obj_set_height(gobj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gobj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(gobj, gap, LV_PART_MAIN);
    ApplyFill(gobj, grid_json);
    // fill/bg 底色 grid：solver 已按 viewport_w-2*inset 求解，这里补同宽 pad，行容器同步收窄。
    const int inset = GetInt(layout_grid, "inset", 0);
    const int inner_w = viewport_w - 2 * inset;
    if (inset > 0) lv_obj_set_style_pad_all(gobj, inset, LV_PART_MAIN);

    const cJSON* cells_json = GetItem(grid_json, "cells");
    const cJSON* rows_json = GetItem(grid_json, "rows");
    cJSON* owned_rows = nullptr;
    const cJSON* effective_rows = nullptr;
    if (cJSON_IsArray(rows_json)) {
        effective_rows = rows_json;
    } else if (HasKey(grid_json, "bind_rows")) {
        owned_rows = BuildBindRowsForRender(grid_json, card->data);
        effective_rows = owned_rows;
    }
    const cJSON* cols_meta = GetItem(grid_json, "cols");
    bool has_title = false;
    if (cJSON_IsArray(cols_meta)) {
        const cJSON* cd = nullptr;
        cJSON_ArrayForEach(cd, cols_meta)
            if (GetStr(cd, "title")) has_title = true;
    }

    std::map<int, std::vector<const cJSON*>> by_row;
    const cJSON* lc = nullptr;
    cJSON_ArrayForEach(lc, GetItem(layout_grid, "cells")) { by_row[GetInt(lc, "row", 0)].push_back(lc); }

    int seq = 0;
    for (auto& kv : by_row) {
        std::vector<const cJSON*>& row_cells = kv.second;
        lv_obj_t* rowobj = lv_obj_create(gobj);
        screen_strip_obj_chrome(rowobj);
        lv_obj_remove_flag(rowobj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(rowobj, &s_transp_bg, LV_PART_MAIN);
        lv_obj_set_width(rowobj, inner_w);
        lv_obj_set_height(rowobj, LV_SIZE_CONTENT);

        const bool is_header_row = has_title && seq == 0;
        const bool is_divider_row = has_title && seq == 1;
        ++seq;

        if (is_header_row) {
            RenderColsHeaderRow(rowobj, row_cells, cols_meta);
            continue;
        }
        if (is_divider_row) {
            RenderColsDividerRow(rowobj, row_cells, inner_w);
            continue;
        }

        std::vector<lv_obj_t*> leaf_objs;
        for (const cJSON* cellj : row_cells) {
            int ci = GetInt(cellj, "ci", -2);
            int x = GetInt(cellj, "x", 0);
            int w = GetInt(cellj, "w", 0);
            const char* align_s = GetStr(cellj, "align", "start");
            const char* wrap_mode = GetStr(cellj, "wrap", "nowrap");
            if (ci < 0) continue;  // 理论上不会出现在非 header/divider 行；防御性跳过

            const cJSON* leaf = nullptr;
            if (cells_json) {
                leaf = cJSON_GetArrayItem(cells_json, ci);
            } else if (effective_rows) {
                int r = ci / 100, c = ci % 100;
                const cJSON* rowarr = cJSON_GetArrayItem(effective_rows, r);
                leaf = rowarr ? cJSON_GetArrayItem(rowarr, c) : nullptr;
            }
            if (!leaf) continue;  // 防御：Validate 已过，理论上不会发生

            if (++node_count > limits.max_nodes) {
                err = "too many nodes (max " + std::to_string(limits.max_nodes) + ")";
                if (owned_rows) cJSON_Delete(owned_rows);
                return nullptr;
            }
            lv_obj_t* leafobj = BuildLeafWidget(rowobj, leaf, err);
            if (!leafobj) {
                if (owned_rows) cJSON_Delete(owned_rows);
                return nullptr;
            }
            FinishLeafWidget(leafobj, leaf, card, x, w, align_s, wrap_mode);
            leaf_objs.push_back(leafobj);
        }
        CenterRowCross(rowobj, leaf_objs);
    }

    if (owned_rows) cJSON_Delete(owned_rows);
    return gobj;
}

}  // namespace

// F1 修复：见 pi_card_render.h 头注。单独把 grid_spec 包成一个一元 root 数组喂 solver::Solve
// （与正式 RenderNode 同一套 Input 参数，只是 root 只有这一个 grid），复用 RenderGridBlock
// 建控件——保证 rebuild 出来的几何跟"这张卡从头整个重渲一遍"逐位一致，不会因为抄了另一条
// 路径而在 track 宽度/截断/对齐上跟首次渲染打架。
lv_obj_t* RebuildBindRowsGrid(lv_obj_t* old_gobj, const cJSON* grid_spec, UiCard* card,
                              int viewport_w, int gap, const RenderLimits& limits, std::string& err) {
    lv_obj_t* parent = lv_obj_get_parent(old_gobj);
    uint32_t idx = lv_obj_get_index(old_gobj);
    lv_obj_delete(old_gobj);  // 递归删子树；子控件的 DELETE 回调（如有）随之触发，无需手动清理

    cJSON* wrap = cJSON_CreateArray();
    cJSON_AddItemToArray(wrap, cJSON_Duplicate(grid_spec, 1));
    solver::Input in;
    in.root = wrap;
    in.data = card->data;
    in.viewport_w = viewport_w;
    in.gap = gap > 0 ? gap : solver::kStackGap;
    in.measure = SolverMeasureCb;
    in.measure_ctx = nullptr;
    cJSON* layout = solver::Solve(in);
    const cJSON* layout_grids = GetItem(layout, "grids");
    const cJSON* layout_grid = layout_grids ? cJSON_GetArrayItem(layout_grids, 0) : nullptr;
    const cJSON* wrapped_grid = cJSON_GetArrayItem(wrap, 0);

    lv_obj_t* gobj = nullptr;
    if (layout_grid) {
        int node_count = 0;
        gobj = RenderGridBlock(parent, wrapped_grid, layout_grid, card, viewport_w, in.gap,
                               node_count, limits, err);
    } else {
        err = "internal: solver produced no grid for bind_rows rebuild";
    }
    cJSON_Delete(layout);
    cJSON_Delete(wrap);
    if (gobj) lv_obj_move_to_index(gobj, idx);  // 挪回原来的兄弟顺序位置（delete+create 天然追加到末尾）
    return gobj;
}

lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow,
                     bool in_list_row, bool row_all_growable) {
    EnsureCardStyles();  // 惰性初始化共享几何样式（首次进入即建，覆盖全部下游入口）
    (void)depth;
    (void)parent_flow;
    (void)in_list_row;
    (void)row_all_growable;  // v2：树深恒为 2（card -> grid -> leaf），这些旧参数不再有意义，
                             // 仅为保留签名兼容其余调用点（见任务报告 e/f）。
    if (!cJSON_IsArray(node)) {
        err = "root must be an array of grid blocks (v2 schema; run Repair()+Validate() first)";
        return nullptr;
    }
    const int ngrid = cJSON_GetArraySize(node);
    if (ngrid < 1) {
        err = "root has no grid blocks";
        return nullptr;
    }

    const int viewport_w = ViewportWidthFor(card, parent);

    solver::Input in;
    in.root = node;
    in.data = card->data;
    in.viewport_w = viewport_w;
    in.gap = solver::kStackGap;
    in.measure = SolverMeasureCb;
    in.measure_ctx = nullptr;
    cJSON* layout = solver::Solve(in);
    const cJSON* layout_grids = GetItem(layout, "grids");

    lv_obj_t* card_root = lv_obj_create(parent);
    screen_strip_obj_chrome(card_root);
    lv_obj_remove_flag(card_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(card_root, &s_transp_bg, LV_PART_MAIN);
    lv_obj_set_width(card_root, viewport_w + 48);  // 48 = 2×24 卡片内边距，呼应旧 depth==0 pad
    lv_obj_set_height(card_root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card_root, solver::kStackGap, LV_PART_MAIN);
    pi_theme::ApplyBg(card_root, Tok::Card);
    lv_obj_set_style_bg_opa(card_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card_root, 18, LV_PART_MAIN);
    pi_theme::ApplyBorder(card_root, Tok::Line);
    lv_obj_set_style_border_width(card_root, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card_root, 24, LV_PART_MAIN);

    int gi = 0;
    const cJSON* grid_json = nullptr;
    cJSON_ArrayForEach(grid_json, node) {
        const cJSON* layout_grid = layout_grids ? cJSON_GetArrayItem(layout_grids, gi) : nullptr;
        if (!layout_grid) {
            err = "internal: solver layout/grid count mismatch";
            cJSON_Delete(layout);
            return nullptr;
        }
        lv_obj_t* gobj = RenderGridBlock(card_root, grid_json, layout_grid, card, viewport_w,
                                         solver::kStackGap, node_count, limits, err);
        if (!gobj) {
            cJSON_Delete(layout);
            return nullptr;  // 失败向上冒泡，host 删 root 整卡回滚
        }
        // F1 修复：bind_rows grid 登记一个 List 型 DataConsumer——没有它 ui_update 的
        // data.append/remove/replace/set 写了 card->data 之后没人触发这个 grid 重渲，工具调用
        // 却仍回 "(ok)"，LLM 以为生效了、画面纹丝不动（见任务报告根因）。grid_spec 存一份
        // card-owned 的完整 grid 块 JSON 进 json_pool——原始 spec 树在 OnRenderEvent 里
        // RenderNode 返回后就地 cJSON_Delete 了，consumer 活得比它长，必须自己持有一份。
        if (HasKey(grid_json, "bind_rows")) {
            UiCard::DataConsumer dc;
            dc.obj = gobj;
            dc.kind = UiCard::DataConsumer::List;
            dc.key = GetStr(grid_json, "bind_rows", "");
            cJSON* spec_dup = cJSON_Duplicate(grid_json, 1);
            card->json_pool.push_back(spec_dup);
            dc.grid_spec = spec_dup;
            dc.viewport_w = viewport_w;
            dc.gap = solver::kStackGap;
            dc.limits = limits;
            card->consumers.push_back(std::move(dc));
        }
        ++gi;
    }
    cJSON_Delete(layout);
    return card_root;
}

// ---------------------------------------------------------------------------
// 流式生长卡片 v2（docs/CARD_V2.md §4，改造4）：预览渲染。见 pi_card_render.h 的详细头注。
// v1 的生长边状态机（RenderPreviewNode/SyncPreviewNode/PreviewSyncContainer/USER_2 标记/
// s_leaf_sig/RefreshPreviewDataLabels，靠叶子级/容器级增量对齐维持前缀不变量）已整体删除。
// v2 树深恒为 2（card→grid→leaf），grid 是原子渲染单位：pi_card_preview.cc 按
// pi_card_preview_sig::GridSignature 判定"这个 grid 变没变"，变了就整块 lv_obj_delete 旧
// 容器、调用这里的 RenderGridBlockPreview 整块重建；没变就原样不动——不再需要按位置对齐的
// 逐节点同步，也不再需要 RefreshPreviewDataLabels 那样的全树回刷通道（data 已经折进签名，
// 迟到时相关 grid 的签名自然变化）。
namespace {

constexpr int kPreviewMaxNodes = 64;  // 同正式渲染节点上限（§6.1），预览超限静默停止建、不报错

// 预览期 bind 一次性取值（流式期 bind 控件不再空转占位）：不走 Acquire——那会动 refcount、
// 触发种子/活性刷新、对动态路径还会启动异步拉取，预览"零 bind 零 DataConsumer"的零副作用
// 契约不能破。用 ReadForWorker 直跑 getter 读快照：它只对 WorkerRead::Safe 的静态路径放行
// （getter 均非阻塞、线程安全，在 LVGL 线程跑与在 worker 线程跑等同安全，见 pi_card_data.h
// 头注），动态路径（stock.*）恒 false、Unsafe/未注册也 false——这些场景返回 false，调用方
// 维持缺省占位（"--"/0/JSON 静态值），等 adopt 后正式 bind 接管。取值是建控件时的一次快照，
// 不随流持续刷新（同一 grid 只要签名不变就不会重建，值也就不会跟着 DataHub 联动——这是"零
// 订阅"设计的直接后果，adopt 后正式渲染立刻接管保证最终值正确）。
bool PreviewPeekInt(const char* path, int& out) {
    if (path == nullptr) return false;
    HubValue v;
    if (!DataHub::Instance().ReadForWorker(path, v)) return false;
    if (const int* iv = std::get_if<int>(&v)) {
        out = *iv;
        return true;
    }
    if (const bool* bv = std::get_if<bool>(&v)) {
        out = *bv ? 1 : 0;
        return true;
    }
    return false;
}

// bind label 的预览直显：镜像 ApplyBind 的 label 分支口径——Int/Bool 走 fmt（缺省 "%d"，
// FmtSafeForType 兜底防 %s 套 int 崩 vsnprintf），String 直显 + SafeFont 防 mono 字体渲
// 中文豆腐块。返回是否成功取到值并落了文本；false 时调用方维持 "--" 占位。
bool PreviewSeedBindLabel(lv_obj_t* lbl, const cJSON* node, const char* path) {
    HubValue v;
    if (path == nullptr || !DataHub::Instance().ReadForWorker(path, v)) return false;
    if (const std::string* sv = std::get_if<std::string>(&v)) {
        const lv_font_t* f = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
        if (SafeFont(f, sv->c_str())) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
        lv_label_set_text(lbl, sv->c_str());
        return true;
    }
    int iv = 0;
    if (const int* ip = std::get_if<int>(&v)) iv = *ip;
    else if (const bool* bp = std::get_if<bool>(&v)) iv = *bp ? 1 : 0;
    else return false;
    HubType t = HubType::Int;
    DataHub::Instance().TypeOf(path, t);
    const char* fmt = GetStr(node, "fmt", "%d");
    std::string ferr;
    if (!FmtSafeForType(fmt, t, ferr)) fmt = "%d";
    lv_label_set_text_fmt(lbl, fmt, iv);
    return true;
}

// 尾部不完整 UTF-8 序列剪除后落 label：partial parser 补全未闭合字符串时按字节截断，快照
// 可能停在多字节字符中间（"三城�"），LVGL 会渲出替换乱码一闪而过。渲进预览 label/button 前
// 把结尾那半个字符剪掉（最多回看 3 字节找 lead byte，纯 ASCII 尾部零开销）。只影响预览观感；
// 正式渲染拿到的是校验过的完整 JSON，不经过这里。
void SetPreviewLabelText(lv_obj_t* lbl, const char* txt) {
    size_t len = std::strlen(txt);
    size_t cut = len;
    for (size_t back = 1; back <= 3 && back <= len; ++back) {
        unsigned char c = static_cast<unsigned char>(txt[len - back]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte，继续往前找 lead
        if ((c & 0x80) == 0) break;        // ASCII 收尾：完整，不剪
        size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (need > back) cut = len - back;  // 序列没凑齐字节数 → 从 lead 处剪断
        break;  // 找到 lead（或非法字节）即定论；再往前都是已完整的字符
    }
    const std::string cut_buf = (cut == len) ? std::string() : std::string(txt, cut);
    const char* final_txt = (cut == len) ? txt : cut_buf.c_str();
    const lv_font_t* f = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
    if (SafeFont(f, final_txt)) lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
    lv_label_set_text(lbl, final_txt);
}

// 预览期 bind_data 上下文：PreviewSetData/PreviewOnArgs 每帧把快照顶层 "data" 对象借出，
// 建标签时一次性直读（不是通道式回刷——data 变化会让引用它的 grid 签名变化、整块重渲，见
// pi_card_preview_sig.h 头注）；帧尾必须清空，指针不许跨帧存活。
const cJSON* s_preview_data = nullptr;

// 预览 label 文本决策，镜像正式渲染优先级：bind（DataHub 快照）> bind_data（partial data
// 直读）> 静态 text。bind/bind_data 取不到值时 "--" 占位；静态 text 含 {value}/{v} 视为
// bind_data 键还没流完的半成品模板，同样占位不裸渲。
void PreviewLabelText(lv_obj_t* lbl, const cJSON* node) {
    if (const char* bind = GetStr(node, "bind")) {
        if (!PreviewSeedBindLabel(lbl, node, bind)) lv_label_set_text(lbl, "--");
        return;
    }
    if (const char* dk = GetStr(node, "bind_data")) {
        std::string txt = "--";
        if (const cJSON* v = s_preview_data ? GetItem(s_preview_data, dk) : nullptr) {
            const char* tpl = GetStr(node, "text");
            txt = tpl ? SubstDataValue(tpl, v) : Stringify(v);
        }
        SetPreviewLabelText(lbl, txt.c_str());
        return;
    }
    if (const char* txt = GetStr(node, "text")) {
        if (std::strstr(txt, "{value}") != nullptr || std::strstr(txt, "{v}") != nullptr) {
            lv_label_set_text(lbl, "--");
            return;
        }
        SetPreviewLabelText(lbl, txt);
        return;
    }
    lv_label_set_text(lbl, "");  // text/bind/bind_data 全都还没流出来：置空，别渲 LVGL 默认 "Text"
}

// 预览专用叶子构造：与 BuildLeafWidget 同构（12 种类型中的 10 种——chart/stock_chart 数据
// 驱动或有网络副作用，预览期跳过不建、留空位，见函数头注），零 bind/事件/DataConsumer 注册，
// 值用上面几个"快照直读"辅助函数取。返回 nullptr 表示"这个位置预览期不建实体"（不是错误）。
lv_obj_t* BuildLeafWidgetPreview(lv_obj_t* parent, const cJSON* node) {
    const char* type = GetStr(node, "type");
    if (type == nullptr) return nullptr;  // 半吐的 cell（type 键还没吐出来）：安静跳过
    lv_obj_t* obj = nullptr;
    if (std::strcmp(type, "label") == 0) {
        obj = lv_label_create(parent);
        ApplyLabelStyle(obj, node);  // 先落字体/角色，PreviewSeedBindLabel 的 SafeFont 才有正确基准
        PreviewLabelText(obj, node);
    } else if (std::strcmp(type, "icon") == 0) {
        Tok tok = Tok::Dim;
        ToneTok(GetStr(node, "tone", "dim"), tok);
        obj = MakeIcon(parent, GetStr(node, "icon", GetStr(node, "name", "dot")),
                       GetInt(node, "size", 22), tok);
    } else if (std::strcmp(type, "button") == 0) {
        obj = lv_button_create(parent);
        lv_obj_t* lbl = lv_label_create(obj);
        const char* text = GetStr(node, "text", "");
        SetPreviewLabelText(lbl, text);
        Tok fg = ApplyButtonStyle(obj, lbl, node);
        if (const char* icon_name = GetStr(node, "icon")) {
            lv_obj_t* ic = MakeIcon(obj, icon_name, GetInt(node, "size", 20), fg);
            lv_obj_move_to_index(ic, 0);
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(obj, 8, LV_PART_MAIN);
            if (text[0] == '\0') lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_center(lbl);
        }
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);  // 预览不可交互，不挂 on_click
    } else if (std::strcmp(type, "slider") == 0 || std::strcmp(type, "arc") == 0 ||
               std::strcmp(type, "bar") == 0) {
        // 三个数值控件同构：JSON min/max/value 先落，bind 时镜像 ApplyBind 口径——DataHub
        // 量程（RangeOf）盖过 JSON 区间、快照值（PreviewPeekInt）盖过 JSON 静态 value，让
        // 流式期就显示真实音量/亮度/电量，adopt 换装零跳变；取不到快照（Unsafe/动态路径）
        // 维持 JSON 值或控件默认（0），不算错。
        const bool is_arc = std::strcmp(type, "arc") == 0;
        const bool is_bar = std::strcmp(type, "bar") == 0;
        obj = is_arc ? lv_arc_create(parent) : is_bar ? lv_bar_create(parent) : lv_slider_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;
        const char* bind = GetStr(node, "bind");
        int lo = 0, hi = 0;
        if (bind != nullptr && DataHub::Instance().RangeOf(bind, lo, hi)) { mn = lo; mx = hi; }
        int val = GetInt(node, "value", 0);
        bool has_val = HasKey(node, "value");
        if (int pv = 0; bind != nullptr && PreviewPeekInt(bind, pv)) { val = pv; has_val = true; }
        if (is_arc) {
            lv_arc_set_range(obj, mn, mx);
            if (has_val) lv_arc_set_value(obj, val);
        } else if (is_bar) {
            lv_bar_set_range(obj, mn, mx);
            if (has_val) lv_bar_set_value(obj, val, LV_ANIM_OFF);
        } else {
            lv_slider_set_range(obj, mn, mx);
            if (has_val) lv_slider_set_value(obj, val, LV_ANIM_OFF);
        }
        if (!is_bar) lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else if (std::strcmp(type, "switch") == 0) {
        obj = lv_switch_create(parent);
        bool checked = GetBool(node, "checked");
        if (int pv = 0; PreviewPeekInt(GetStr(node, "bind"), pv)) checked = pv != 0;  // 快照盖静态值
        if (checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else if (std::strcmp(type, "qrcode") == 0) {
        obj = lv_qrcode_create(parent);
        int sz = GetInt(node, "size", 160);
        if (sz < 96) sz = 96;
        else if (sz > 320) sz = 320;
        lv_qrcode_set_size(obj, sz);
        lv_qrcode_set_dark_color(obj, pi_theme::PaletteOf(true).tx);
        lv_qrcode_set_light_color(obj, pi_theme::PaletteOf(true).bg);
        const char* txt = GetStr(node, "text", "");
        lv_qrcode_update(obj, txt, static_cast<uint32_t>(std::strlen(txt)));
    } else if (std::strcmp(type, "choice") == 0) {
        obj = MakeChoice(parent, node);
        if (int pv = 0; PreviewPeekInt(GetStr(node, "bind"), pv)) ChoiceSetValue(obj, pv);  // 快照选中真实档位
    } else if (std::strcmp(type, "divider") == 0) {
        obj = lv_obj_create(parent);
        screen_strip_obj_chrome(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(obj, 1);
        pi_theme::ApplyBg(obj, Tok::Line);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        return nullptr;  // chart/stock_chart（数据驱动/有网络副作用）、未知/半吐 type：跳过不建
    }
    return obj;
}

// 落位 + 外观收尾（无 bind/事件/DataConsumer 注册）：口径同 FinishLeafWidget，去掉 card 相关的
// 一切（ApplyBind/AttachEvent/DataConsumer/死控件判定）。wrap_mode 语义同 FinishLeafWidget；
// 预览路径原本就不钳高（流式生长期间高度本就在变，钳死一行反而跟"随内容自然生长"的预览定位
// 冲突），这里维持这个既有取舍，只是判定依据从 role 猜测换成 solver 回填的 wrap 字段。
void FinishLeafWidgetPreview(lv_obj_t* obj, const cJSON* node, int x, int w, const char* align,
                             const char* wrap_mode) {
    const char* type = GetStr(node, "type");
    lv_obj_set_pos(obj, x, 0);
    if (!std::strcmp(type, "arc") || !std::strcmp(type, "qrcode")) {
        if (w > 0) lv_obj_set_size(obj, w, w);
    } else if (!std::strcmp(type, "divider")) {
        if (w > 0) lv_obj_set_width(obj, w);
    } else if (std::strcmp(type, "icon") != 0 && w > 0) {
        lv_obj_set_width(obj, w);
    }
    if (!std::strcmp(type, "label")) {
        const bool right = align && !std::strcmp(align, "end");
        if (right) lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        const bool dynamic_text = GetStr(node, "bind") || GetStr(node, "bind_data");
        const bool ellipsis = wrap_mode && !std::strcmp(wrap_mode, "ellipsis");
        if (ellipsis) {
            lv_label_set_long_mode(obj, dynamic_text ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_DOT);
        } else {
            lv_label_set_long_mode(obj, dynamic_text ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_WRAP);
        }
    }
    ApplyDefaultStyle(obj, type, /*depth=*/1);
}

}  // namespace

lv_obj_t* MakePreviewCardRoot(lv_obj_t* parent, int viewport_w) {
    EnsureCardStyles();
    // 镜像 RenderNode 顶层 card_root 的建法（圆角/边框/pad/竖排间距）——两处刻意保持同一份视觉
    // 常量，adopt 换装时观感零跳变；card_root 外观与 UiCard 无关，不需要card 指针。
    lv_obj_t* card_root = lv_obj_create(parent);
    screen_strip_obj_chrome(card_root);
    lv_obj_remove_flag(card_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(card_root, &s_transp_bg, LV_PART_MAIN);
    lv_obj_set_width(card_root, viewport_w + 48);  // 48 = 2×24 卡片内边距，呼应 RenderNode
    lv_obj_set_height(card_root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card_root, solver::kStackGap, LV_PART_MAIN);
    pi_theme::ApplyBg(card_root, Tok::Card);
    lv_obj_set_style_bg_opa(card_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card_root, 18, LV_PART_MAIN);
    pi_theme::ApplyBorder(card_root, Tok::Line);
    lv_obj_set_style_border_width(card_root, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card_root, 24, LV_PART_MAIN);
    return card_root;
}

lv_obj_t* RenderGridBlockPreview(lv_obj_t* card_root, const cJSON* grid_json, const cJSON* data,
                                 int viewport_w, int gap) {
    EnsureCardStyles();
    solver::Input in;
    cJSON* one = cJSON_CreateArray();
    cJSON_AddItemReferenceToArray(one, const_cast<cJSON*>(grid_json));  // 引用，Delete(one) 不连累 grid_json
    in.root = one;
    in.data = data;
    in.viewport_w = viewport_w;
    in.gap = gap;
    in.measure = SolverMeasureCb;
    in.measure_ctx = nullptr;
    cJSON* layout = solver::Solve(in);
    const cJSON* layout_grid = cJSON_GetArrayItem(GetItem(layout, "grids"), 0);

    lv_obj_t* gobj = lv_obj_create(card_root);
    screen_strip_obj_chrome(gobj);
    lv_obj_remove_flag(gobj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(gobj, &s_transp_bg, LV_PART_MAIN);
    lv_obj_set_width(gobj, viewport_w);
    lv_obj_set_height(gobj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gobj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(gobj, gap, LV_PART_MAIN);
    ApplyFill(gobj, grid_json);
    // 与正式 RenderGridBlock 同款 fill 内边距（solver 按收窄视口求解，这里补 pad）。
    const int inset = layout_grid ? GetInt(layout_grid, "inset", 0) : 0;
    const int inner_w = viewport_w - 2 * inset;
    if (inset > 0) lv_obj_set_style_pad_all(gobj, inset, LV_PART_MAIN);

    if (!layout_grid) {
        cJSON_Delete(layout);
        cJSON_Delete(one);
        return gobj;  // solver 没吐出这个 grid 的 layout（不该发生，防御性早退，留空盒子）
    }

    const cJSON* cells_json = GetItem(grid_json, "cells");
    const cJSON* rows_json = GetItem(grid_json, "rows");
    cJSON* owned_rows = nullptr;
    const cJSON* effective_rows = nullptr;
    if (cJSON_IsArray(rows_json)) {
        effective_rows = rows_json;
    } else if (HasKey(grid_json, "bind_rows")) {
        owned_rows = BuildBindRowsForRender(grid_json, data);  // card-agnostic，正式/预览共用
        effective_rows = owned_rows;
    }
    const cJSON* cols_meta = GetItem(grid_json, "cols");
    bool has_title = false;
    if (cJSON_IsArray(cols_meta)) {
        const cJSON* cd = nullptr;
        cJSON_ArrayForEach(cd, cols_meta)
            if (GetStr(cd, "title")) has_title = true;
    }

    std::map<int, std::vector<const cJSON*>> by_row;
    const cJSON* lc = nullptr;
    cJSON_ArrayForEach(lc, GetItem(layout_grid, "cells")) { by_row[GetInt(lc, "row", 0)].push_back(lc); }

    int seq = 0;
    int node_count = 0;
    for (auto& kv : by_row) {
        std::vector<const cJSON*>& row_cells = kv.second;
        lv_obj_t* rowobj = lv_obj_create(gobj);
        screen_strip_obj_chrome(rowobj);
        lv_obj_remove_flag(rowobj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(rowobj, &s_transp_bg, LV_PART_MAIN);
        lv_obj_set_width(rowobj, inner_w);
        lv_obj_set_height(rowobj, LV_SIZE_CONTENT);

        const bool is_header_row = has_title && seq == 0;
        const bool is_divider_row = has_title && seq == 1;
        ++seq;

        if (is_header_row) {
            RenderColsHeaderRow(rowobj, row_cells, cols_meta);
            continue;
        }
        if (is_divider_row) {
            RenderColsDividerRow(rowobj, row_cells, inner_w);
            continue;
        }

        std::vector<lv_obj_t*> leaf_objs;
        for (const cJSON* cellj : row_cells) {
            if (node_count >= kPreviewMaxNodes) break;  // 预算耗尽：静默停止生长，不报错
            int ci = GetInt(cellj, "ci", -2);
            int x = GetInt(cellj, "x", 0);
            int w = GetInt(cellj, "w", 0);
            const char* align_s = GetStr(cellj, "align", "start");
            const char* wrap_mode = GetStr(cellj, "wrap", "nowrap");
            if (ci < 0) continue;

            const cJSON* leaf = nullptr;
            if (cells_json) {
                leaf = cJSON_GetArrayItem(cells_json, ci);
            } else if (effective_rows) {
                int r = ci / 100, c = ci % 100;
                const cJSON* rowarr = cJSON_GetArrayItem(effective_rows, r);
                leaf = rowarr ? cJSON_GetArrayItem(rowarr, c) : nullptr;
            }
            if (!leaf) continue;

            lv_obj_t* leafobj = BuildLeafWidgetPreview(rowobj, leaf);
            if (!leafobj) continue;  // chart/stock_chart/半吐 type：跳过不建，不是错误
            ++node_count;
            FinishLeafWidgetPreview(leafobj, leaf, x, w, align_s, wrap_mode);
            leaf_objs.push_back(leafobj);
        }
        CenterRowCross(rowobj, leaf_objs);
    }

    if (owned_rows) cJSON_Delete(owned_rows);
    cJSON_Delete(layout);
    cJSON_Delete(one);
    return gobj;
}

int CountSpecNodes(const cJSON* node) {
    // v2：node 是信封的 "root" 数组（grid 块列表）。逐 grid 累计其 cells/rows/bind_rows 展开
    // 后的叶子数——口径必须与 kMaxGrids/64 节点上限同源（§6.1），bind_rows 按 max×行 cell 数
    // 预留（EffMax 已经把 max 夹在 [1,20]，与 Validate 的口径一致）。
    if (!cJSON_IsArray(node)) return 0;
    int count = 0;
    const cJSON* grid = nullptr;
    cJSON_ArrayForEach(grid, node) {
        ++count;  // grid 块自身也计一个节点（与 RenderGridBlockPreview 的 kPreviewMaxNodes 口径
                  // 松弛对齐；grid 数已经另有 kMaxGrids 上限，这里的 +1 只是让口径不低估）。
        const cJSON* cells = GetItem(grid, "cells");
        if (cJSON_IsArray(cells)) {
            count += cJSON_GetArraySize(cells);
            continue;
        }
        const cJSON* rows = GetItem(grid, "rows");
        if (cJSON_IsArray(rows)) {
            const cJSON* row = nullptr;
            cJSON_ArrayForEach(row, rows) {
                if (cJSON_IsArray(row)) count += cJSON_GetArraySize(row);
            }
            continue;
        }
        if (HasKey(grid, "bind_rows")) {
            const cJSON* item = GetItem(grid, "item");
            int item_cells = cJSON_IsArray(item) ? cJSON_GetArraySize(item) : 1;
            int max = HasKey(grid, "max") ? GetInt(grid, "max", 20) : 20;
            if (max < 1) max = 1;
            if (max > 20) max = 20;
            count += max * item_cells;
        }
    }
    return count;
}

void PreviewSetData(const cJSON* data) { s_preview_data = cJSON_IsObject(data) ? data : nullptr; }

bool ApplyProps(lv_obj_t* obj, const cJSON* props, std::string& err) {
    if (!cJSON_IsObject(props)) {
        err = "props is not an object";
        return false;
    }
    if (const char* txt = GetStr(props, "text")) {
        // do:'patch' 改 heading（puhui_24_4）标签文本也是一个动态落文本点，同样可能吐出缺字
        // 新中文——SafeFont 兜一遍（非 heading 字体空操作）。是本函数唯一改文本的地方，不必
        // 判 obj 是不是 label：ApplyProps 只会被派给带 text 的节点。
        const lv_font_t* f = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        if (SafeFont(f, txt)) lv_obj_set_style_text_font(obj, f, LV_PART_MAIN);
        lv_label_set_text(obj, txt);
    }
    if (HasKey(props, "value")) {
        int v = GetInt(props, "value", 0);
        if (lv_obj_check_type(obj, &lv_slider_class)) lv_slider_set_value(obj, v, LV_ANIM_ON);
        else if (lv_obj_check_type(obj, &lv_bar_class)) lv_bar_set_value(obj, v, LV_ANIM_ON);
        else if (lv_obj_check_type(obj, &lv_arc_class)) lv_arc_set_value(obj, v);
        else { int dummy; if (ChoiceValue(obj, dummy)) ChoiceSetValue(obj, v); }
    }
    if (const cJSON* chk = GetItem(props, "checked"); chk && cJSON_IsBool(chk)) {
        if (cJSON_IsTrue(chk)) lv_obj_add_state(obj, LV_STATE_CHECKED);
        else lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
    if (const cJSON* hid = GetItem(props, "hidden"); hid && cJSON_IsBool(hid)) {
        if (cJSON_IsTrue(hid)) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    Tok tok;
    lv_color_t c;
    if (ToneTok(GetStr(props, "tone"), tok)) pi_theme::ApplyText(obj, tok);
    else if (ParseHex(GetStr(props, "color"), c)) lv_obj_set_style_text_color(obj, c, LV_PART_MAIN);
    return true;
}

// choice 复合控件的取值/置值（公开 API，pi_card_actions.cc 跨 TU 调用）。ChoiceCtx 定义在本
// 文件顶部的匿名命名空间里，只在此 TU 可见——这两个包装函数就是它对外的唯一出口。
bool ChoiceValue(lv_obj_t* obj, int& out) {
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) return false;
    auto* ctx = static_cast<ChoiceCtx*>(lv_obj_get_user_data(obj));
    if (!ctx) return false;
    out = ctx->value;
    return true;
}

void ChoiceSetValue(lv_obj_t* obj, int idx) {
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) return;
    auto* ctx = static_cast<ChoiceCtx*>(lv_obj_get_user_data(obj));
    if (!ctx || ctx->count <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= ctx->count) idx = ctx->count - 1;
    ctx->value = idx;
    ChoiceRestyle(obj);
}

// 取当前选中段的按钮文本（字符串回流用：{label} token / CollectState 的 idx(label)）。
bool ChoiceLabel(lv_obj_t* obj, std::string& out) {
    if (!obj || !lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) return false;
    auto* ctx = static_cast<ChoiceCtx*>(lv_obj_get_user_data(obj));
    if (!ctx || ctx->value < 0 || ctx->value >= ctx->count) return false;
    lv_obj_t* lbl = lv_obj_get_child(ctx->btns[ctx->value], 0);
    if (lbl && lv_obj_check_type(lbl, &lv_label_class)) {
        out = lv_label_get_text(lbl);
        return true;
    }
    return false;
}

// ------------------------------- 干跑校验（v2） ------------------------------
// docs/CARD_V2.md §6.1：root = grid 块数组，每块恰含 cells/rows/bind_rows 之一，树深恒 2
// （grid → 叶子），叶子是 12 种类型之一。叶子级规则（bind 存在性/fmt 相容/数值控件禁绑
// string/qrcode 长度/choice 选项数/chart 历史/stock_chart/action 合法性）逐条复用 v1 已实现
// 的逻辑，未重写判定本身。
namespace {

constexpr int kMaxGrids = 8;  // §6.1：grid 数上限（root 数组长度）

bool IsLeafType(const char* t) {
    static const char* kLeaf[] = {"label", "button", "slider", "arc",   "switch",
                                 "bar",   "icon",   "divider", "qrcode", "choice",
                                 "chart", "stock_chart"};
    for (auto* k : kLeaf)
        if (!std::strcmp(k, t)) return true;
    return false;
}

// 先扫一遍收集全树已声明的 id，供 toggle/show/hide/patch 的 target 校验。递归口径与渲染器
// 一致：只有 cells/rows 里的叶子会被渲染出真实控件；bind_rows 的 "item" 行模板不收集
// （N 行实例 id 不唯一，同 v1 list 语义，toggle/show/hide/patch 校验期已对行内动作整体收窄）。
void CollectLeafId(const cJSON* leaf, std::set<std::string>& ids) {
    if (!cJSON_IsObject(leaf)) return;
    if (const char* id = GetStr(leaf, "id")) ids.insert(id);
}
void CollectRowIds(const cJSON* row, std::set<std::string>& ids) {
    if (!cJSON_IsArray(row)) return;
    const cJSON* c = nullptr;
    cJSON_ArrayForEach(c, row) CollectLeafId(c, ids);
}
void CollectGridIds(const cJSON* grid, std::set<std::string>& ids) {
    if (!cJSON_IsObject(grid)) return;
    if (const cJSON* cells = GetItem(grid, "cells"); cJSON_IsArray(cells)) {
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cells) CollectLeafId(c, ids);
    } else if (const cJSON* rows = GetItem(grid, "rows"); cJSON_IsArray(rows)) {
        const cJSON* r = nullptr;
        cJSON_ArrayForEach(r, rows) CollectRowIds(r, ids);
    }
}
void CollectNodeIdsV2(const cJSON* root, std::set<std::string>& ids) {
    if (!cJSON_IsArray(root)) return;
    const cJSON* g = nullptr;
    cJSON_ArrayForEach(g, root) CollectGridIds(g, ids);
}

// 校验单个叶子 cell（12 种之一）。逐条规则原样照抄 v1 ValidateNode 里对应类型的判定，
// 只是去掉了容器（column/row/grid/list）分支——v2 里叶子不可能是容器，出现即拒绝。
// in_row：是否位于 bind_rows 的 "item" 行模板内（受限 action 集合：仅 report/set/close，
// 复用 ValidateActions 的 in_list_row 语义）。
bool ValidateLeaf(const cJSON* node, const std::set<std::string>& node_ids, std::string& err,
                  bool in_row, int& count, const RenderLimits& limits) {
    if (!cJSON_IsObject(node)) {
        err = "cell is not an object";
        return false;
    }
    if (++count > limits.max_nodes) {
        err = "too many nodes (max " + std::to_string(limits.max_nodes) + ")";
        return false;
    }
    if (HasKey(node, "children")) {
        err = "leaf cells can't have children — tree depth is fixed at card -> grid -> leaf";
        return false;
    }
    const char* type = GetStr(node, "type");
    if (!type) {
        err = "cell missing type";
        return false;
    }
    if (!IsLeafType(type)) {
        if (!std::strcmp(type, "column") || !std::strcmp(type, "row") ||
            !std::strcmp(type, "grid") || !std::strcmp(type, "list")) {
            err = std::string("no nested containers allowed inside a grid cell (got type: '") +
                  type + "'); a card is only card -> grid -> leaf, two levels deep";
        } else {
            err = std::string("unknown type: ") + type;
        }
        return false;
    }
    if (std::strcmp(type, "chart") == 0) {
        const char* path = GetStr(node, "bind_history");
        if (!path || !DataHub::Instance().HasHistory(path)) {
            std::string avail;
            for (const auto& m : DataHub::Instance().ListPaths()) {
                if (!m.has_history) continue;
                if (!avail.empty()) avail += ", ";
                avail += m.path;
            }
            err = std::string("chart bind_history '") + (path ? path : "") +
                  "' has no history; available: " + avail;
            return false;
        }
    }
    if (std::strcmp(type, "stock_chart") == 0) {
        if (!pi_card_stock::ValidateNode(node, err)) return false;
    }
    if (std::strcmp(type, "qrcode") == 0) {
        const char* txt = GetStr(node, "text");
        if (!txt || !txt[0]) {
            err = "qrcode needs a non-empty text";
            return false;
        }
        if (std::strlen(txt) > 256) {
            err = "qrcode text too long (max 256 bytes); shorten the URL/payload";
            return false;
        }
    }
    if (std::strcmp(type, "choice") == 0) {
        const cJSON* opts = GetItem(node, "options");
        if (!opts || !cJSON_IsArray(opts)) {
            err = "choice needs an \"options\" array";
            return false;
        }
        int n = cJSON_GetArraySize(opts);
        if (n < 2 || n > 6) {
            err = "choice needs 2 to 6 options";
            return false;
        }
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, opts) {
            if (!cJSON_IsString(it)) {
                err = "choice options must all be strings";
                return false;
            }
        }
    }
    // bind 路径必须已注册（或命中动态 provider 的合法模式）
    if (const char* path = GetStr(node, "bind")) {
        if (!DataHub::Instance().Has(path)) {
            // media.* 已整体注销（播控只走内置播放器，见 pi_card_media/host Init 注释）：给
            // 弱模型一条有出路的拒绝——generic "unknown bind path" 只会让它换个路径名连环
            // 重试，drain 侧 SpecUsesMedia 的话术又因 worker 先拒而永远到不了。
            if (std::strncmp(path, "media.", 6) == 0) {
                err = "media.* paths don't exist — never render player UI, the built-in player "
                      "appears automatically; confirm playback in plain text instead";
                return false;
            }
            err = std::string("unknown bind path: ") + path;
            if (const char* h = DataHub::Instance().HintFor(path)) err += std::string("; ") + h;
            return false;
        }
        if (std::strcmp(type, "label") == 0) {
            if (const char* fmt = GetStr(node, "fmt")) {
                HubType t = HubType::Int;
                DataHub::Instance().TypeOf(path, t);
                if (!FmtSafeForType(fmt, t, err)) return false;
            }
        }
        if (std::strcmp(type, "slider") == 0 || std::strcmp(type, "arc") == 0 ||
            std::strcmp(type, "bar") == 0 || std::strcmp(type, "switch") == 0 ||
            std::strcmp(type, "choice") == 0) {
            HubType t = HubType::Int;
            DataHub::Instance().TypeOf(path, t);
            if (t == HubType::String) {
                err = std::string(type) + " cannot bind a string path '" + path +
                      "'; bind a number path or use a label with fmt \"%s\"";
                return false;
            }
        }
    }
    if (!ValidateActions(GetItem(node, "on_click"), node_ids, err, in_row)) return false;
    if (!ValidateActions(GetItem(node, "on_change"), node_ids, err, in_row)) return false;
    if (!ValidateActions(GetItem(node, "on_release"), node_ids, err, in_row)) return false;
    return true;
}

bool ValidateCellArray(const cJSON* arr, const std::set<std::string>& node_ids, std::string& err,
                       bool in_row, int& count, const RenderLimits& limits) {
    const cJSON* c = nullptr;
    cJSON_ArrayForEach(c, arr) {
        if (!ValidateLeaf(c, node_ids, err, in_row, count, limits)) return false;
    }
    return true;
}

// 一个 grid 块：恰含 cells/rows/bind_rows 之一（§6.1）。
bool ValidateGrid(const cJSON* grid, const std::set<std::string>& node_ids, const cJSON* data,
                  std::string& err, int& count, const RenderLimits& limits) {
    if (!cJSON_IsObject(grid)) {
        err = "each root element must be a grid block object";
        return false;
    }
    const bool has_cells = HasKey(grid, "cells");
    const bool has_rows = HasKey(grid, "rows");
    const bool has_bind_rows = HasKey(grid, "bind_rows");
    const int forms = (has_cells ? 1 : 0) + (has_rows ? 1 : 0) + (has_bind_rows ? 1 : 0);
    if (forms != 1) {
        err = forms == 0
                  ? "grid block needs exactly one of cells/rows/bind_rows (has none)"
                  : "grid block must contain exactly one of cells/rows/bind_rows (has more than one)";
        return false;
    }

    if (has_cells) {
        const cJSON* cells = GetItem(grid, "cells");
        if (!cJSON_IsArray(cells) || cJSON_GetArraySize(cells) == 0) {
            err = "cells must be a non-empty array of leaf cells";
            return false;
        }
        return ValidateCellArray(cells, node_ids, err, /*in_row=*/false, count, limits);
    }

    if (has_rows) {
        const cJSON* rows = GetItem(grid, "rows");
        if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0) {
            err = "rows must be a non-empty 2D array (array of row arrays)";
            return false;
        }
        const cJSON* row = nullptr;
        cJSON_ArrayForEach(row, rows) {
            if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) == 0) {
                err = "each row must be a non-empty array of leaf cells";
                return false;
            }
            if (!ValidateCellArray(row, node_ids, err, /*in_row=*/false, count, limits)) return false;
        }
        return true;
    }

    // bind_rows：需要 item（叶子或叶子数组）+ bind_rows（data key）。行内 action 已被
    // ValidateLeaf 的 in_row=true 收窄到 report/set/close（复用 ValidateActions 语义）。
    const cJSON* item = GetItem(grid, "item");
    if (!item || !(cJSON_IsObject(item) || cJSON_IsArray(item))) {
        err = "bind_rows needs an 'item' leaf or leaf array (the row template)";
        return false;
    }
    const char* key = GetStr(grid, "bind_rows");
    if (!key || !key[0]) {
        err = "bind_rows needs a non-empty data key";
        return false;
    }
    int tmpl_cells = cJSON_IsArray(item) ? cJSON_GetArraySize(item) : 1;
    if (tmpl_cells < 1) tmpl_cells = 1;
    int tmpl_probe = 0;  // 独立探针计数：只校验模板结构本身，不占用真实 64 节点账本
    if (cJSON_IsArray(item)) {
        if (cJSON_GetArraySize(item) == 0) {
            err = "bind_rows item array must not be empty";
            return false;
        }
        if (!ValidateCellArray(item, node_ids, err, /*in_row=*/true, tmpl_probe, limits)) return false;
    } else {
        if (!ValidateLeaf(item, node_ids, err, /*in_row=*/true, tmpl_probe, limits)) return false;
    }
    const cJSON* arr = data ? GetItem(data, key) : nullptr;
    int len = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    int eff = EffMax(grid, len);
    count += eff * tmpl_cells;  // 预留：按 eff_max×模板 cell 数记账，≥render 实际用量
    if (count > limits.max_nodes) {
        err = "bind_rows reserves " + std::to_string(eff * tmpl_cells) +
              " nodes (max×template cells); over " + std::to_string(limits.max_nodes) +
              " — lower max or simplify the row template";
        return false;
    }
    return true;
}

}  // namespace

bool Validate(const cJSON* root_node, const cJSON* data, std::string& err) {
    if (!cJSON_IsArray(root_node)) {
        err = "root must be an array of grid blocks: [{\"cells\":[...]}|{\"rows\":[...]}|"
              "{\"bind_rows\":\"key\",\"item\":...}, ...]";
        return false;
    }
    int ngrid = cJSON_GetArraySize(root_node);
    if (ngrid < 1) {
        err = "root must have at least 1 grid block";
        return false;
    }
    if (ngrid > kMaxGrids) {
        err = "root has " + std::to_string(ngrid) + " grid blocks (max " +
              std::to_string(kMaxGrids) + "); split into a smaller card";
        return false;
    }
    RenderLimits limits;
    // 预统计整卡节点数（与下方逐叶校验完全同口径：cells/rows 按叶子数、bind_rows 按
    // EffMax×模板 cell 数预留）——超限时把「实际声明了多少」带给模型。只回 "too many nodes
    // (max 64)" 弱模型无从校准该砍多少，会盲目微调连环重试（serial_ppa_on.log 三连拒实录）；
    // 带上具体数字一步收敛。逐叶的 ++count 护栏保留作兜底。
    int declared = 0;
    const cJSON* pg = nullptr;
    cJSON_ArrayForEach(pg, root_node) {
        if (!cJSON_IsObject(pg)) continue;
        if (const cJSON* cells = GetItem(pg, "cells"); cJSON_IsArray(cells)) {
            declared += cJSON_GetArraySize(cells);
        } else if (const cJSON* rows = GetItem(pg, "rows"); cJSON_IsArray(rows)) {
            const cJSON* row = nullptr;
            cJSON_ArrayForEach(row, rows) {
                if (cJSON_IsArray(row)) declared += cJSON_GetArraySize(row);
            }
        } else if (HasKey(pg, "bind_rows")) {
            const cJSON* item = GetItem(pg, "item");
            int tmpl = cJSON_IsArray(item) ? cJSON_GetArraySize(item) : 1;
            if (tmpl < 1) tmpl = 1;
            const char* key = GetStr(pg, "bind_rows");
            const cJSON* arr = (data && key) ? GetItem(data, key) : nullptr;
            declared += EffMax(pg, cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0) * tmpl;
        }
    }
    if (declared > limits.max_nodes) {
        err = "card declares " + std::to_string(declared) + " nodes (max " +
              std::to_string(limits.max_nodes) + "); cut at least " +
              std::to_string(declared - limits.max_nodes) +
              " leaves — split into multiple cards, or lower bind_rows max";
        return false;
    }
    std::set<std::string> node_ids;
    CollectNodeIdsV2(root_node, node_ids);  // 两遍：target 可以前向引用数组里靠后声明的 grid
    int count = 0;
    const cJSON* g = nullptr;
    cJSON_ArrayForEach(g, root_node) {
        if (!ValidateGrid(g, node_ids, data, err, count, limits)) return false;
    }
    return true;
}

// --------------------------------- Repair（v2） ------------------------------
// docs/CARD_V2.md §6.2：能修则修、不重试；正确性/安全类问题仍留给 Validate 同步拒绝。
namespace {

// 静默剥除的旧属性（grid 块级 + 叶子级通用，见 §6.2 表第一行）。"cols" 不在此列——它在
// rows/bind_rows 形态里是合法的表头描述数组，只有值为纯数字（旧 fr 权重）时才当作旧属性剥除。
constexpr const char* kStaleKeys[] = {"justify", "align",  "grow", "gap", "pad", "w",
                                      "h",       "span",   "size", "color", "bg"};

void StripStaleKeys(cJSON* obj) {
    if (!cJSON_IsObject(obj)) return;
    for (auto* k : kStaleKeys) cJSON_DeleteItemFromObject(obj, k);
}

// 旧 fr 权重 cols（元素全是数字）在 rows/bind_rows 形态里没有意义（v2 cols 只描述 title/num
// 表头），静默整体剥除，solver/渲染器改按内容/行 cell 数自动定列。cells 形态的 cols 本就无
// 意义，出现即剥除（不区分新旧写法）。
void StripStaleColsIfLegacy(cJSON* grid, bool is_cells_form) {
    cJSON* cols = cJSON_GetObjectItem(grid, "cols");
    if (!cJSON_IsArray(cols)) return;
    if (is_cells_form) {
        cJSON_DeleteItemFromObject(grid, "cols");
        return;
    }
    bool all_scalar = cJSON_GetArraySize(cols) > 0;
    const cJSON* c = nullptr;
    cJSON_ArrayForEach(c, cols) {
        if (!cJSON_IsNumber(c) && !cJSON_IsString(c)) { all_scalar = false; break; }
    }
    if (all_scalar) cJSON_DeleteItemFromObject(grid, "cols");  // 旧 fr 权重 / "auto" 轨道声明
}

// 一个叶子 cell 数组（cells[] / 某一 row[] / bind_rows 的 item[]）级别的修复：
//  * 剥除每个叶子上的旧属性；
//  * 删除旧 spacer 节点，若它夹在两个 cell 之间则给紧随其后的 cell 补 side:"end"（启发式，
//    仅当 spacer 既不是首元素也不是末元素时才补，对应 "两 cell 之间" 的字面意思）。
// 注意：column/row/grid/list 等容器类型叶子**不**在此做自动扁平化——即便只嵌套一层，语义
// 也无法安全展开（它们携带布局属性，简单提升 children 可能改变原意），统一交给 Validate
// 以 "no nested containers" 拒绝重试，让模型显式改写。
cJSON* RepairCellArray(cJSON* arr, bool* changed) {
    if (!cJSON_IsArray(arr)) return arr;
    cJSON* out = cJSON_CreateArray();
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        cJSON* cell = cJSON_GetArrayItem(arr, i);
        const char* type = GetStr(cell, "type");
        if (type && !std::strcmp(type, "spacer")) {
            if (changed) *changed = true;
            // spacer 在两个真实 cell 之间：给下一个即将追加进 out 的 cell 补 side:end。
            // 用一个待处理标记：下一次追加非 spacer 时补上。
            bool between = (i > 0) && (i < n - 1);
            if (between) {
                // 找下一个非 spacer 的原始节点在 arr 里的位置，直接改写它自己的 cJSON（还没被
                // 复制进 out 之前）。简单起见：标记到 "cell" 本身之后遇到的第一个真实叶子。
                for (int j = i + 1; j < n; ++j) {
                    cJSON* nxt = cJSON_GetArrayItem(arr, j);
                    const char* nt = GetStr(nxt, "type");
                    if (nt && !std::strcmp(nt, "spacer")) continue;
                    if (!HasKey(nxt, "side")) cJSON_AddStringToObject(nxt, "side", "end");
                    break;
                }
            }
            continue;  // spacer 本身不进 out
        }
        cJSON* dup = cJSON_Duplicate(cell, 1);
        StripStaleKeys(dup);
        cJSON_AddItemToArray(out, dup);
    }
    return out;
}

void ReplaceCellArray(cJSON* grid, const char* key) {
    cJSON* arr = cJSON_GetObjectItem(grid, key);
    if (!cJSON_IsArray(arr)) return;
    bool changed = false;
    cJSON* fixed = RepairCellArray(arr, &changed);
    cJSON_ReplaceItemInObject(grid, key, fixed);
}

// 单个 grid 块的修复：剥属性 + cols 长度纠偏 + 旧 list→bind_rows 改写 + cells/rows 内容修复。
// 返回值：该数组元素最终应替换成的 cJSON（可能是原地修改后的同一个指针，也可能是新分配的）。
cJSON* RepairGrid(cJSON* grid, std::vector<std::string>* notes) {
    if (!cJSON_IsObject(grid)) return grid;

    // 旧 v1 "list" 节点（type:"list", item, bind_data, max?, empty?）→ 改写成 bind_rows 形态。
    // 注意不能在此 cJSON_Delete(grid)：grid 仍挂在 root 数组里，调用方 Repair() 拿到新指针后
    // 走 cJSON_ReplaceItemInArray，由它解链并删除旧节点——这里先删会让替换沿着已释放节点的
    // prev/next 走（UAF + double-free）。
    if (const char* t = GetStr(grid, "type"); t && !std::strcmp(t, "list")) {
        cJSON* item = cJSON_DetachItemFromObject(grid, "item");
        const char* bind_data = GetStr(grid, "bind_data");
        cJSON* out = cJSON_CreateObject();
        if (item) cJSON_AddItemToObject(out, "item", item);
        cJSON_AddStringToObject(out, "bind_rows", bind_data ? bind_data : "");
        if (HasKey(grid, "max")) cJSON_AddNumberToObject(out, "max", GetInt(grid, "max", 8));
        if (const char* empty = GetStr(grid, "empty")) cJSON_AddStringToObject(out, "empty", empty);
        if (notes) notes->push_back("legacy list node rewritten to bind_rows");
        return out;
    }

    // 叶子被直接当块用：{"type":"divider"}（裸叶子，可带完整叶子属性）或速记 {"divider":null}/
    // {"divider":{}}（真机实录 2026-08-01：root 末尾塞 {"divider":null} 被零形态拒绝、白烧一轮
    // 重试）。包成单叶 cells 块 + note。必须在块级事件剥除之前做——裸叶子的 on_click 属于
    // 叶子，要保留。真零形态/未知键仍落到函数末尾 return，由 Validate 给可修复错误。
    if (!HasKey(grid, "cells") && !HasKey(grid, "rows") && !HasKey(grid, "bind_rows")) {
        cJSON* leaf = nullptr;
        const char* t = GetStr(grid, "type");
        cJSON* only = grid->child;
        if (t && IsLeafType(t)) {
            leaf = cJSON_Duplicate(grid, 1);
        } else if (only && only->next == nullptr && only->string && IsLeafType(only->string) &&
                   (cJSON_IsNull(only) || cJSON_IsTrue(only) || cJSON_IsObject(only))) {
            leaf = cJSON_IsObject(only) ? cJSON_Duplicate(only, 1) : cJSON_CreateObject();
            cJSON_DeleteItemFromObject(leaf, "type");  // 防 value 对象里也带 type 撞车
            cJSON_AddStringToObject(leaf, "type", only->string);
        }
        if (leaf) {
            StripStaleKeys(leaf);
            cJSON* out = cJSON_CreateObject();
            cJSON* cells = cJSON_AddArrayToObject(out, "cells");
            cJSON_AddItemToArray(cells, leaf);
            if (notes) {
                notes->push_back("a bare leaf was used as a grid block — wrapped into a one-cell "
                                 "cells block (every grid needs cells/rows/bind_rows)");
            }
            return out;
        }
    }

    StripStaleKeys(grid);  // grid 块级旧属性

    // 实测弱模型会把 on_click 挂到 grid 块上（对 "row tap" 的误读）——渲染器只认叶子事件，
    // 静默忽略等于交互悄悄丢失且无从得知。剥掉并 note 告知正确挂法（宽进严出，不硬拒）。
    {
        bool had_evt = false;
        for (const char* k : {"on_click", "on_change", "on_release"}) {
            if (HasKey(grid, k)) {
                cJSON_DeleteItemFromObject(grid, k);
                had_evt = true;
            }
        }
        if (had_evt && notes) {
            notes->push_back(
                "grid-level on_click/on_change/on_release were ignored — attach events to a leaf "
                "inside cells/rows/item instead");
        }
    }

    const bool is_cells_form = HasKey(grid, "cells");
    StripStaleColsIfLegacy(grid, is_cells_form);

    if (is_cells_form) {
        ReplaceCellArray(grid, "cells");
        return grid;
    }
    if (cJSON* rows = cJSON_GetObjectItem(grid, "rows"); cJSON_IsArray(rows)) {
        // 逐行剥属性/spacer 修复。裸叶子对象（模型忘了 rows 是二维、写成一维叶子数组——真机
        // 实录连续两次原样重试）包成单格行；schema 已放开 rows 内层约束让它到得了这里。
        int nrows = cJSON_GetArraySize(rows);
        cJSON* fixed_rows = cJSON_CreateArray();
        int max_cells = 0;
        bool wrapped_bare = false;
        for (int i = 0; i < nrows; ++i) {
            cJSON* row = cJSON_GetArrayItem(rows, i);
            bool changed = false;
            cJSON* fixed_row;
            if (cJSON_IsArray(row)) {
                fixed_row = RepairCellArray(row, &changed);
            } else if (cJSON_IsObject(row)) {
                cJSON* leaf = cJSON_Duplicate(row, 1);
                StripStaleKeys(leaf);
                fixed_row = cJSON_CreateArray();
                cJSON_AddItemToArray(fixed_row, leaf);
                wrapped_bare = true;
            } else {
                fixed_row = cJSON_Duplicate(row, 1);
            }
            int rc = cJSON_IsArray(fixed_row) ? cJSON_GetArraySize(fixed_row) : 0;
            if (rc > max_cells) max_cells = rc;
            cJSON_AddItemToArray(fixed_rows, fixed_row);
        }
        if (wrapped_bare && notes) {
            notes->push_back(
                "rows must be 2-D (each row an ARRAY of leaves); bare leaf objects were wrapped "
                "into single-cell rows");
        }
        cJSON_ReplaceItemInObject(grid, "rows", fixed_rows);
        // cols 长度纠偏（§6.2）：更短则补空列，更长则截断。
        if (cJSON* cols = cJSON_GetObjectItem(grid, "cols"); cJSON_IsArray(cols)) {
            int ncols = cJSON_GetArraySize(cols);
            if (ncols != max_cells && max_cells > 0) {
                cJSON* fixed_cols = cJSON_CreateArray();
                for (int c = 0; c < max_cells; ++c) {
                    if (c < ncols) cJSON_AddItemToArray(fixed_cols, cJSON_Duplicate(cJSON_GetArrayItem(cols, c), 1));
                    else cJSON_AddItemToArray(fixed_cols, cJSON_CreateObject());
                }
                cJSON_ReplaceItemInObject(grid, "cols", fixed_cols);
                if (notes) notes->push_back("cols length adjusted to match row cell count");
            }
        }
        return grid;
    }
    if (HasKey(grid, "bind_rows")) {
        // item 模板（叶子或叶子数组）同样剥属性；不做 spacer 启发式（行模板里 spacer 罕见且
        // 语义不明确——一行只渲一次模板，"两 cell 之间" 的相邻关系在多行重复展开后会失真）。
        if (cJSON* item = cJSON_GetObjectItem(grid, "item"); cJSON_IsArray(item)) {
            bool changed = false;
            cJSON* fixed = RepairCellArray(item, &changed);
            cJSON_ReplaceItemInObject(grid, "item", fixed);
        } else if (cJSON_IsObject(item)) {
            StripStaleKeys(item);
        }
        return grid;
    }
    return grid;  // 多形态/零形态等结构性问题留给 Validate 拒绝
}

}  // namespace

bool Repair(cJSON* envelope, std::string& err, std::vector<std::string>* notes) {
    if (!cJSON_IsObject(envelope)) return true;  // 非对象：不是本函数能处理的形状，交给上游
    cJSON* root = cJSON_GetObjectItem(envelope, "root");
    if (!root) return true;  // 缺 root：Validate 自己会报 "missing root" 之类，无需在此处理

    if (cJSON_IsObject(root)) {
        if (HasKey(root, "preset") || HasKey(root, "slots")) {
            err = "preset 已移除，请直接给 root grid 数组，示例见 system prompt";
            return false;
        }
        // "children" 键是旧深层树的标志——无法安全展开成 v2 单 grid 块，原样留给 Validate
        // 以 "root must be an array" 拒绝，不强行包数组制造一张看似合法实则语义全错的卡。
        if (!HasKey(root, "children")) {
            cJSON* wrapped = cJSON_CreateArray();
            cJSON_AddItemToArray(wrapped, cJSON_Duplicate(root, 1));
            cJSON_ReplaceItemInObject(envelope, "root", wrapped);
            if (notes) notes->push_back("root was a single grid object, wrapped into a 1-element array");
        }
        root = cJSON_GetObjectItem(envelope, "root");
    }

    if (cJSON_IsArray(root)) {
        int n = cJSON_GetArraySize(root);
        // 宽进严出：>kMaxGrids 不再硬拒（弱模型拿到 pi-c/Validate 的干拒绝后往往越改越错、
        // 连环重试），截到上限渲染 + note 告知——LLM 下一张自然学会拆卡。
        if (n > kMaxGrids) {
            for (int i = n - 1; i >= kMaxGrids; --i) cJSON_DeleteItemFromArray(root, i);
            if (notes)
                notes->push_back("root had " + std::to_string(n) + " grids; only the first " +
                                 std::to_string(kMaxGrids) +
                                 " were rendered — split into multiple cards");
            n = kMaxGrids;
        }
        for (int i = 0; i < n; ++i) {
            cJSON* grid = cJSON_GetArrayItem(root, i);
            cJSON* fixed = RepairGrid(grid, notes);
            if (fixed != grid) cJSON_ReplaceItemInArray(root, i, fixed);
        }
    }
    return true;
}

// --------------------------------- Lint ------------------------------------
namespace {
// 动作数组里是否挂了 report——on_change 挂 report 是最贵的反模式（见 Lint 规则 3）。
bool ActionsHaveReport(const cJSON* arr) {
    if (!arr || !cJSON_IsArray(arr)) return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        const char* d = GetStr(item, "do");
        if (d && std::strcmp(d, "report") == 0) return true;
    }
    return false;
}

// choice 的 on_change 数组里有没有任何「回流」动作（report/set/patch）——没有就说明这个
// choice 的选择结果哪儿都去不了（F3：choice 专属检查，见 Lint() 规则清单）。
bool ActionsHaveOutlet(const cJSON* arr) {
    if (!arr || !cJSON_IsArray(arr)) return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        const char* d = GetStr(item, "do");
        if (d && (!std::strcmp(d, "report") || !std::strcmp(d, "set") || !std::strcmp(d, "patch")))
            return true;
    }
    return false;
}

struct LintState {
    int node_count = 0;
    int grid_count = 0;
    int primary_count = 0;
    bool has_label = false;
    bool emoji_seen = false;  // 整卡只提示一次，别按节点刷屏
    std::vector<std::string>* hints = nullptr;
};

// 设备字体（puhui/mono/lucide）没有 emoji 字形，text 里的 emoji 上屏就是缺字豆腐块（sim
// 控制中心卡的 🔊💡⚙️⏱📡🎵 全部复现）。粗扫 UTF-8 码点：补充平面 + BMP 杂项符号/技术符号
// /箭头符号段（含变体选择符 FE0F）视为 emoji；℃(0x2103)、°(0xB0)、·(0xB7) 等正常排版符号
// 不在范围内，不误伤。
bool TextHasEmoji(const char* s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0) {
        uint32_t cp = 0;
        int len = 1;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            len = 4;
        }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = i; break; }  // 截断/坏序列：按已读部分推进
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += len;
        if (cp == 0xFE0F || cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) ||
            (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2B00 && cp <= 0x2BFF)) {
            return true;
        }
    }
    return false;
}

// invoke 动作若指向 confirm 级命令，点击会弹固件确认 sheet——非阻断提醒 LLM 别把它当
// 零往返的本地动作用（用户可能取消，流程要能处理「没发生」这一支）。
void LintInvokeConfirm(const cJSON* actions, std::vector<std::string>* hints) {
    if (!actions || !cJSON_IsArray(actions)) return;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, actions) {
        const char* d = GetStr(item, "do");
        if (!d || std::strcmp(d, "invoke") != 0) continue;
        const char* cmd = GetStr(item, "cmd");
        if (!cmd) continue;
        pi_card::CmdLevel lvl;
        if (CommandRegistry::Instance().LevelOf(cmd, lvl) && lvl == pi_card::CmdLevel::Confirm) {
            hints->push_back(std::string("invoke cmd '") + cmd +
                             "' is confirm-level: tapping it pops a firmware confirm sheet, it "
                             "doesn't execute immediately.");
        }
    }
}

void LintLeaf(const cJSON* leaf, const cJSON* data, LintState& st) {
    if (!cJSON_IsObject(leaf)) return;
    ++st.node_count;
    const char* type = GetStr(leaf, "type");
    if (!type) return;

    LintInvokeConfirm(GetItem(leaf, "on_click"), st.hints);
    LintInvokeConfirm(GetItem(leaf, "on_change"), st.hints);
    LintInvokeConfirm(GetItem(leaf, "on_release"), st.hints);

    if (!st.emoji_seen) {
        const char* text = GetStr(leaf, "text");
        if (text != nullptr && TextHasEmoji(text)) {
            st.emoji_seen = true;
            st.hints->push_back(
                "Text contains emoji, but the device fonts have no emoji glyphs — they render as "
                "missing-glyph boxes; drop them or use icon nodes / button icon (Lucide names) "
                "instead.");
        }
    }

    if (std::strcmp(type, "label") == 0) {
        const char* text = GetStr(leaf, "text");
        const bool has_bind = HasKey(leaf, "bind") || HasKey(leaf, "bind_data");
        if ((text == nullptr || text[0] == '\0') && !has_bind) {
            st.hints->push_back(
                "This label has no text and no bind/bind_data; it renders empty.");
        } else {
            st.has_label = true;
        }
        // mono/value 走数字等宽字体，无 CJK 字形——fmt 里夹中文单位（"%d分钟"）上屏就是
        // 豆腐块（Haiku 实测踩中）。单位放行首的文字 label 里才安全。
        const char* fmt = GetStr(leaf, "fmt");
        const cJSON* mono = GetItem(leaf, "mono");
        const char* role = GetStr(leaf, "role");
        if (fmt != nullptr && (cJSON_IsTrue(mono) || (role && std::strcmp(role, "value") == 0))) {
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(fmt); *p; ++p) {
                if (*p >= 0x80) {
                    st.hints->push_back(std::string("fmt '") + fmt +
                                        "' has non-ASCII text but mono/value cells use the number "
                                        "font (no CJK glyphs — renders as boxes); keep fmt ASCII "
                                        "and put the unit in the row's text label.");
                    break;
                }
            }
        }
    }

    if (std::strcmp(type, "chart") == 0) {
        const char* path = GetStr(leaf, "bind_history");
        if (!path || !DataHub::Instance().HasHistory(path)) {
            st.hints->push_back(std::string("This chart's bind_history '") + (path ? path : "") +
                                "' isn't a history-enabled path; it will render empty.");
        }
        if (HasKey(leaf, "points")) {
            int p = GetInt(leaf, "points", 60);
            if (p < 8 || p > 120) {
                st.hints->push_back("chart points is clamped to [8,120]; the declared value is out "
                                    "of range and will be silently adjusted.");
            }
        }
    }

    if (std::strcmp(type, "button") == 0) {
        const char* variant = GetStr(leaf, "variant");
        if (variant && std::strcmp(variant, "primary") == 0) ++st.primary_count;
        const char* text = GetStr(leaf, "text");
        if ((text == nullptr || text[0] == '\0') && GetStr(leaf, "icon") == nullptr) {
            st.hints->push_back("This button has neither text nor icon and renders as a blank "
                                "pill; give it a short text and/or an icon (Lucide name).");
        }
        if (!HasKey(leaf, "on_click")) {
            st.hints->push_back(
                "This button has no on_click and does nothing when tapped — a dead control.");
        }
    }

    {
        // 未知图标名回落成圆点——当场纠正 LLM 生造的名字（button 的 icon 属性同样适用）。
        const char* icon = GetStr(leaf, "icon");
        if (icon == nullptr && std::strcmp(type, "icon") == 0) icon = GetStr(leaf, "name");
        if (icon != nullptr && !IconKnown(icon)) {
            st.hints->push_back(std::string("Icon '") + icon +
                                "' is not in the built-in Lucide subset and renders as a plain "
                                "dot; use a more common Lucide icon name.");
        }
    }

    // 死控件：slider/switch/arc 有交互属性却 on_click/on_change/on_release 全缺——用户碰它
    // 什么都不会发生（F3 规则清单）。choice 走独立的「回流出口」检查，见下。
    if (std::strcmp(type, "slider") == 0 || std::strcmp(type, "switch") == 0 ||
        std::strcmp(type, "arc") == 0) {
        if (ActionsHaveReport(GetItem(leaf, "on_change"))) {
            st.hints->push_back(std::string("The on_change of this ") + type +
                                " reports on every change and costs an LLM round-trip each time; "
                                "use on_release or a local patch/set/toggle instead.");
        }
        // 与渲染器 FinishLeafWidget 的 live 判定同口径：bind（可写路径 AttachWriteback 直控
        // 硬件；只读路径是刻意的置灰仪表）或有 id（值随 report 自动上送）都不是死控件。
        // 系统提示词的标准音量卡就是纯 bind 无 handler——之前不认 bind，每张标准控制卡都被
        // 误报 dead control，弱模型会画蛇添足加 on_change 或整卡重渲。
        const bool has_handler = HasKey(leaf, "on_click") || HasKey(leaf, "on_change") ||
                                 HasKey(leaf, "on_release") || HasKey(leaf, "bind") ||
                                 GetStr(leaf, "id") != nullptr;
        if (!has_handler) {
            st.hints->push_back(std::string("This ") + type +
                                " has no bind and no on_click/on_change/on_release — a dead control "
                                "that does nothing when the user interacts with it.");
        }
    }

    if (std::strcmp(type, "choice") == 0) {
        // choice 没有 on_release——别给弱模型指一条不存在的路（旧文案 "use on_release" 的坑）。
        // 一次性选单用 report 是正路，只提醒常驻选择器的往返成本。
        if (ActionsHaveReport(GetItem(leaf, "on_change"))) {
            st.hints->push_back(
                "This choice reports on every tap (one LLM round-trip each) — fine for a one-shot "
                "pick; for a persistent selector prefer a local set/patch instead.");
        }
        const bool has_id = GetStr(leaf, "id") != nullptr;
        const bool has_outlet = ActionsHaveOutlet(GetItem(leaf, "on_change"));
        if (!has_outlet && !has_id) {
            st.hints->push_back(
                "This choice's on_change has no report/set/patch action and no id, so the "
                "selection has no way back to the app or the LLM.");
        }
    }
}

// 一个 grid 块（cells/rows/bind_rows 三形态之一）的叶子遍历（CARD_V2.md §1.2）。
void LintGrid(const cJSON* grid, const cJSON* data, LintState& st) {
    if (!cJSON_IsObject(grid)) return;
    ++st.grid_count;
    const cJSON* cells = GetItem(grid, "cells");
    const cJSON* rows = GetItem(grid, "rows");
    if (cJSON_IsArray(cells)) {
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cells) LintLeaf(c, data, st);
        return;
    }
    if (cJSON_IsArray(rows)) {
        const cJSON* r = nullptr;
        cJSON_ArrayForEach(r, rows) {
            if (!cJSON_IsArray(r)) continue;
            const cJSON* c = nullptr;
            cJSON_ArrayForEach(c, r) LintLeaf(c, data, st);
        }
        return;
    }
    // bind_rows：行数据来自 data，节点账本按 eff_max×模板 cell 数预留（与 Validate 同口径），
    // 不逐行重复同一条 hint——只对模板本身跑一遍 leaf 检查（LintLeaf 会 ++node_count，这里
    // 先扣掉那次自增再按预留数补回，避免模板 cell 被数两次）。
    const char* key = GetStr(grid, "bind_rows");
    const cJSON* item = GetItem(grid, "item");
    const cJSON* arr = (data && key) ? GetItem(data, key) : nullptr;
    const int len = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    const int eff = EffMax(grid, len);
    int tcount = 0;
    if (cJSON_IsArray(item)) {
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, item) {
            LintLeaf(c, data, st);
            --st.node_count;
            ++tcount;
        }
    } else if (item) {
        LintLeaf(item, data, st);
        --st.node_count;
        tcount = 1;
    }
    st.node_count += eff * tcount;
}
}  // namespace

// Lint v2（CARD_V2.md §8 步骤5）：遍历 root（grid 块数组）+ 三种形态展开到叶子，非阻断建议。
// 检查项：primary 按钮 >1；空 label（无 text 也无 bind/bind_data）；死控件（button/slider/
// switch/arc 交互属性 on_click/on_change/on_release 全缺）；choice 无回流出口（on_change 里
// 没有 report/set/patch 且无 id）；逼近节点上限（>=64 的 90%=58）/ grid 上限（>=8 的 90%=7）。
// 沿用的 v1 检查语义（emoji/未知 icon/invoke confirm 级/chart bind_history 有效性/on_change
// 挂 report 的往返代价）不依赖树形状，逐叶子判断即可，一并保留在 LintLeaf 里。
std::vector<std::string> Lint(const cJSON* root_node, const cJSON* data) {
    std::vector<std::string> hints;
    LintState st;
    st.hints = &hints;
    if (cJSON_IsArray(root_node)) {
        const cJSON* grid = nullptr;
        cJSON_ArrayForEach(grid, root_node) LintGrid(grid, data, st);
    } else {
        LintGrid(root_node, data, st);  // 兼容 Repair 前的单 grid 对象（§6.2 自动修规则）
    }

    if (st.primary_count > 1) {
        hints.push_back("Card has " + std::to_string(st.primary_count) +
                        " primary buttons; keep exactly one amber call-to-action and make the "
                        "rest ghost/plain/default.");
    }
    if (!st.has_label) {
        hints.push_back("Card has no text label; add a title/label so the user can tell what it is.");
    }
    if (st.node_count >= 58) {
        hints.push_back("Card uses " + std::to_string(st.node_count) +
                        "/64 nodes; near the limit — split it or simplify.");
    }
    if (st.grid_count >= 7) {
        hints.push_back("Card uses " + std::to_string(st.grid_count) +
                        "/8 grid blocks; near the limit — merge or drop some.");
    }
    return hints;
}

}  // namespace pi_card
