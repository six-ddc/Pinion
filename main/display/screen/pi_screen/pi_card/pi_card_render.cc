#include "pi_card_render.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "esp_log.h"

#include "pi_card_actions.h"
#include "pi_card_cmd.h"  // CommandRegistry（invoke 的 Lint 提示）
#include "pi_card_data.h"
#include "pi_card_icons.h"
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
// 容器/控件底色：fill(令牌) > bg(hex)。无则不动（透明或默认样式）。
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
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 15, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    const lv_style_selector_t pressed = LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    Tok text_tone = Tok::Tx;
    if (!std::strcmp(variant, "primary")) {
        pi_theme::ApplyBg(btn, Tok::Accent);
        pi_theme::ApplyBg(btn, Tok::AccentDim, pressed);
        text_tone = Tok::Bg;  // 深字压在琥珀上（深/浅主题都够对比）
    } else if (!std::strcmp(variant, "ghost")) {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        pi_theme::ApplyBorder(btn, Tok::Line);
        pi_theme::ApplyBg(btn, Tok::Card2, pressed);
    } else if (!std::strcmp(variant, "plain")) {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
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

bool IsGrowable(const char* type) {
    return std::strcmp(type, "button") == 0 || std::strcmp(type, "slider") == 0 ||
           std::strcmp(type, "bar") == 0 || std::strcmp(type, "spacer") == 0 ||
           std::strcmp(type, "arc") == 0 || std::strcmp(type, "choice") == 0 ||
           std::strcmp(type, "chart") == 0;
}
bool IsInteractive(const char* type) {
    return std::strcmp(type, "button") == 0 || std::strcmp(type, "slider") == 0 ||
           std::strcmp(type, "switch") == 0 || std::strcmp(type, "arc") == 0 ||
           std::strcmp(type, "choice") == 0;
}

// ------------------------------ 自适应尺寸 ---------------------------------
// parent_flow: 0=column（含 root）, 1=row。growable/label 在 column 里默认全宽、
// 在 row 里默认按比例分配（flex-grow 1），使「一排按钮均分」「一列控件铺满」这
// 类最简 JSON 也有好布局。显式 w/grow 永远优先。
enum { FLOW_COL = 0, FLOW_ROW = 1 };
void ApplySizing(lv_obj_t* obj, const char* type, const cJSON* node, int parent_flow) {
    const bool spacer = std::strcmp(type, "spacer") == 0;
    if (HasKey(node, "w")) {
        lv_obj_set_width(obj, GetInt(node, "w", 0));
    } else if (HasKey(node, "grow")) {
        lv_obj_set_flex_grow(obj, static_cast<uint8_t>(GetInt(node, "grow", 0)));
    } else if (spacer) {
        // 行内 spacer = 横向弹性填充（把两侧顶开）；列内 spacer 绝不 grow —— 否则会
        // 吃掉竖向空间把卡片撑高。列内当固定小间隔（见下方默认高）。
        if (parent_flow == FLOW_ROW) lv_obj_set_flex_grow(obj, 1);
    } else if (std::strcmp(type, "divider") == 0) {
        lv_obj_set_width(obj, LV_PCT(100));
    } else if (parent_flow == FLOW_ROW) {
        if (IsGrowable(type)) lv_obj_set_flex_grow(obj, 1);
        // row 里的 label/icon/switch 保持自然宽，多件并排更自然
    } else {  // column
        if (IsGrowable(type) || std::strcmp(type, "label") == 0) lv_obj_set_width(obj, LV_PCT(100));
    }
    if (HasKey(node, "h")) lv_obj_set_height(obj, GetInt(node, "h", 0));
    else if (spacer && parent_flow != FLOW_ROW) lv_obj_set_height(obj, 8);
}

// ------------------------------ 默认精致样式 -------------------------------
// 呼应 pi_quick_panel / CreateToolCard：轨道 Card2、强调 Accent、卡面 Card。
void ApplyDefaultStyle(lv_obj_t* obj, const char* type, int depth) {
    if (std::strcmp(type, "slider") == 0) {
        lv_obj_set_height(obj, 6);
        pi_theme::ApplyBg(obj, Tok::Line);  // 底轨（整盒宽、可见）—— 把手边缘贴齐它的端点
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        pi_theme::ApplyBg(obj, Tok::Accent, LV_PART_INDICATOR);
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        // 把手用中性 Tx（而非又一块琥珀）+ 细描边 + 微投影：强调色只留给填充轨，
        // 把手更精致、层次更清。
        pi_theme::ApplyBg(obj, Tok::Tx, LV_PART_KNOB);
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_pad_all(obj, 10, LV_PART_KNOB);  // ~26px 把手
        lv_obj_set_style_shadow_width(obj, 8, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(obj, lv_color_hex(0x000000), LV_PART_KNOB);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_30, LV_PART_KNOB);
        lv_obj_set_ext_click_area(obj, 20);
        // 把手超出轨道两端各约 13px（26px 直径）。给 MAIN 加水平 padding 把轨道向内缩，
        // 使 0%/100% 端点的把手停在滑块自己的盒子内、不外伸吃穿行 gap 顶到相邻图标/label。
        // padding 是盒内内缩，不改变 flex 占位——故 label 不会被挤出裁切（margin 会）。
        lv_obj_set_style_pad_hor(obj, 13, LV_PART_MAIN);
    } else if (std::strcmp(type, "bar") == 0) {
        lv_obj_set_height(obj, 6);
        pi_theme::ApplyBg(obj, Tok::Line);  // 底轨可见，与 slider 一致
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        pi_theme::ApplyBg(obj, Tok::Accent, LV_PART_INDICATOR);
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        // 与 slider 同款水平内缩，让同列的 slider 轨道与 bar 轨道左右端对齐（bar 无把手
        // 本不需内缩，但为跨行视觉对齐须一致）。
        lv_obj_set_style_pad_hor(obj, 13, LV_PART_MAIN);
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
    if (depth == 0 && (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0)) {
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
        lv_obj_set_style_radius(seg, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(seg, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(seg, 0, LV_PART_MAIN);
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
        lv_label_bind_text(obj, subj, fmt);
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
    return false;
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

lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow,
                     bool in_list_row) {
    if (!cJSON_IsObject(node)) {
        err = "node is not an object";
        return nullptr;
    }
    if (depth > limits.max_depth) {
        err = "nesting too deep (max " + std::to_string(limits.max_depth) + ")";
        return nullptr;
    }
    if (++node_count > limits.max_nodes) {
        err = "too many nodes (max " + std::to_string(limits.max_nodes) + ")";
        return nullptr;
    }
    const char* type = GetStr(node, "type");
    if (!type) {
        err = "node missing type";
        return nullptr;
    }

    lv_obj_t* obj = nullptr;
    int this_flow = FLOW_COL;

    if (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0) {
        const bool is_row = std::strcmp(type, "row") == 0;
        this_flow = is_row ? FLOW_ROW : FLOW_COL;
        obj = lv_obj_create(parent);
        screen_strip_obj_chrome(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_width(obj, HasKey(node, "w") ? GetInt(node, "w", 0) : LV_PCT(100));
        lv_obj_set_height(obj, HasKey(node, "h") ? GetInt(node, "h", 0) : LV_SIZE_CONTENT);
        // row 支持自动换行，超宽不裁剪 —— 自适应关键。
        lv_obj_set_flex_flow(obj, is_row ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
        int gap = GetInt(node, "gap", is_row ? 12 : 12);
        lv_obj_set_style_pad_row(obj, gap, LV_PART_MAIN);
        lv_obj_set_style_pad_column(obj, gap, LV_PART_MAIN);
        if (is_row) {
            lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
        }
    } else if (std::strcmp(type, "label") == 0) {
        obj = lv_label_create(parent);
        if (const char* txt = GetStr(node, "text")) lv_label_set_text(obj, txt);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
        ApplyLabelStyle(obj, node);  // role/size 排版 + 颜色
    } else if (std::strcmp(type, "icon") == 0) {
        Tok tok = Tok::Dim;  // 图标默认次要（Dim）；重点图标可 tone:tx/accent
        ToneTok(GetStr(node, "tone", "dim"), tok);
        obj = MakeIcon(parent, GetStr(node, "icon", GetStr(node, "name", "dot")),
                       GetInt(node, "size", 22), tok);
    } else if (std::strcmp(type, "button") == 0) {
        obj = lv_button_create(parent);
        lv_obj_t* lbl = lv_label_create(obj);
        const char* text = GetStr(node, "text", "");
        lv_label_set_text(lbl, text);  // 无文字置空，避免 LVGL "Text" 占位
        Tok fg = ApplyButtonStyle(obj, lbl, node);  // 变体 + 字体 + 颜色
        if (const char* icon_name = GetStr(node, "icon")) {
            // 图标按钮：icon 可单用或与 text 并存（图标居左），与文字同前景令牌。
            lv_obj_t* ic = MakeIcon(obj, icon_name, GetInt(node, "size", 20), fg);
            lv_obj_move_to_index(ic, 0);
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(obj, 8, LV_PART_MAIN);
            // 仅图标时藏掉空 label，免得 pad_column 让图标偏心。
            if (text[0] == '\0') lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_center(lbl);
        }
    } else if (std::strcmp(type, "slider") == 0) {
        obj = lv_slider_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;  // 退化区间兜底
        lv_slider_set_range(obj, mn, mx);
        if (HasKey(node, "value")) lv_slider_set_value(obj, GetInt(node, "value", 0), LV_ANIM_OFF);
    } else if (std::strcmp(type, "arc") == 0) {
        obj = lv_arc_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;  // 退化区间兜底
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
        // 固定"纸面"配色：深浅主题都深码浅底，保证可扫描、不随主题反色（深主题下是一张亮色
        // 小卡，刻意取舍：可扫描优先）。
        lv_qrcode_set_dark_color(obj, pi_theme::PaletteOf(true).tx);
        lv_qrcode_set_light_color(obj, pi_theme::PaletteOf(true).bg);
        const char* txt = GetStr(node, "text", "");
        lv_qrcode_update(obj, txt, static_cast<uint32_t>(std::strlen(txt)));
    } else if (std::strcmp(type, "choice") == 0) {
        obj = MakeChoice(parent, node);
    } else if (std::strcmp(type, "chart") == 0) {
        // 只读历史折线：不走 ApplyBind（无 lv_subject），直接种子 HistorySnapshot + 订阅
        // AddHistorySink 增量追加；LV_EVENT_DELETE 时摘除 sink（D17，无悬垂回调）。
        obj = lv_chart_create(parent);
        lv_chart_set_type(obj, LV_CHART_TYPE_LINE);
        lv_chart_set_update_mode(obj, LV_CHART_UPDATE_MODE_SHIFT);
        int points = GetInt(node, "points", 60);
        points = std::min(120, std::max(8, points));
        lv_chart_set_point_count(obj, static_cast<uint16_t>(points));
        lv_chart_set_div_line_count(obj, 3, 0);  // 少量水平网格线；不设纵向/轴刻度=无轴无图例
        pi_theme::ApplyBg(obj, Tok::Card2);
        lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
        lv_obj_set_style_line_color(obj, pi_theme::Color(Tok::Line2), LV_PART_MAIN);
        lv_obj_set_height(obj, GetInt(node, "h", 120));  // 默认 120px（ApplySizing 若给了显式 h 会再套一遍同值）
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
        // 行情卡叶子控件（pi_card_stock）：symbol 声明式，行情数据设备侧直取直画、
        // 自适应刷新、分段按钮切周期、按住图面看数值；生命周期（注册表/canvas
        // 缓冲/timer）全部自管。
        obj = pi_card_stock::Create(parent, node);
        if (obj == nullptr) {
            err = "stock_chart alloc failed";
            return nullptr;
        }
    } else if (std::strcmp(type, "list") == 0) {
        // data 驱动的行模板重复器：item 是一个节点模板，按 card->data[bind_data] 数组的每个
        // 元素克隆+替换{i}/{n}/{item.*}后各渲一份。行数计入 64 节点预算（Validate 已按
        // eff_max×模板节点数预留，故这里怎么渲都不会超预算）。
        obj = lv_obj_create(parent);
        screen_strip_obj_chrome(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
        // 与 column/row 分支一致：显式给 SIZE_CONTENT，否则落回 lv_obj_create 的默认固定高度，
        // 撑不满全部行、把最后几行裁在盒子外（bug 曾在此复现：obj 高度停在第 4 行，第 5 行
        // 虽已正确布局到 y460-484 却被父容器裁掉，因为父容器自身高度从没被设过）。
        lv_obj_set_width(obj, HasKey(node, "w") ? GetInt(node, "w", 0) : LV_PCT(100));
        lv_obj_set_height(obj, HasKey(node, "h") ? GetInt(node, "h", 0) : LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(obj, GetInt(node, "gap", 10), LV_PART_MAIN);
        const cJSON* item = GetItem(node, "item");
        const cJSON* arr = card->data ? GetItem(card->data, GetStr(node, "bind_data", "")) : nullptr;
        int len = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        int eff = EffMax(node, len);
        int rows = std::min(len, eff);
        for (int i = 0; i < rows; i++) {
            cJSON* clone = cJSON_Duplicate(item, 1);
            SubstRecord(clone, cJSON_GetArrayItem(arr, i), i);
            bool ok = RenderNode(obj, clone, card, limits, depth + 1, node_count, err, FLOW_COL, true) !=
                      nullptr;
            cJSON_Delete(clone);
            if (!ok) return nullptr;  // 失败向上冒泡，host 删 root 整卡回滚
        }
        const char* empty_txt = GetStr(node, "empty");
        if (rows == 0 && empty_txt) {
            lv_obj_t* lbl = lv_label_create(obj);
            lv_label_set_text(lbl, empty_txt);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            const lv_font_t* f = &font_pi_mono_14;
            SafeFont(f, empty_txt);  // 中文兜底文案回退 puhui，不出豆腐块
            lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
            pi_theme::ApplyText(lbl, Tok::Faint);
            ++node_count;
        }
        // 模板 clone 一份存进 card 的 json_pool（card 存活期常驻），供 ui_update 数据变更时
        // RefreshDataConsumers 全量重渲这个 list 子树用；item 本身随本轮渲染完的 cJSON_Delete
        // 一起释放，不能直接借它的指针。
        // 双保险：嵌套 list 已在 ValidateNode 拒绝，这里 !in_list_row 是渲染器侧的防御性
        // 守卫——万一校验被绕过，外层 list 重渲时 lv_obj_clean 会先删掉这个 obj，若仍注册了
        // consumer/json_pool 就是悬垂指针 + 每次重渲无界增长；in_list_row 下干脆不登记。
        if (!in_list_row) {
            cJSON* tpl = cJSON_Duplicate(item, 1);
            card->json_pool.push_back(tpl);
            UiCard::DataConsumer dc;
            dc.obj = obj;
            dc.kind = UiCard::DataConsumer::List;
            dc.key = GetStr(node, "bind_data", "");
            dc.item_tpl = tpl;
            dc.empty_text = empty_txt ? empty_txt : "";
            dc.eff_max = eff;
            dc.depth = depth;
            dc.limits = limits;
            card->consumers.push_back(std::move(dc));
        }
    } else if (std::strcmp(type, "divider") == 0) {
        obj = lv_obj_create(parent);
        screen_strip_obj_chrome(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(obj, 1);
        pi_theme::ApplyBg(obj, Tok::Line);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    } else if (std::strcmp(type, "spacer") == 0) {
        obj = lv_obj_create(parent);
        screen_strip_obj_chrome(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_size(obj, 0, 0);  // 本体不占尺寸；由 ApplySizing 给弹性/间隔
    } else {
        err = std::string("unknown type: ") + type;
        return nullptr;
    }

    // OOM 兜底：当前 LVGL 配置下 lv_*_create 分配失败会先 while(1) 卡死、极少真返回
    // null，此为廉价的前向防护——失败经既有通路向上冒泡 → host 删 root 整卡回滚。
    if (!obj) {
        err = std::string("widget create failed: ") + type;
        return nullptr;
    }

    ApplyDefaultStyle(obj, type, depth);
    // label/button/icon 的字体与颜色已在各自分支处理。
    ApplyCommonProps(obj, type, node, card, in_list_row);
    ApplySizing(obj, type, node, parent_flow);  // 自适应尺寸：按父容器主轴定默认

    // bind（放在 value/checked 之后覆盖静态初值）
    if (const char* path = GetStr(node, "bind")) ApplyBind(obj, type, path, node, card);

    // data-label：label 若声明 bind_data 且没有 bind（bind 优先，二者都给以 bind 为准），
    // 从 card->data 取值直显/套模板；登记 DataConsumer 供 ui_update 的 data 变更定向刷新。
    if (std::strcmp(type, "label") == 0) {
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

    // 事件
    AttachEvent(obj, LV_EVENT_CLICKED, card, GetItem(node, "on_click"));
    AttachEvent(obj, LV_EVENT_VALUE_CHANGED, card, GetItem(node, "on_change"));
    AttachEvent(obj, LV_EVENT_RELEASED, card, GetItem(node, "on_release"));

    // 死控件兜底：switch/slider 若拨/拖了不会有任何效果——做成只读展示（去交互 + 视觉降级），
    // 杜绝「看着能设其实是摆设」的假开关/假滑条（如演示卡里那个装饰性网络开关）。
    //
    // 「有效果」共四种，缺一不可：
    //   1. 绑到可写路径 → 直接控硬件
    //   2/3. 挂了 on_change / on_release → 有动作
    //   4. 没 bind 但有 id → **纯本地表单控件**：它的值会被下一次 report 自动带回 LLM
    //      （见 pi_card_actions.cc 的 CollectState）。拨它当场没动静，但提交时算数——这正是
    //      「选数量 + 加急开关 + 确认按钮」这类表单的用法，绝不能 DIM 掉。
    // 注意第 4 条特意要求「没 bind」：绑到**只读**路径的控件（如 bind battery.level）即便给了
    // id（多半是留给 ui_update 用的）也仍是纯展示，拖它写不回任何地方 → 该 DIM 还得 DIM。
    if (std::strcmp(type, "switch") == 0 || std::strcmp(type, "slider") == 0 ||
        std::strcmp(type, "arc") == 0) {
        const char* bind = GetStr(node, "bind");
        const bool live = (bind && DataHub::Instance().Writable(bind)) ||
                          GetItem(node, "on_change") != nullptr ||
                          GetItem(node, "on_release") != nullptr ||
                          (!bind && GetStr(node, "id") != nullptr);
        if (!live) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);       // 不可交互
            lv_obj_set_style_opa(obj, LV_OPA_60, LV_PART_MAIN);   // 视觉降级 = 只读态
        }
    }

    // 递归子节点（把本容器的 flow 传给子级做自适应尺寸）
    if (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0) {
        const cJSON* children = GetItem(node, "children");
        if (children && cJSON_IsArray(children)) {
            const cJSON* child = nullptr;
            cJSON_ArrayForEach(child, children) {
                if (!RenderNode(obj, child, card, limits, depth + 1, node_count, err, this_flow,
                               in_list_row))
                    return nullptr;  // 失败向上冒泡，host 删 root 整卡回滚
            }
        }
    }
    return obj;
}

bool ApplyProps(lv_obj_t* obj, const cJSON* props, std::string& err) {
    if (!cJSON_IsObject(props)) {
        err = "props is not an object";
        return false;
    }
    if (const char* txt = GetStr(props, "text")) lv_label_set_text(obj, txt);
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

// ------------------------------- 干跑校验 ----------------------------------
namespace {
// 先扫一遍收集全树已声明的 id，供 toggle/show/hide 的 target 校验。递归口径必须与渲染器
// 一致（只有 column/row 的 children 会被渲染），否则会放行一个运行时根本找不到的 target。
// list 的 item 模板挂在 "item" 字段而非 "children"，本函数天然不递归进去——行内 id 不进
// node_ids（与渲染期不进 card->nodes 呼应），toggle/show/hide/patch 校验期已整体拒绝行内用。
void CollectNodeIds(const cJSON* node, std::set<std::string>& ids) {
    if (!cJSON_IsObject(node)) return;
    if (const char* id = GetStr(node, "id")) ids.insert(id);
    const char* type = GetStr(node, "type");
    if (!type) return;
    if (std::strcmp(type, "column") != 0 && std::strcmp(type, "row") != 0) return;
    const cJSON* children = GetItem(node, "children");
    if (!children || !cJSON_IsArray(children)) return;
    const cJSON* child = nullptr;
    cJSON_ArrayForEach(child, children) CollectNodeIds(child, ids);
}

bool ValidateNode(const cJSON* node, const RenderLimits& limits, int depth, int& count,
                  const std::set<std::string>& node_ids, const cJSON* data, std::string& err,
                  bool in_list_row = false) {
    if (!cJSON_IsObject(node)) {
        err = "node is not an object";
        return false;
    }
    if (depth > limits.max_depth) {
        err = "nesting too deep (max " + std::to_string(limits.max_depth) + ")";
        return false;
    }
    if (++count > limits.max_nodes) {
        err = "too many nodes (max " + std::to_string(limits.max_nodes) + ")";
        return false;
    }
    const char* type = GetStr(node, "type");
    if (!type) {
        err = "node missing type";
        return false;
    }
    static const char* kTypes[] = {"column", "row",   "label", "button", "slider", "arc",
                                   "switch", "bar",   "icon",  "divider", "spacer", "qrcode",
                                   "choice", "list",  "chart", "stock_chart"};
    bool known = false;
    for (auto* t : kTypes)
        if (std::strcmp(t, type) == 0) known = true;
    if (!known) {
        err = std::string("unknown type: ") + type;
        return false;
    }
    if (std::strcmp(type, "list") == 0) {
        // 嵌套 list 拒绝：外层 list 重渲时会 lv_obj_clean 掉整个行子树，内层 list 注册的
        // DataConsumer/json_pool 模板会悬垂且每次外层重渲都会新增一份，永久增长。与 D6
        // "行模板受限"同一口径——直接在校验期堵死，不留渲染期兜底的坑。
        if (in_list_row) {
            err = "a list can't nest inside another list's item template; flatten the data or use one list";
            return false;
        }
        const cJSON* item = GetItem(node, "item");
        if (!cJSON_IsObject(item)) {
            err = "list needs an 'item' node template";
            return false;
        }
        const char* bind_data = GetStr(node, "bind_data");
        if (!bind_data) {
            err = "list needs a 'bind_data' data key";
            return false;
        }
        const cJSON* arr = data ? GetItem(data, bind_data) : nullptr;
        int len = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        int eff = EffMax(node, len);
        int tcount = 0;
        if (!ValidateNode(item, limits, depth + 1, tcount, node_ids, data, err, /*in_list_row=*/true)) {
            return false;
        }
        count += eff * tcount;  // 预留：validate 按 eff_max×模板节点数记账，≥render 实际用量
        if (count > limits.max_nodes) {
            err = "list reserves " + std::to_string(eff * tcount) + " nodes (max×rowNodes); over " +
                  std::to_string(limits.max_nodes) + " — lower max or simplify the row";
            return false;
        }
        if (GetStr(node, "empty")) ++count;
        return true;  // list 不再走通用 children 递归（其"children"本就不存在）
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
            err = std::string("unknown bind path: ") + path;
            // 前缀对但格式错（如 stock.茅台.price）：附 provider 的用法提示教 LLM 改对。
            if (const char* h = DataHub::Instance().HintFor(path)) err += std::string("; ") + h;
            return false;
        }
        // label 的 fmt 必须与绑定值类型相容，否则渲染时 newlib vsnprintf 会解引用坏指针崩溃
        // （见 FmtSafeForType）。首道也是主道防线：同步把错误回给 LLM 重试。
        if (std::strcmp(type, "label") == 0) {
            if (const char* fmt = GetStr(node, "fmt")) {
                HubType t = HubType::Int;
                DataHub::Instance().TypeOf(path, t);
                if (!FmtSafeForType(fmt, t, err)) return false;
            }
        }
        // 数值型控件（slider/arc/bar/switch/choice）只能绑 Int/Bool 路径：它们的 bind 落地在
        // lv_subject 的 int union 上（lv_*_bind_value / 手动 observer 都读 int），绑到 String
        // 路径不会崩，但读到的是 union 里的垃圾值，显示无意义。校验器比渲染器更严是允许的
        // （同构约束只要求"放行的必可安全渲染"），这里直接同步拒绝，让 LLM 换绑数值路径或
        // 改用带 fmt 的 label 展示字符串。
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
    // 事件动作合法性（in_list_row 时拒绝 toggle/show/hide/patch，见 ValidateActions）
    if (!ValidateActions(GetItem(node, "on_click"), node_ids, err, in_list_row)) return false;
    if (!ValidateActions(GetItem(node, "on_change"), node_ids, err, in_list_row)) return false;
    if (!ValidateActions(GetItem(node, "on_release"), node_ids, err, in_list_row)) return false;
    // 递归子节点
    if (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0) {
        const cJSON* children = GetItem(node, "children");
        if (children && cJSON_IsArray(children)) {
            const cJSON* child = nullptr;
            cJSON_ArrayForEach(child, children)
                if (!ValidateNode(child, limits, depth + 1, count, node_ids, data, err, in_list_row))
                    return false;
        }
    }
    return true;
}
}  // namespace

bool Validate(const cJSON* root_node, const cJSON* data, std::string& err) {
    RenderLimits limits;
    int count = 0;
    std::set<std::string> node_ids;
    CollectNodeIds(root_node, node_ids);  // 两遍：target 可以前向引用树里靠后声明的节点
    return ValidateNode(root_node, limits, 0, count, node_ids, data, err);
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

struct LintState {
    int node_count = 0;
    int max_depth = 0;
    int primary_count = 0;
    bool has_label = false;
    std::vector<std::string>* hints = nullptr;
};

// 粗略数一棵模板子树的节点数（不生成 hint，仅供 list 的 eff×tcount 预估用）。
int CountTemplateNodes(const cJSON* node) {
    if (!cJSON_IsObject(node)) return 0;
    int c = 1;
    const char* type = GetStr(node, "type");
    if (type && (!std::strcmp(type, "column") || !std::strcmp(type, "row"))) {
        const cJSON* children = GetItem(node, "children");
        if (children && cJSON_IsArray(children)) {
            const cJSON* child = nullptr;
            cJSON_ArrayForEach(child, children) c += CountTemplateNodes(child);
        }
    }
    return c;
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

void LintWalk(const cJSON* node, int depth, const cJSON* data, LintState& st) {
    if (!cJSON_IsObject(node)) return;
    ++st.node_count;
    if (depth > st.max_depth) st.max_depth = depth;
    const char* type = GetStr(node, "type");
    if (!type) return;
    LintInvokeConfirm(GetItem(node, "on_click"), st.hints);
    LintInvokeConfirm(GetItem(node, "on_change"), st.hints);
    LintInvokeConfirm(GetItem(node, "on_release"), st.hints);
    if (std::strcmp(type, "list") == 0) {
        // list 无 "children"，其行数按 eff_max×模板节点数逼近 render 实际用量，累进
        // node_count 让 56/64 上限提示对 list 卡也生效；不深入生成行内 hint（避免同一条
        // 提示因行数被放大 N 倍地重复）。
        const cJSON* arr = data ? GetItem(data, GetStr(node, "bind_data", "")) : nullptr;
        int len = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        int eff = EffMax(node, len);
        int tcount = CountTemplateNodes(GetItem(node, "item"));
        st.node_count += eff * tcount;
        return;
    }
    if (std::strcmp(type, "label") == 0) st.has_label = true;
    if (std::strcmp(type, "chart") == 0) {
        const char* path = GetStr(node, "bind_history");
        if (!path || !DataHub::Instance().HasHistory(path)) {
            st.hints->push_back(std::string("This chart's bind_history '") + (path ? path : "") +
                                "' isn't a history-enabled path; it will render empty.");
        }
        if (HasKey(node, "points")) {
            int p = GetInt(node, "points", 60);
            if (p < 8 || p > 120) {
                st.hints->push_back("chart points is clamped to [8,120]; the declared value is out "
                                    "of range and will be silently adjusted.");
            }
        }
    }
    if (std::strcmp(type, "button") == 0) {
        const char* variant = GetStr(node, "variant");
        if (variant && std::strcmp(variant, "primary") == 0) ++st.primary_count;
        const char* text = GetStr(node, "text");
        if ((text == nullptr || text[0] == '\0') && GetStr(node, "icon") == nullptr) {
            st.hints->push_back("This button has neither text nor icon and renders as a blank "
                                "pill; give it a short text and/or an icon (Lucide name).");
        }
    }
    {
        // 未知图标名回落成圆点——当场纠正 LLM 生造的名字（button 的 icon 属性同样适用）。
        const char* icon = GetStr(node, "icon");
        if (icon == nullptr && std::strcmp(type, "icon") == 0) icon = GetStr(node, "name");
        if (icon != nullptr && !IconKnown(icon)) {
            st.hints->push_back(std::string("Icon '") + icon +
                                "' is not in the built-in Lucide subset and renders as a plain "
                                "dot; use a more common Lucide icon name.");
        }
    }
    const bool ctrl = std::strcmp(type, "slider") == 0 || std::strcmp(type, "switch") == 0 ||
                      std::strcmp(type, "arc") == 0 || std::strcmp(type, "choice") == 0;
    if (ctrl) {
        if (ActionsHaveReport(GetItem(node, "on_change"))) {
            st.hints->push_back(std::string("The on_change of this ") + type +
                                " reports on every change and costs an LLM round-trip each time; "
                                "use on_release or a local patch/set/toggle instead.");
        }
        const char* bind = GetStr(node, "bind");
        const bool writable = bind && DataHub::Instance().Writable(bind);
        const bool has_id = GetStr(node, "id") != nullptr;
        const bool has_handler =
            GetItem(node, "on_change") != nullptr || GetItem(node, "on_release") != nullptr;
        if (std::strcmp(type, "choice") == 0) {
            if (!(has_id || bind || has_handler)) {
                st.hints->push_back(
                    "This choice has no id, bind, or on_change, so the selection goes nowhere; add "
                    "an id (rides back on the next report) or an on_change.");
            }
        } else if (!(writable || has_handler || (!bind && has_id))) {
            st.hints->push_back(std::string("This ") + type +
                                " is inert (no writable bind, id, or handler) and will render "
                                "dimmed/read-only; give it an id to report its value, a writable "
                                "bind, or an on_release.");
        }
    }
    if (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0) {
        const cJSON* children = GetItem(node, "children");
        if (children && cJSON_IsArray(children)) {
            const cJSON* child = nullptr;
            cJSON_ArrayForEach(child, children) LintWalk(child, depth + 1, data, st);
        }
    }
}
}  // namespace

std::vector<std::string> Lint(const cJSON* root_node, const cJSON* data) {
    std::vector<std::string> hints;
    LintState st;
    st.hints = &hints;
    LintWalk(root_node, 0, data, st);

    if (st.primary_count > 1) {
        hints.push_back("Card has " + std::to_string(st.primary_count) +
                        " primary buttons; keep exactly one amber call-to-action and make the "
                        "rest ghost/plain/default.");
    }
    if (!st.has_label) {
        hints.push_back("Card has no text label; add a title/label so the user can tell what it is.");
    }
    if (st.node_count >= 56) {
        hints.push_back("Card uses " + std::to_string(st.node_count) +
                        "/64 nodes; near the limit — split it or simplify.");
    }
    if (st.max_depth >= 7) {
        hints.push_back("Card nests " + std::to_string(st.max_depth) +
                        " levels deep (max 8); flatten some rows/columns.");
    }
    return hints;
}

}  // namespace pi_card
