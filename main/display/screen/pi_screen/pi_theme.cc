#include "pi_theme.h"

#include <utility>
#include <vector>

#include "settings.h"

namespace {

constexpr int kTokCount = static_cast<int>(pi_theme::Tok::kCount);

// 令牌顺序与 Tok 枚举一致：Bg Card Card2 Line Line2 Tx Dim Faint Accent AccentDim Ok Err
constexpr uint32_t kDarkHex[kTokCount] = {
    0x0E0C09, 0x16130E, 0x181510, 0x2A251C, 0x3A3226, 0xEDE6D6,
    0x97907E, 0x5F5849, 0xFFAE1F, 0x8A6420, 0x9BC46B, 0xE25B4E,
};
constexpr uint32_t kLightHex[kTokCount] = {
    0xF2EDE2, 0xFBF8F1, 0xEAE5D8, 0xDAD2C0, 0xC6BCA6, 0x2B251B,
    0x6E6552, 0xA79D85, 0xB87400, 0xD9A94F, 0x5E8A2E, 0xC23B2E,
};

// 遮罩：深色 = 纯黑 60%；浅色 = 深墨（tx 同源）45%，压得住又不发灰。
constexpr uint32_t kDarkScrimHex = 0x000000;
constexpr uint32_t kLightScrimHex = 0x2B251B;
constexpr lv_opa_t kDarkScrimOpa = LV_OPA_60;
constexpr lv_opa_t kLightScrimOpa = 115;  // ~45%

bool s_light = false;

// 按"属性 x 令牌"惰性初始化的共享样式表。每只 style 只带一个颜色属性，
// 切换主题时改值 + report_style_change 即可全 UI 生效。
lv_style_t s_bg[kTokCount];
lv_style_t s_text[kTokCount];
lv_style_t s_border[kTokCount];
bool s_bg_init[kTokCount] = {};
bool s_text_init[kTokCount] = {};
bool s_border_init[kTokCount] = {};
lv_style_t s_scrim;
bool s_scrim_init = false;

std::vector<std::pair<int, void (*)()>> s_listeners;
int s_next_listener_id = 0;

const uint32_t* CurHexTable() { return s_light ? kLightHex : kDarkHex; }

lv_color_t TokColor(pi_theme::Tok t) { return lv_color_hex(CurHexTable()[static_cast<int>(t)]); }

void RefreshScrimStyle() {
    lv_style_set_bg_color(&s_scrim, lv_color_hex(s_light ? kLightScrimHex : kDarkScrimHex));
    lv_style_set_bg_opa(&s_scrim, s_light ? kLightScrimOpa : kDarkScrimOpa);
}

// 把全部已建样式刷成当前主题的颜色值。
void RefreshStyles() {
    for (int i = 0; i < kTokCount; i++) {
        lv_color_t c = lv_color_hex(CurHexTable()[i]);
        if (s_bg_init[i])
            lv_style_set_bg_color(&s_bg[i], c);
        if (s_text_init[i])
            lv_style_set_text_color(&s_text[i], c);
        if (s_border_init[i])
            lv_style_set_border_color(&s_border[i], c);
    }
    if (s_scrim_init)
        RefreshScrimStyle();
}

lv_style_t* EnsureStyle(lv_style_t* arr, bool* init, pi_theme::Tok t,
                        void (*setter)(lv_style_t*, lv_color_t)) {
    int i = static_cast<int>(t);
    if (!init[i]) {
        lv_style_init(&arr[i]);
        setter(&arr[i], TokColor(t));
        init[i] = true;
    }
    return &arr[i];
}

void SetBgColor(lv_style_t* s, lv_color_t c) { lv_style_set_bg_color(s, c); }
void SetTextColor(lv_style_t* s, lv_color_t c) { lv_style_set_text_color(s, c); }
void SetBorderColor(lv_style_t* s, lv_color_t c) { lv_style_set_border_color(s, c); }

// 同属性的旧令牌样式先摘干净（含重复挂载），再挂新令牌 —— Apply* 即可做
// 运行期角色切换。
void ApplyOne(lv_obj_t* obj, lv_style_t* arr, bool* init, pi_theme::Tok t,
              void (*setter)(lv_style_t*, lv_color_t), lv_style_selector_t sel) {
    for (int i = 0; i < kTokCount; i++) {
        if (init[i])
            lv_obj_remove_style(obj, &arr[i], sel);
    }
    lv_obj_add_style(obj, EnsureStyle(arr, init, t, setter), sel);
}

}  // namespace

namespace pi_theme {

void Init() {
    Settings ui("ui", false);
    s_light = ui.GetInt("theme", 0) != 0;  // 0=深色 1=浅色（与设置页既有键一致）
    RefreshStyles();                       // 惰性表通常尚未建，兜底重复 Init 也无害
}

const Palette& PaletteOf(bool light) {
    auto build = [](const uint32_t* h) {
        Palette p;
        p.bg = lv_color_hex(h[0]);
        p.card = lv_color_hex(h[1]);
        // h[2] 是派生令牌 Card2，不进 Palette
        p.line = lv_color_hex(h[3]);
        p.line2 = lv_color_hex(h[4]);
        p.tx = lv_color_hex(h[5]);
        p.dim = lv_color_hex(h[6]);
        p.faint = lv_color_hex(h[7]);
        p.accent = lv_color_hex(h[8]);
        p.accent_dim = lv_color_hex(h[9]);
        p.ok = lv_color_hex(h[10]);
        p.err = lv_color_hex(h[11]);
        return p;
    };
    static const Palette kDark = build(kDarkHex);
    static const Palette kLight = build(kLightHex);
    return light ? kLight : kDark;
}

const Palette& Get() { return PaletteOf(s_light); }

bool IsLight() { return s_light; }

void Set(bool light) {
    if (light == s_light)
        return;
    s_light = light;
    {
        Settings ui("ui", true);
        ui.SetInt("theme", light ? 1 : 0);
    }
    RefreshStyles();
    lv_obj_report_style_change(nullptr);  // 全对象样式缓存失效 + 重绘
    for (auto& l : s_listeners)
        l.second();
}

int AddListener(void (*cb)()) {
    int id = s_next_listener_id++;
    s_listeners.emplace_back(id, cb);
    return id;
}

void RemoveListener(int id) {
    for (auto it = s_listeners.begin(); it != s_listeners.end(); ++it) {
        if (it->first == id) {
            s_listeners.erase(it);
            return;
        }
    }
}

lv_color_t Color(Tok t) { return TokColor(t); }

uint32_t Hex(Tok t) { return CurHexTable()[static_cast<int>(t)]; }

void ApplyBg(lv_obj_t* obj, Tok t, lv_style_selector_t sel) {
    ApplyOne(obj, s_bg, s_bg_init, t, SetBgColor, sel);
}

void ApplyText(lv_obj_t* obj, Tok t, lv_style_selector_t sel) {
    ApplyOne(obj, s_text, s_text_init, t, SetTextColor, sel);
}

void ApplyBorder(lv_obj_t* obj, Tok t, lv_style_selector_t sel) {
    ApplyOne(obj, s_border, s_border_init, t, SetBorderColor, sel);
}

void ApplyScrim(lv_obj_t* obj) {
    if (!s_scrim_init) {
        lv_style_init(&s_scrim);
        RefreshScrimStyle();
        s_scrim_init = true;
    }
    lv_obj_add_style(obj, &s_scrim, LV_PART_MAIN);
}

}  // namespace pi_theme
