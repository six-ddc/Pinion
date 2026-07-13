#include "pi_card_render.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_log.h"

#include "pi_card_actions.h"
#include "pi_card_data.h"
#include "pi_card_icons.h"
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
// 稳定性护栏：mono 字体只有 ASCII，一旦文本含中文会渲成豆腐块。此时无论角色如何都
// 回退到 puhui，保证「怎么拼都读得出字」。返回是否发生了回退（供去掉字距）。
bool SafeFont(const lv_font_t*& font, const char* text) {
    if (IsMonoFont(font) && HasCjk(text)) {
        font = &font_puhui_20_4;
        return true;
    }
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
void ApplyButtonStyle(lv_obj_t* btn, lv_obj_t* lbl, const cJSON* node) {
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
}

bool IsGrowable(const char* type) {
    return std::strcmp(type, "button") == 0 || std::strcmp(type, "slider") == 0 ||
           std::strcmp(type, "bar") == 0 || std::strcmp(type, "spacer") == 0;
}
bool IsInteractive(const char* type) {
    return std::strcmp(type, "button") == 0 || std::strcmp(type, "slider") == 0 ||
           std::strcmp(type, "switch") == 0;
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
    if (lv_obj_check_type(w, &lv_slider_class)) {
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
    if (std::strcmp(type, "slider") == 0) {
        lv_obj_add_event_cb(obj, HwReleasedCb, LV_EVENT_RELEASED, wb);  // 终值兜底
    }
    lv_obj_add_event_cb(obj, HwFreeCb, LV_EVENT_DELETE, wb);
}

// bar 无内建 bind，用 observer 手动同步。
void BarObserverCb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* bar = lv_observer_get_target_obj(observer);
    if (bar) lv_bar_set_value(bar, lv_subject_get_int(subject), LV_ANIM_ON);
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
        const char* fmt = GetStr(node, "fmt", t == HubType::String ? nullptr : "%d");
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
    }
}

// ------------------------------ 通用属性 -----------------------------------
void ApplyCommonProps(lv_obj_t* obj, const char* type, const cJSON* node, UiCard* card) {
    if (const char* id = GetStr(node, "id")) card->nodes[id] = obj;
    if (HasKey(node, "pad")) lv_obj_set_style_pad_all(obj, GetInt(node, "pad", 0), LV_PART_MAIN);
    ApplyFill(obj, node);
    if (GetBool(node, "hidden")) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (IsInteractive(type)) screen_swipe_back_ignore(obj, true);
}

}  // namespace

// ---------------------------------------------------------------------------
lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow) {
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
        lv_label_set_text(lbl, GetStr(node, "text", ""));  // 无文字置空，避免 LVGL "Text" 占位
        ApplyButtonStyle(obj, lbl, node);  // 变体 + 字体 + 颜色
        lv_obj_center(lbl);
    } else if (std::strcmp(type, "slider") == 0) {
        obj = lv_slider_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;  // 退化区间兜底
        lv_slider_set_range(obj, mn, mx);
        if (HasKey(node, "value")) lv_slider_set_value(obj, GetInt(node, "value", 0), LV_ANIM_OFF);
    } else if (std::strcmp(type, "bar") == 0) {
        obj = lv_bar_create(parent);
        int mn = GetInt(node, "min", 0), mx = GetInt(node, "max", 100);
        if (mx <= mn) mx = mn + 1;
        lv_bar_set_range(obj, mn, mx);
        if (HasKey(node, "value")) lv_bar_set_value(obj, GetInt(node, "value", 0), LV_ANIM_OFF);
    } else if (std::strcmp(type, "switch") == 0) {
        obj = lv_switch_create(parent);
        if (GetBool(node, "checked")) lv_obj_add_state(obj, LV_STATE_CHECKED);
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

    ApplyDefaultStyle(obj, type, depth);
    // label/button/icon 的字体与颜色已在各自分支处理。
    ApplyCommonProps(obj, type, node, card);
    ApplySizing(obj, type, node, parent_flow);  // 自适应尺寸：按父容器主轴定默认

    // bind（放在 value/checked 之后覆盖静态初值）
    if (const char* path = GetStr(node, "bind")) ApplyBind(obj, type, path, node, card);

    // 事件
    AttachEvent(obj, LV_EVENT_CLICKED, card, GetItem(node, "on_click"));
    AttachEvent(obj, LV_EVENT_VALUE_CHANGED, card, GetItem(node, "on_change"));
    AttachEvent(obj, LV_EVENT_RELEASED, card, GetItem(node, "on_release"));

    // 死控件兜底：switch/slider 若既没绑到「可写」路径、也没挂 on_change/on_release，
    // 拨/拖它不会有任何效果（不控硬件、不回报 LLM）——做成只读展示（去交互 + 视觉降级），
    // 杜绝「看着能设其实是摆设」的假开关/假滑条（如演示卡里那个装饰性网络开关）。绑到
    // 只读路径的控件也落这里 → 纯状态显示，值仍随 observer 实时刷新。
    if (std::strcmp(type, "switch") == 0 || std::strcmp(type, "slider") == 0) {
        const char* bind = GetStr(node, "bind");
        const bool live = (bind && DataHub::Instance().Writable(bind)) ||
                          GetItem(node, "on_change") != nullptr ||
                          GetItem(node, "on_release") != nullptr;
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
                if (!RenderNode(obj, child, card, limits, depth + 1, node_count, err, this_flow))
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

// ------------------------------- 干跑校验 ----------------------------------
namespace {
bool ValidateNode(const cJSON* node, const RenderLimits& limits, int depth, int& count,
                  std::string& err) {
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
    static const char* kTypes[] = {"column", "row",   "label",   "button", "slider",
                                   "switch", "bar",   "icon",    "divider", "spacer"};
    bool known = false;
    for (auto* t : kTypes)
        if (std::strcmp(t, type) == 0) known = true;
    if (!known) {
        err = std::string("unknown type: ") + type;
        return false;
    }
    // bind 路径必须已注册
    if (const char* path = GetStr(node, "bind")) {
        if (!DataHub::Instance().Has(path)) {
            err = std::string("unknown bind path: ") + path;
            return false;
        }
    }
    // 事件动作合法性
    if (!ValidateActions(GetItem(node, "on_click"), err)) return false;
    if (!ValidateActions(GetItem(node, "on_change"), err)) return false;
    if (!ValidateActions(GetItem(node, "on_release"), err)) return false;
    // 递归子节点
    if (std::strcmp(type, "column") == 0 || std::strcmp(type, "row") == 0) {
        const cJSON* children = GetItem(node, "children");
        if (children && cJSON_IsArray(children)) {
            const cJSON* child = nullptr;
            cJSON_ArrayForEach(child, children)
                if (!ValidateNode(child, limits, depth + 1, count, err)) return false;
        }
    }
    return true;
}
}  // namespace

bool Validate(const cJSON* root_node, std::string& err) {
    RenderLimits limits;
    int count = 0;
    return ValidateNode(root_node, limits, 0, count, err);
}

}  // namespace pi_card
