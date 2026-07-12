#include "pi_screen.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "esp_log.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "IOExpander.hpp"
#include "lv_markdown/md_view.h"
#include "metalio_hal/audio_pipeline.h"
#include "pi_fonts.h"
#include "pi_ui_bridge.h"
#include "screen_util.h"
#include "settings.h"
#include "volc_asr.h"
#include "volc_tts.h"

// ---------------------------------------------------------------------------
// PiScreen -- pi_agent's four-state conversation UI (720x720).
//
// Visual truth source: design spec HTML (colors/sizes/copy/animation
// cadence) + implementation blueprint (architecture). Three persistent view
// containers (idle / listen / chat -- "tool detail" is the chat view with a
// tool card expanded in place, not a fourth view) are built once in
// Create() and toggled via LV_OBJ_FLAG_HIDDEN, never deleted/rebuilt.
//
// Font-subset note: the mono fonts generated for this app (font_pi_mono_14/
// 17) cover only ASCII + the middle-dot/degree signs (blueprint 3c). The
// design copy uses several decorative Unicode glyphs that are NOT in that
// subset (status-bar block, REC bullet, arrows, checkmark, FLOW/ZEN icons,
// peek carets, thinking-row dotted circle). Where the glyph is a standalone
// prefix/suffix, it is drawn as a tiny lv_obj shape (circle/rect/bars) --
// strictly more faithful than any ASCII stand-in. Where it sits inline in
// dynamic copy, it is substituted with the closest ASCII glyph. See the
// substitution table in the work package report.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* TAG = "PiScreen";

// ----- palette (exact hex values from the design spec HTML) ---------------
constexpr uint32_t kBg       = 0x0E0C09;  // .screen background
constexpr uint32_t kCard     = 0x12100C;  // --bg1: tool card / selected chip bg
constexpr uint32_t kCard2    = 0x181510;  // --bg2: ctxbar track / mode-pressed bg
constexpr uint32_t kLine     = 0x2A251C;  // --line: 1px hairlines
constexpr uint32_t kLine2    = 0x3A3226;  // --line2: stopbtn border
constexpr uint32_t kText     = 0xEDE6D6;  // --tx
constexpr uint32_t kDim      = 0x97907E;  // --dim
constexpr uint32_t kFaint    = 0x5F5849;  // --faint
constexpr uint32_t kAmber    = 0xFFAE1F;  // --amber (the one accent)
constexpr uint32_t kAmberDim = 0x8A6420;  // --amber-dim
constexpr uint32_t kOk       = 0x9BC46B;  // --ok
constexpr uint32_t kErr      = 0xE25B4E;  // --err

// ----- layout (720x720 native px, per design spec) -------------------------
constexpr int32_t kW      = 720;
constexpr int32_t kH      = 720;
constexpr int32_t kSbarH  = 56;
constexpr int32_t kHintH  = 112;
constexpr int32_t kDockH  = 112;
constexpr int32_t kMidH   = kH - kSbarH - kHintH;   // 552
constexpr int32_t kFeedH  = kH - kSbarH - kDockH;   // 552

// S1 数据源已从预置假转写（kPresetAsr 走带）换成真 ASR（volc_asr 流式
// delta）——见下方 "real ASR session engine" 一节；四状态视觉与交互结构不变。

constexpr int32_t kSwipeCancelThreshold = 80;  // up-swipe px to cancel PTT
constexpr int32_t kNvsNamespaceMaxTools = 4;   // cached tool cards per turn (ZEN peek)

// ----- per-view state --------------------------------------------------
enum class ViewState { Idle, Listen, Chat };

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_idle_view = nullptr;
lv_obj_t* s_listen_view = nullptr;
lv_obj_t* s_chat_view = nullptr;
lv_obj_t* s_ptt_layer = nullptr;  // persistent touch target, y:[56,720)

ViewState s_state = ViewState::Idle;
ViewState s_return_state = ViewState::Idle;  // where PTT cancel/send returns to
bool s_zen = false;

// idle view widgets
lv_obj_t* s_clock_lbl = nullptr;
lv_obj_t* s_date_lbl = nullptr;
lv_obj_t* s_idle_breath = nullptr;
lv_obj_t* s_idle_ctx_fill = nullptr;
lv_obj_t* s_idle_ctx_lbl = nullptr;
lv_timer_t* s_clock_timer = nullptr;

// listen view widgets
lv_obj_t* s_wave_row = nullptr;
lv_obj_t* s_asr_lbl = nullptr;
lv_obj_t* s_rec_lbl = nullptr;
lv_timer_t* s_asr_timer = nullptr;  // 70ms：轮询真 ASR 共享态并渲染（AsrTick）
lv_timer_t* s_rec_timer = nullptr;
int s_rec_secs = 0;
std::vector<lv_obj_t*> s_wave_bars;

// idle/chat sbar model-name labels (filled in from pi_agent_model_name()
// once the agent is up -- see UpdateModelLabels())
lv_obj_t* s_idle_model_lbl = nullptr;
lv_obj_t* s_chat_model_lbl = nullptr;

// chat view widgets
lv_obj_t* s_feed = nullptr;
lv_obj_t* s_chat_ctx_fill = nullptr;
lv_obj_t* s_chat_ctx_lbl = nullptr;
lv_obj_t* s_mode_icon_flow = nullptr;  // 3-bar hamburger, shown in FLOW
lv_obj_t* s_mode_icon_zen = nullptr;   // ring, shown in ZEN
lv_obj_t* s_mode_lbl = nullptr;
lv_obj_t* s_tts_dot = nullptr;  // 状态栏 TTS 开关状态点（琥珀=开/线框灰=关）
bool s_tts_on = true;           // NVS "pi_screen"/"tts_on"；Create 时灌入 agent 桥
lv_obj_t* s_dock_stat_lbl = nullptr;
lv_obj_t* s_dock_action_box = nullptr;  // holds either STOP or TALK contents
lv_obj_t* s_stop_btn = nullptr;
lv_obj_t* s_talk_btn = nullptr;
lv_obj_t* s_act_line = nullptr;
lv_obj_t* s_act_dot = nullptr;
lv_obj_t* s_act_text = nullptr;
lv_obj_t* s_act_peek = nullptr;
lv_obj_t* s_peek_container = nullptr;
bool s_zen_peeking = false;
bool s_zen_turn_done = false;

// current-turn cursors
lv_obj_t* s_cur_think_row = nullptr;
lv_obj_t* s_cur_think_dot = nullptr;
lv_obj_t* s_cur_think_lbl = nullptr;
lv_obj_t* s_cur_tool_card = nullptr;
lv_obj_t* s_cur_tool_dot = nullptr;
lv_obj_t* s_cur_tool_fn_lbl = nullptr;
lv_obj_t* s_cur_tool_ret_lbl = nullptr;
lv_obj_t* s_cur_tool_body_args_lbl = nullptr;
lv_obj_t* s_cur_tool_body_partial_row = nullptr;  // "partial 等待流式返回" row, RUNNING-only
lvmd::MdView* s_cur_md = nullptr;  // current reply stream; owned by the LVGL tree, not us
uint32_t s_turn_start_ms = 0;
uint32_t s_think_start_ms = 0;
uint32_t s_tool_start_ms = 0;
int s_turn_tool_count = 0;
std::string s_turn_last_tool_output;
int s_out_tokens = 0;
int32_t s_ttfb_ms = -1;  // time to first text_delta this turn; -1 = not yet seen
int s_ctx_pct = 0;
bool s_ctx_known = false;  // false until a real DONE.i1 / context_window reading exists
int s_in_tokens = 0;       // real usage.input from the last DONE, for the dock's "^" stat
bool s_in_tokens_known = false;
std::string s_last_user_prompt;  // for the error banner's retry action

lv_timer_t* s_think_timer = nullptr;
lv_timer_t* s_tool_running_timer = nullptr;
lv_timer_t* s_cursor_blink_timer = nullptr;
lv_timer_t* s_drain_timer = nullptr;

struct ToolCacheEntry {
    std::string name;
    std::string args;
    std::string output;
    int elapsed_ms = 0;
};
std::vector<ToolCacheEntry> s_tool_cache;
bool s_turn_had_thinking = false;
float s_turn_thinking_secs = 0.0f;

// PTT gesture tracking
bool s_ptt_tracking = false;
int32_t s_ptt_start_y = 0;
bool s_ptt_via_key = false;  // current Listen was entered via PWR_KEY (no
                             // physical "release" signal -- see StartListen)

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
lv_obj_t* MakeRect(lv_obj_t* parent, int32_t w, int32_t h, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

lv_obj_t* MakeCircle(lv_obj_t* parent, int32_t d, uint32_t color) {
    lv_obj_t* o = MakeRect(parent, d, d, color);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    return o;
}

lv_obj_t* MakeRing(lv_obj_t* parent, int32_t d, uint32_t border_color, int32_t border_w) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, border_w, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(border_color), LV_PART_MAIN);
    return o;
}

void BreathExecCb(void* var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

void StartBreath(lv_obj_t* obj, uint32_t half_period_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, BreathExecCb);
    lv_anim_set_values(&a, 64, 255);  // opacity .25 -> 1.0
    lv_anim_set_duration(&a, half_period_ms);
    lv_anim_set_reverse_duration(&a, half_period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void StopBreath(lv_obj_t* obj) {
    lv_anim_delete(obj, BreathExecCb);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}

void WaveHeightExecCb(void* var, int32_t v) { lv_obj_set_height(static_cast<lv_obj_t*>(var), v); }
void WaveOpaExecCb(void* var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

std::string FormatSecs1(float secs) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1fs", secs);
    return buf;
}

// Byte offset of the boundary after `cp_count` whole UTF-8 codepoints in
// `s` (clamped to strlen(s) if `s` has fewer). Shared by the ASR reveal
// timer (which needs to color only the most-recently-revealed codepoints
// amber) and the codepoint counter used to size the reveal.
int Utf8PrefixBytes(const char* s, int cp_count) {
    int bytes = 0;
    int cp = 0;
    while (s[bytes] != '\0' && cp < cp_count) {
        unsigned char c = static_cast<unsigned char>(s[bytes]);
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        bytes += len;
        cp++;
    }
    return bytes;
}

void SetLabelFont(lv_obj_t* label, const lv_font_t* font, uint32_t color) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
}

// CTX gauge: real ratio only. pi_agent_context_window() returning 0 (model
// not loaded) means the ratio is meaningless, and no completed turn yet
// means there's no real usage.input to divide -- both render "CTX --" with
// an empty bar rather than a fabricated percentage.
void SetCtxFill(lv_obj_t* fill, int pct) {
    if (fill == nullptr) return;
    // Never leave a visible 0-width fill in the tree -- see the HIDDEN note
    // in BuildCtx(). pct<=0 (unknown ratio, or a real ratio that rounds to
    // 0) both just hide the bar; the track itself still reads "empty".
    if (pct <= 0) {
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_width(fill, LV_PCT(pct));
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
}

void RenderCtxGauge() {
    bool known = s_ctx_known && pi_agent_context_window() > 0;
    int pct = known ? s_ctx_pct : 0;
    const char* text = known ? "CTX" : "CTX --";
    SetCtxFill(s_idle_ctx_fill, pct);
    SetCtxFill(s_chat_ctx_fill, pct);
    if (s_idle_ctx_lbl != nullptr) lv_label_set_text(s_idle_ctx_lbl, text);
    if (s_chat_ctx_lbl != nullptr) lv_label_set_text(s_chat_ctx_lbl, text);
}

void ApplyCtxUnknown() {
    s_ctx_known = false;
    s_ctx_pct = 0;
    RenderCtxGauge();
}

// input_tokens is DONE.i1 (real pi_usage_t.input, per pi_ui_bridge.h); the
// context window is model.context_window via pi_agent_context_window().
void ApplyCtxUsage(uint32_t input_tokens) {
    uint32_t window = pi_agent_context_window();
    if (window == 0) {
        ApplyCtxUnknown();
        return;
    }
    int pct = static_cast<int>((static_cast<uint64_t>(input_tokens) * 100) / window);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    s_ctx_pct = pct;
    s_ctx_known = true;
    RenderCtxGauge();
}

// Real model names (e.g. a deepseek variant) can run longer than the sbar
// has room for at mono-17 next to id/CTX/wifi (and the mode button in the
// chat sbar). Policy: keep the tail -- the part that actually distinguishes
// versions/tiers -- and mark the cut with a leading "..." (three ASCII
// dots; the single-glyph ellipsis isn't in the mono font's ASCII+·°
// subset). Names at or under the budget pass through unchanged.
constexpr size_t kMaxModelNameChars = 16;

std::string FormatModelName(const char* raw) {
    if (raw == nullptr) return "?";
    std::string s(raw);
    if (s.size() <= kMaxModelNameChars) return s;
    return "..." + s.substr(s.size() - (kMaxModelNameChars - 3));
}

void UpdateModelLabels() {
    std::string name = FormatModelName(pi_agent_model_name());
    if (s_idle_model_lbl != nullptr) lv_label_set_text(s_idle_model_lbl, name.c_str());
    if (s_chat_model_lbl != nullptr) lv_label_set_text(s_chat_model_lbl, name.c_str());
}

// ---------------------------------------------------------------------------
// new-session / mode toggle (forward declared, defined after view builders)
// ---------------------------------------------------------------------------
void NewSession();
void SetZen(bool zen);
void Go(ViewState s);
void UpdateDockStat();
void StartListen(ViewState return_state);
void FinishListenSend();
void RetryLastPrompt();
void ShowErrorBanner(const char* message);

// ---------------------------------------------------------------------------
// shared status-bar pieces
// ---------------------------------------------------------------------------

// "|pi" id box: a small filled rect standing in for the "▮" glyph (not in the
// mono font's ASCII+·° subset) + a "pi" label. Tapping it anywhere starts a
// new session (design: "点状态栏 logo -> 新会话").
lv_obj_t* BuildIdBox(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    screen_strip_obj_chrome(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 8, LV_PART_MAIN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(box, true);

    lv_obj_t* mark = MakeRect(box, 8, 18, kAmber);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    (void)mark;

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, "pi");
    SetLabelFont(lbl, &font_pi_mono_17, kText);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(
        box, [](lv_event_t*) { NewSession(); }, LV_EVENT_CLICKED, nullptr);
    return box;
}

lv_obj_t* BuildWifi(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    screen_strip_obj_chrome(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, LV_SIZE_CONTENT, 14);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(box, 2, LV_PART_MAIN);
    static const int32_t kBarH[3] = {5, 9, 13};
    static const uint32_t kBarColor[3] = {kDim, kDim, kFaint};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* bar = MakeRect(box, 3, kBarH[i], kBarColor[i]);
        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
    }
    return box;
}

// CTX gauge: 64x6 track (--bg2) + amber fill, plus the "CTX" label itself
// (swapped to "CTX --" when the ratio is unknown -- see RenderCtxGauge()).
// Returns the fill/label obj via out params so callers can update them
// later.
lv_obj_t* BuildCtx(lv_obj_t* parent, lv_obj_t** out_fill, lv_obj_t** out_label) {
    lv_obj_t* box = lv_obj_create(parent);
    screen_strip_obj_chrome(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 8, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, "CTX");
    SetLabelFont(lbl, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(lbl, 1, LV_PART_MAIN);
    *out_label = lbl;

    lv_obj_t* track = lv_obj_create(box);
    screen_strip_obj_chrome(track);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(track, 64, 6);
    lv_obj_set_style_radius(track, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(track, lv_color_hex(kCard2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(track, true, LV_PART_MAIN);

    lv_obj_t* fill = lv_obj_create(track);
    screen_strip_obj_chrome(fill);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, LV_PCT(100), LV_PCT(100));  // real width set by RenderCtxGauge()
    lv_obj_set_style_bg_color(fill, lv_color_hex(kAmber), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    // Start hidden rather than width=0: a 0-width filled rect is still a
    // draw op with a degenerate (zero-area) buffer, which on this board's
    // PPA/cache-sync flush path produced a steady stream of
    // esp_cache_msync "invalid addr" errors while the screen was up. A
    // HIDDEN object is skipped by the renderer entirely -- no draw op, no
    // degenerate buffer. RenderCtxGauge() unhides it once there's a real,
    // non-zero ratio to show.
    lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    *out_fill = fill;
    return box;
}

lv_obj_t* MakeSpacer(lv_obj_t* parent) {
    lv_obj_t* sp = lv_obj_create(parent);
    screen_strip_obj_chrome(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_grow(sp, 1);
    return sp;
}

lv_obj_t* MakeSbarBase(lv_obj_t* parent) {
    lv_obj_t* sbar = lv_obj_create(parent);
    screen_strip_obj_chrome(sbar);
    lv_obj_remove_flag(sbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sbar, kW, kSbarH);
    lv_obj_set_pos(sbar, 0, 0);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(sbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(sbar, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_right(sbar, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_column(sbar, 20, LV_PART_MAIN);

    lv_obj_t* rule = MakeRect(parent, kW, 1, kLine);
    lv_obj_set_pos(rule, 0, kSbarH - 1);
    return sbar;
}

// ---------------------------------------------------------------------------
// S0 -- idle view
// ---------------------------------------------------------------------------
void UpdateIdleClock(lv_timer_t*) {
    if (s_clock_lbl == nullptr) return;
    time_t now = time(nullptr);
    struct tm tm_info = {};
    char buf[24];
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= 2025 - 1900) {
        // ":" recolored faint, matching the design's <small> colon treatment.
        std::snprintf(buf, sizeof(buf), "%02d#5F5849 :#%02d", tm_info.tm_hour, tm_info.tm_min);
    } else {
        std::snprintf(buf, sizeof(buf), "--#5F5849 :#--");
    }
    lv_label_set_text(s_clock_lbl, buf);

    if (s_date_lbl != nullptr) {
        static const char* kWd[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        static const char* kMo[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char dbuf[32];
        if (tm_info.tm_year >= 2025 - 1900) {
            std::snprintf(dbuf, sizeof(dbuf), "%s \xc2\xb7 %s %d", kWd[tm_info.tm_wday],
                          kMo[tm_info.tm_mon], tm_info.tm_mday);
        } else {
            std::snprintf(dbuf, sizeof(dbuf), "--- \xc2\xb7 --- --");
        }
        lv_label_set_text(s_date_lbl, dbuf);
    }
}

void BuildIdleView(lv_obj_t* parent) {
    s_idle_view = lv_obj_create(parent);
    screen_strip_obj_chrome(s_idle_view);
    lv_obj_remove_flag(s_idle_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_idle_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_idle_view, kW, kH);
    lv_obj_set_pos(s_idle_view, 0, 0);
    lv_obj_set_style_bg_opa(s_idle_view, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t* sbar = MakeSbarBase(s_idle_view);
    lv_obj_t* idbox = BuildIdBox(sbar);
    (void)idbox;
    s_idle_model_lbl = lv_label_create(sbar);
    lv_label_set_text(s_idle_model_lbl, "...");  // filled in by UpdateModelLabels() on LOAD
    SetLabelFont(s_idle_model_lbl, &font_pi_mono_17, kDim);
    MakeSpacer(sbar);
    lv_obj_t* ctx_fill = nullptr;
    lv_obj_t* ctx_lbl = nullptr;
    BuildCtx(sbar, &ctx_fill, &ctx_lbl);
    s_idle_ctx_fill = ctx_fill;
    s_idle_ctx_lbl = ctx_lbl;
    BuildWifi(sbar);

    lv_obj_t* mid = lv_obj_create(s_idle_view);
    screen_strip_obj_chrome(mid);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mid, kW, kMidH);
    lv_obj_set_pos(mid, 0, kSbarH);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(mid, 10, LV_PART_MAIN);

    s_clock_lbl = lv_label_create(mid);
    lv_label_set_recolor(s_clock_lbl, true);
    lv_label_set_text(s_clock_lbl, "--:--");
    SetLabelFont(s_clock_lbl, &font_pi_clock_132, kText);

    s_date_lbl = lv_label_create(mid);
    lv_label_set_text(s_date_lbl, "");
    SetLabelFont(s_date_lbl, &font_pi_mono_17, kDim);
    lv_obj_set_style_text_letter_space(s_date_lbl, 3, LV_PART_MAIN);

    UpdateIdleClock(nullptr);

    s_idle_breath = MakeCircle(mid, 12, kAmber);
    lv_obj_remove_flag(s_idle_breath, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_top(s_idle_breath, 34, LV_PART_MAIN);
    StartBreath(s_idle_breath, 1600);

    lv_obj_t* hint = lv_obj_create(s_idle_view);
    screen_strip_obj_chrome(hint);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hint, kW, kHintH);
    lv_obj_set_pos(hint, 0, kSbarH + kMidH);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t* hrule = MakeRect(s_idle_view, kW, 1, kLine);
    lv_obj_set_pos(hrule, 0, kSbarH + kMidH);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint, 16, LV_PART_MAIN);

    lv_obj_t* h1 = lv_label_create(hint);
    lv_label_set_text(h1, "\xe6\x8c\x89\xe4\xbd\x8f\xe8\xaf\xb4\xe8\xaf\x9d");  // "按住说话"
    SetLabelFont(h1, &font_puhui_20_4, kFaint);
    lv_obj_t* h2 = lv_label_create(hint);
    lv_label_set_text(h2, "HOLD KEY / TOUCH TO TALK");
    SetLabelFont(h2, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(h2, 2, LV_PART_MAIN);

    s_clock_timer = lv_timer_create(UpdateIdleClock, 1000, nullptr);
}

// ---------------------------------------------------------------------------
// S1 -- listen view (PTT / fake ASR)
// ---------------------------------------------------------------------------
void UpdateRecTimer(lv_timer_t*) {
    s_rec_secs++;
    if (s_rec_lbl == nullptr) return;
    // The "●" bullet is a real lv_obj dot built alongside this label in
    // BuildListenView() (font subset has no bullet glyph); this label only
    // owns the "REC m:ss" text.
    char buf[24];
    std::snprintf(buf, sizeof(buf), "REC %d:%02d", s_rec_secs / 60, s_rec_secs % 60);
    lv_label_set_text(s_rec_lbl, buf);
}

// Trailing codepoints of the live ASR text rendered amber, matching the
// design's ".cur" = "just-revealed fragment" treatment. 6 is a plain
// character count, not a "word" (real CJK word-segmentation for a transient
// highlight would be pure overkill). Rendering itself happens in AsrTick
// (defined with the rest of the real-ASR engine, before StartListen).
constexpr int kAsrHighlightCodepoints = 6;

void BuildListenView(lv_obj_t* parent) {
    s_listen_view = lv_obj_create(parent);
    screen_strip_obj_chrome(s_listen_view);
    lv_obj_remove_flag(s_listen_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_listen_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_listen_view, kW, kH);
    lv_obj_set_pos(s_listen_view, 0, 0);
    lv_obj_set_style_bg_opa(s_listen_view, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_listen_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* sbar = MakeSbarBase(s_listen_view);
    BuildIdBox(sbar);
    lv_obj_t* rec_dot = MakeCircle(sbar, 8, kAmber);
    lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_CLICKABLE);
    s_rec_lbl = lv_label_create(sbar);
    lv_label_set_text(s_rec_lbl, "REC 0:00");
    SetLabelFont(s_rec_lbl, &font_pi_mono_17, kAmber);
    MakeSpacer(sbar);
    BuildWifi(sbar);

    lv_obj_t* mid = lv_obj_create(s_listen_view);
    screen_strip_obj_chrome(mid);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mid, kW - 128, kMidH);
    lv_obj_set_pos(mid, 64, kSbarH);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(mid, 44, LV_PART_MAIN);

    s_wave_row = lv_obj_create(mid);
    screen_strip_obj_chrome(s_wave_row);
    lv_obj_remove_flag(s_wave_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_wave_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_wave_row, kW - 128, 120);
    lv_obj_set_style_bg_opa(s_wave_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_wave_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wave_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_wave_row, 6, LV_PART_MAIN);

    for (int i = 0; i < 26; i++) {
        float h = 20.0f + std::round(90.0f * std::fabs(std::sin(i * 0.7f)) *
                                     (0.4f + 0.6f * std::fabs(std::sin(i * 2.3f))));
        lv_obj_t* bar = MakeRect(s_wave_row, 8, 12, kAmber);
        lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
        s_wave_bars.push_back(bar);

        lv_anim_t ah;
        lv_anim_init(&ah);
        lv_anim_set_var(&ah, bar);
        lv_anim_set_exec_cb(&ah, WaveHeightExecCb);
        lv_anim_set_values(&ah, 12, static_cast<int32_t>(h));
        lv_anim_set_duration(&ah, 500);
        lv_anim_set_reverse_duration(&ah, 500);
        lv_anim_set_delay(&ah, (i * 67) % 500);
        lv_anim_set_repeat_count(&ah, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ah, lv_anim_path_ease_in_out);
        lv_anim_start(&ah);

        lv_anim_t ao;
        lv_anim_init(&ao);
        lv_anim_set_var(&ao, bar);
        lv_anim_set_exec_cb(&ao, WaveOpaExecCb);
        lv_anim_set_values(&ao, 115, 255);
        lv_anim_set_duration(&ao, 500);
        lv_anim_set_reverse_duration(&ao, 500);
        lv_anim_set_delay(&ao, (i * 67) % 500);
        lv_anim_set_repeat_count(&ao, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ao, lv_anim_path_ease_in_out);
        lv_anim_start(&ao);
    }

    s_asr_lbl = lv_label_create(mid);
    lv_label_set_recolor(s_asr_lbl, true);  // trailing revealed fragment -> amber
    lv_label_set_text(s_asr_lbl, "");
    lv_label_set_long_mode(s_asr_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_asr_lbl, kW - 128);
    lv_obj_set_style_text_align(s_asr_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    SetLabelFont(s_asr_lbl, &font_puhui_30_4, kText);
    lv_obj_set_style_text_line_space(s_asr_lbl, 8, LV_PART_MAIN);

    lv_obj_t* hint = lv_obj_create(s_listen_view);
    screen_strip_obj_chrome(hint);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hint, kW, kHintH);
    lv_obj_set_pos(hint, 0, kSbarH + kMidH);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t* hrule = MakeRect(s_listen_view, kW, 1, kLine);
    lv_obj_set_pos(hrule, 0, kSbarH + kMidH);
    lv_obj_set_style_pad_left(hint, 44, LV_PART_MAIN);
    lv_obj_set_style_pad_right(hint, 44, LV_PART_MAIN);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* cancel_lbl = lv_label_create(hint);
    lv_label_set_text(cancel_lbl, "^ \xe4\xb8\x8a\xe6\xbb\x91\xe5\x8f\x96\xe6\xb6\x88");  // "^ 上滑取消"
    SetLabelFont(cancel_lbl, &font_puhui_20_4, kFaint);
    lv_obj_t* go_lbl = lv_label_create(hint);
    lv_label_set_text(go_lbl, "\xe6\x9d\xbe\xe5\xbc\x80\xe5\x8f\x91\xe9\x80\x81 ->");  // "松开发送 ->"
    SetLabelFont(go_lbl, &font_puhui_20_4, kAmber);
}

// ---------------------------------------------------------------------------
// S2/S3 -- chat view: sbar (with FLOW/ZEN toggle), feed, dock
// ---------------------------------------------------------------------------
void ApplyModeVisual() {
    if (s_mode_icon_flow == nullptr) return;
    lv_obj_add_flag(s_zen ? s_mode_icon_flow : s_mode_icon_zen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_zen ? s_mode_icon_zen : s_mode_icon_flow, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_mode_lbl, s_zen ? "ZEN" : "FLOW");

    // ZEN: hide any already-built think/tool widgets for the in-flight turn
    // and show the act line; FLOW: the reverse. Per-turn cache still drives
    // "查看过程" rebuilds regardless of which mode we're in right now.
    if (s_cur_think_row != nullptr) {
        if (s_zen) lv_obj_add_flag(s_cur_think_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_cur_think_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_cur_tool_card != nullptr) {
        if (s_zen) lv_obj_add_flag(s_cur_tool_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_cur_tool_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_act_line != nullptr) {
        if (s_zen) lv_obj_remove_flag(s_act_line, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_act_line, LV_OBJ_FLAG_HIDDEN);
    }
}

void SetZen(bool zen) {
    s_zen = zen;
    Settings settings("pi_screen", true);
    settings.SetBool("zen_mode", zen);
    ApplyModeVisual();
}

// TTS 播报开关（状态栏）：mono "TTS" 字样 + 状态点（琥珀=开/线框灰=关，
// 🔊/🔇 语义——字体子集没有喇叭 glyph，沿用本文件"点代 glyph"惯例）。
// 默认开；关闭立即打断当前播报（pi_agent_tts_set_enabled 内部异步 stop）。
void ApplyTtsVisual() {
    if (s_tts_dot == nullptr) return;
    lv_obj_set_style_bg_color(s_tts_dot, lv_color_hex(s_tts_on ? kAmber : kLine2),
                              LV_PART_MAIN);
}

void SetTtsOn(bool on) {
    s_tts_on = on;
    Settings settings("pi_screen", true);
    settings.SetBool("tts_on", on);
    pi_agent_tts_set_enabled(on);
    ApplyTtsVisual();
}

lv_obj_t* BuildChatSbar(lv_obj_t* parent) {
    lv_obj_t* sbar = MakeSbarBase(parent);
    BuildIdBox(sbar);
    s_chat_model_lbl = lv_label_create(sbar);
    lv_label_set_text(s_chat_model_lbl, "...");  // filled in by UpdateModelLabels() on LOAD
    SetLabelFont(s_chat_model_lbl, &font_pi_mono_17, kDim);
    MakeSpacer(sbar);
    lv_obj_t* ctx_fill = nullptr;
    lv_obj_t* ctx_lbl = nullptr;
    BuildCtx(sbar, &ctx_fill, &ctx_lbl);
    s_chat_ctx_fill = ctx_fill;
    s_chat_ctx_lbl = ctx_lbl;

    lv_obj_t* mode_btn = lv_obj_create(sbar);
    screen_strip_obj_chrome(mode_btn);
    lv_obj_remove_flag(mode_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mode_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(mode_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(mode_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(mode_btn, lv_color_hex(kLine), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(mode_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(mode_btn, 4, LV_PART_MAIN);
    lv_obj_add_flag(mode_btn, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mode_btn, true);
    lv_obj_set_flex_flow(mode_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mode_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mode_btn, 8, LV_PART_MAIN);

    // FLOW icon: 3 stacked bars ("≡" isn't in the mono subset).
    lv_obj_t* flow_icon = lv_obj_create(mode_btn);
    screen_strip_obj_chrome(flow_icon);
    lv_obj_remove_flag(flow_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(flow_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(flow_icon, 14, 12);
    lv_obj_set_style_bg_opa(flow_icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(flow_icon, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(flow_icon, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) MakeRect(flow_icon, 14, 2, kFaint);
    s_mode_icon_flow = flow_icon;

    // ZEN icon: a small ring ("◎" isn't in the mono subset).
    lv_obj_t* zen_icon = MakeRing(mode_btn, 12, kFaint, 2);
    lv_obj_add_flag(zen_icon, LV_OBJ_FLAG_HIDDEN);
    s_mode_icon_zen = zen_icon;

    s_mode_lbl = lv_label_create(mode_btn);
    lv_label_set_text(s_mode_lbl, "FLOW");
    SetLabelFont(s_mode_lbl, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(s_mode_lbl, 1, LV_PART_MAIN);

    lv_obj_add_event_cb(
        mode_btn, [](lv_event_t*) { SetZen(!s_zen); }, LV_EVENT_CLICKED, nullptr);

    // TTS 开关（见 SetTtsOn 的注释）
    lv_obj_t* tts_btn = lv_obj_create(sbar);
    screen_strip_obj_chrome(tts_btn);
    lv_obj_remove_flag(tts_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tts_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tts_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(tts_btn, 4, LV_PART_MAIN);
    lv_obj_add_flag(tts_btn, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(tts_btn, true);
    lv_obj_set_flex_flow(tts_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tts_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tts_btn, 6, LV_PART_MAIN);
    s_tts_dot = MakeCircle(tts_btn, 8, kAmber);
    lv_obj_remove_flag(s_tts_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* tts_lbl = lv_label_create(tts_btn);
    lv_label_set_text(tts_lbl, "TTS");
    SetLabelFont(tts_lbl, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(tts_lbl, 1, LV_PART_MAIN);
    lv_obj_remove_flag(tts_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        tts_btn, [](lv_event_t*) { SetTtsOn(!s_tts_on); }, LV_EVENT_CLICKED, nullptr);

    BuildWifi(sbar);
    return sbar;
}

// ---------------------------------------------------------------------------
// feed rows
// ---------------------------------------------------------------------------
void AppendUserRow(const std::string& text) {
    lv_obj_t* row = lv_obj_create(s_feed);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);

    lv_obj_t* who = lv_label_create(row);
    lv_label_set_text(who, "YOU");
    SetLabelFont(who, &font_pi_mono_14, kAmber);
    lv_obj_set_style_text_letter_space(who, 2, LV_PART_MAIN);

    lv_obj_t* t = lv_label_create(row);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_text(t, text.c_str());
    SetLabelFont(t, &font_puhui_20_4, kDim);
}

// Thin red-line error banner in the chat feed (design: 红色细线横幅) --
// shared by the agent bridge's UI_ERROR events and the S1 real-ASR failure
// path. The amber "重试" pill resends the last prompt; when no prompt exists
// yet (ASR failed before anything was sent) the pill is omitted since there
// is nothing to retry.
void ShowErrorBanner(const char* message) {
    lv_obj_t* row = lv_obj_create(s_feed);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(kErr), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);

    lv_obj_t* msg = lv_label_create(row);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(msg, 1);
    lv_label_set_text(msg, message != nullptr ? message : "unknown error");
    SetLabelFont(msg, &font_puhui_20_4, kErr);

    if (!s_last_user_prompt.empty()) {
        lv_obj_t* retry = lv_label_create(row);
        lv_label_set_text(retry, "\xe9\x87\x8d\xe8\xaf\x95");  // "重试"
        SetLabelFont(retry, &font_puhui_20_4, kAmber);
        lv_obj_add_flag(retry, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(retry, true);
        lv_obj_add_event_cb(
            retry, [](lv_event_t*) { RetryLastPrompt(); }, LV_EVENT_CLICKED, nullptr);
    }
}

// "thinking" prefix: a small ring standing in for "◌" (not in the mono
// subset), matching the ZEN-icon treatment above.
lv_obj_t* CreateThinkRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);

    lv_obj_t* dot = MakeRing(row, 10, kAmberDim, 1);
    s_cur_think_dot = dot;
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, "thinking");
    SetLabelFont(lbl, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(lbl, 1, LV_PART_MAIN);
    s_cur_think_lbl = lbl;
    return row;
}

lv_obj_t* CreateToolCard(lv_obj_t* parent, const char* name);
void ToggleToolBody(lv_event_t* e);

lv_obj_t* CreateToolCard(lv_obj_t* parent, const char* name) {
    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(kLine), LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(card, true);
    // head 与 body 纵向堆叠；漏掉这行时二者都落在 (0,0)，展开 body 会与
    // 标题行重叠（sim 交互测试发现的真机同现 bug）。
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* head = lv_obj_create(card);
    screen_strip_obj_chrome(head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(head, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(head, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 14, LV_PART_MAIN);

    lv_obj_t* dot = MakeCircle(head, 10, kAmber);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    StartBreath(dot, 500);
    s_cur_tool_dot = dot;

    lv_obj_t* fn = lv_label_create(head);
    lv_label_set_text(fn, name);
    SetLabelFont(fn, &font_pi_mono_17, kText);
    s_cur_tool_fn_lbl = fn;

    MakeSpacer(head);
    lv_obj_t* ret = lv_label_create(head);
    lv_label_set_text(ret, "RUNNING");
    SetLabelFont(ret, &font_pi_mono_17, kAmber);
    s_cur_tool_ret_lbl = ret;

    lv_obj_t* body = lv_obj_create(card);
    screen_strip_obj_chrome(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(body, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(body, 14, LV_PART_MAIN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* body_rule = MakeRect(body, 1, 1, kLine);
    lv_obj_set_width(body_rule, LV_PCT(100));
    lv_obj_align(body_rule, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* args = lv_label_create(body);
    lv_label_set_recolor(args, true);
    lv_label_set_long_mode(args, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(args, LV_PCT(100));
    lv_label_set_text(args, "#5F5849 args#    (streaming...)");
    SetLabelFont(args, &font_pi_mono_14, kDim);
    lv_obj_set_style_text_line_space(args, 6, LV_PART_MAIN);
    s_cur_tool_body_args_lbl = args;

    // RUNNING-only footer line (design: "partial ▍ 等待流式返回 ·
    // toolcall_delta"). "▍" isn't in the mono font's ASCII+·°-only subset,
    // so it's a real small lv_obj rect, same treatment as the other
    // standalone glyph substitutions in this file. Hidden once TOOL_END
    // finalizes the card (see DrainQueueTick's UI_TOOL_END case).
    lv_obj_t* partial_row = lv_obj_create(body);
    screen_strip_obj_chrome(partial_row);
    lv_obj_remove_flag(partial_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(partial_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(partial_row, LV_PCT(100));
    lv_obj_set_height(partial_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(partial_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(partial_row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(partial_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(partial_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(partial_row, 8, LV_PART_MAIN);
    lv_obj_t* mark = MakeRect(partial_row, 6, 16, kFaint);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* partial_lbl = lv_label_create(partial_row);
    lv_label_set_text(partial_lbl, "partial \xe7\xad\x89\xe5\xbe\x85\xe6\xb5\x81\xe5\xbc\x8f\xe8\xbf\x94\xe5\x9b\x9e");
    SetLabelFont(partial_lbl, &font_puhui_20_4, kFaint);
    s_cur_tool_body_partial_row = partial_row;

    lv_obj_add_event_cb(card, ToggleToolBody, LV_EVENT_CLICKED, body);
    return card;
}

void ToggleToolBody(lv_event_t* e) {
    lv_obj_t* body = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (body == nullptr) return;
    if (lv_obj_has_flag(body, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(body, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
}

void OnPeekClicked(lv_event_t*);

void BuildActLine(lv_obj_t* parent) {
    s_act_line = lv_obj_create(parent);
    screen_strip_obj_chrome(s_act_line);
    lv_obj_remove_flag(s_act_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_act_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(s_act_line, LV_PCT(100));
    lv_obj_set_height(s_act_line, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_act_line, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_act_line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_act_line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_act_line, 12, LV_PART_MAIN);
    lv_obj_add_flag(s_act_line, LV_OBJ_FLAG_HIDDEN);

    s_act_dot = MakeCircle(s_act_line, 8, kAmber);
    lv_obj_remove_flag(s_act_dot, LV_OBJ_FLAG_CLICKABLE);
    StartBreath(s_act_dot, 550);

    s_act_text = lv_label_create(s_act_line);
    lv_label_set_text(s_act_text, "connecting...");
    SetLabelFont(s_act_text, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(s_act_text, 1, LV_PART_MAIN);

    MakeSpacer(s_act_line);
    s_act_peek = lv_label_create(s_act_line);
    lv_label_set_text(s_act_peek, "");
    // Contains CJK ("查看过程"/"收起"): must use the CJK font, not the
    // ASCII+·°-only mono subset, even though the design calls this row out
    // as mono-styled -- see the font-subset note atop this file.
    SetLabelFont(s_act_peek, &font_puhui_20_4, kAmberDim);
    lv_obj_add_flag(s_act_peek, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_act_peek, OnPeekClicked, LV_EVENT_CLICKED, nullptr);
}

// Rebuild (or tear down) a temporary read-only view of the just-finished
// turn's thinking + tool cards from the per-turn cache, per the ZEN "查看
// 过程" interaction. Only available once the turn is done (matches the
// design's `if (!elAct.classList.contains('done')) return;` guard).
void OnPeekClicked(lv_event_t*) {
    if (!s_zen_turn_done) return;
    s_zen_peeking = !s_zen_peeking;
    if (s_zen_peeking) {
        lv_label_set_text(s_act_peek, "\xe6\x94\xb6\xe8\xb5\xb7 ^");  // "收起 ^"
        s_peek_container = lv_obj_create(s_feed);
        screen_strip_obj_chrome(s_peek_container);
        lv_obj_remove_flag(s_peek_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_peek_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(s_peek_container, LV_PCT(100));
        lv_obj_set_height(s_peek_container, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(s_peek_container, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_flex_flow(s_peek_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_peek_container, 16, LV_PART_MAIN);
        // Move it right before the act line so ordering matches the feed.
        lv_obj_move_to_index(s_peek_container, lv_obj_get_index(s_act_line));

        if (s_turn_had_thinking) {
            lv_obj_t* row = CreateThinkRow(s_peek_container);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "thinking \xc2\xb7 %s",
                          FormatSecs1(s_turn_thinking_secs).c_str());
            lv_label_set_text(s_cur_think_lbl, buf);
            (void)row;
        }
        for (auto& tc : s_tool_cache) {
            lv_obj_t* card = CreateToolCard(s_peek_container, tc.name.c_str());
            lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
            StopBreath(s_cur_tool_dot);
            lv_obj_set_style_bg_color(s_cur_tool_dot, lv_color_hex(kOk), LV_PART_MAIN);
            char ret[64];
            std::snprintf(ret, sizeof(ret), "%s \xc2\xb7 %s", tc.output.c_str(),
                          FormatSecs1(tc.elapsed_ms / 1000.0f).c_str());
            lv_label_set_text(s_cur_tool_ret_lbl, ret);
            SetLabelFont(s_cur_tool_ret_lbl, &font_pi_mono_17, kOk);
        }
    } else {
        lv_label_set_text(s_act_peek, "\xe6\x9f\xa5\xe7\x9c\x8b\xe8\xbf\x87\xe7\xa8\x8b >");  // "查看过程 >"
        if (s_peek_container != nullptr) {
            lv_obj_delete(s_peek_container);
            s_peek_container = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// dock: token/ttfb stats + STOP (busy) / TALK (idle-in-chat) action
// ---------------------------------------------------------------------------
void StartListen(ViewState return_state);
void CancelListen();
void FinishListenSend();

// Shared PTT gesture handlers -- registered on both s_ptt_layer (idle/listen
// zone) and the chat dock's TALK button (continue-conversation case).
// user_data carries the ViewState to return to on cancel/send (Idle or
// Chat); hiding a pressed object (never deleting it) keeps LVGL delivering
// PRESSING/RELEASED to the same target, so this works unchanged even though
// StartListen() swaps which view is visible mid-gesture.
void OnPttPressed(lv_event_t* e) {
    ViewState ret = static_cast<ViewState>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    s_ptt_tracking = true;
    lv_indev_t* indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_ptt_start_y = p.y;
    StartListen(ret);
}

void OnPttPressing(lv_event_t* e) {
    if (!s_ptt_tracking) return;
    lv_indev_t* indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_ptt_start_y;
    if (dy < -kSwipeCancelThreshold) {
        s_ptt_tracking = false;
        lv_indev_wait_release(indev);
        CancelListen();
    }
}

void OnPttReleased(lv_event_t* e) {
    (void)e;
    if (!s_ptt_tracking) return;
    s_ptt_tracking = false;
    FinishListenSend();
}

void BuildDock(lv_obj_t* parent) {
    lv_obj_t* dock = lv_obj_create(parent);
    screen_strip_obj_chrome(dock);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dock, kW, kDockH);
    lv_obj_set_pos(dock, 0, kSbarH + kFeedH);
    lv_obj_set_style_bg_opa(dock, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t* rule = MakeRect(parent, kW, 1, kLine);
    lv_obj_set_pos(rule, 0, kSbarH + kFeedH);
    lv_obj_set_style_pad_left(dock, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dock, 28, LV_PART_MAIN);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dock, 24, LV_PART_MAIN);

    s_dock_stat_lbl = lv_label_create(dock);
    lv_label_set_text(s_dock_stat_lbl, "");
    SetLabelFont(s_dock_stat_lbl, &font_pi_mono_14, kFaint);
    lv_obj_set_style_text_letter_space(s_dock_stat_lbl, 1, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_dock_stat_lbl, 6, LV_PART_MAIN);

    MakeSpacer(dock);

    s_dock_action_box = lv_obj_create(dock);
    screen_strip_obj_chrome(s_dock_action_box);
    lv_obj_remove_flag(s_dock_action_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dock_action_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_dock_action_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_dock_action_box, LV_OPA_TRANSP, LV_PART_MAIN);

    // STOP button.
    s_stop_btn = lv_obj_create(s_dock_action_box);
    screen_strip_obj_chrome(s_stop_btn);
    lv_obj_remove_flag(s_stop_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_stop_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_stop_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_stop_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_stop_btn, lv_color_hex(kLine2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_stop_btn, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_stop_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_stop_btn, 30, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_stop_btn, 18, LV_PART_MAIN);
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(s_stop_btn, true);
    lv_obj_set_flex_flow(s_stop_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_stop_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_stop_btn, 14, LV_PART_MAIN);
    lv_obj_t* stop_icon = MakeRect(s_stop_btn, 16, 16, kErr);
    lv_obj_set_style_radius(stop_icon, 3, LV_PART_MAIN);
    lv_obj_t* stop_lbl = lv_label_create(s_stop_btn);
    lv_label_set_text(stop_lbl, "STOP \xc2\xb7 \xe7\x9f\xad\xe6\x8c\x89 KEY");  // "STOP · 短按 KEY"
    // Mixed ASCII+CJK: needs the CJK font (mono has no "短按" glyphs).
    SetLabelFont(stop_lbl, &font_puhui_20_4, kText);
    lv_obj_set_style_text_letter_space(stop_lbl, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(
        s_stop_btn, [](lv_event_t*) { pi_agent_task_abort(); }, LV_EVENT_CLICKED, nullptr);

    // TALK button (post-done): press-and-hold re-enters listen, same as the
    // idle screen's PTT gesture.
    s_talk_btn = lv_obj_create(s_dock_action_box);
    screen_strip_obj_chrome(s_talk_btn);
    lv_obj_remove_flag(s_talk_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_talk_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_talk_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_talk_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_talk_btn, lv_color_hex(kAmberDim), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_talk_btn, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_talk_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_talk_btn, 34, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_talk_btn, 18, LV_PART_MAIN);
    lv_obj_add_flag(s_talk_btn, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(s_talk_btn, true);
    lv_obj_add_flag(s_talk_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(s_talk_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_talk_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_talk_btn, 14, LV_PART_MAIN);
    MakeRing(s_talk_btn, 14, kAmber, 2);
    lv_obj_t* talk_lbl = lv_label_create(s_talk_btn);
    lv_label_set_text(talk_lbl, "\xe6\x8c\x89\xe4\xbd\x8f\xe8\xaf\xb4\xe8\xaf\x9d");  // "按住说话"
    SetLabelFont(talk_lbl, &font_puhui_20_4, kAmber);  // CJK text needs the CJK font
    lv_obj_set_style_text_letter_space(talk_lbl, 2, LV_PART_MAIN);
    void* ret_chat = reinterpret_cast<void*>(static_cast<intptr_t>(ViewState::Chat));
    lv_obj_add_event_cb(s_talk_btn, OnPttPressed, LV_EVENT_PRESSED, ret_chat);
    lv_obj_add_event_cb(s_talk_btn, OnPttPressing, LV_EVENT_PRESSING, ret_chat);
    lv_obj_add_event_cb(s_talk_btn, OnPttReleased, LV_EVENT_RELEASED, ret_chat);
}

void ShowStopBtn() {
    lv_obj_remove_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_talk_btn, LV_OBJ_FLAG_HIDDEN);
}
void ShowTalkBtn() {
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_talk_btn, LV_OBJ_FLAG_HIDDEN);
}

void BuildChatView(lv_obj_t* parent) {
    s_chat_view = lv_obj_create(parent);
    screen_strip_obj_chrome(s_chat_view);
    lv_obj_remove_flag(s_chat_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_chat_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_chat_view, kW, kH);
    lv_obj_set_pos(s_chat_view, 0, 0);
    lv_obj_set_style_bg_opa(s_chat_view, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_chat_view, LV_OBJ_FLAG_HIDDEN);

    BuildChatSbar(s_chat_view);

    s_feed = lv_obj_create(s_chat_view);
    screen_strip_obj_chrome(s_feed);
    lv_obj_set_size(s_feed, kW, kFeedH);
    lv_obj_set_pos(s_feed, 0, kSbarH);
    lv_obj_set_style_bg_opa(s_feed, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_feed, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_feed, LV_DIR_VER);
    lv_obj_set_style_pad_top(s_feed, 30, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_feed, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_feed, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_feed, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_feed, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_feed, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_feed, 26, LV_PART_MAIN);

    BuildActLine(s_feed);
    BuildDock(s_chat_view);
    ShowStopBtn();
}

// ---------------------------------------------------------------------------
// assistant streaming text -- markdown block rendering (components/
// lv_markdown). One MdView per uninterrupted text stream: a thinking/tool
// event finalizes the current stream so later text opens a new view BELOW
// the card (the single-label version kept appending above it). The blinking
// block cursor (same 14x32 amber rect, FLOATING, letter_pos-tracked) now
// lives inside MdView.
// ---------------------------------------------------------------------------
const lvmd::MdTheme& PiMdTheme() {
    static const lvmd::MdTheme theme = [] {
        lvmd::MdTheme t = lvmd::MdThemeDefaultDark();
        t.body = &font_puhui_30_4;
        t.heading = &font_puhui_30_4;
        t.mono = &font_pi_mono_17;
        t.mono_cjk = &font_puhui_20_4;  // code containing non-ASCII: readable beats monospaced
        t.code_info_font = &font_pi_mono_14;
        t.text = kText;
        t.bold = 0xFFD584;    // light amber: no CJK bold font, and white-on-cream was too subtle
        t.italic = 0xFFFFFF;  // no italic glyphs either; gentle emphasis-by-color
        t.strike = kFaint;    // per-span strikethrough is impossible; deleted = faded
        t.inline_code = kAmber;
        t.link = kAmber;
        t.task_done_text = kDim;
        t.table_header = kAmberDim;
        t.heading_color[0] = kAmber;
        t.heading_color[1] = kAmber;
        t.heading_color[2] = kAmberDim;
        t.quote_text = kDim;
        t.quote_bar = kAmberDim;
        t.rule = kLine;
        t.marker = kAmber;
        t.code_text = kText;
        t.code_bg = kCard;
        t.code_border = kLine;
        t.code_info = kFaint;
        return t;
    }();
    return theme;
}

void EnsureMdView() {
    if (s_cur_md != nullptr) return;
    s_cur_md = lvmd::MdView::Create(s_feed, PiMdTheme(), kW - 32 * 2);
}

void AppendAssistantText(const char* delta) {
    EnsureMdView();
    s_cur_md->Append(delta);
}

// Idempotent; the next TEXT_DELTA opens a fresh MdView appended after
// whatever entered the feed in between.
void FinalizeMdView() {
    if (s_cur_md == nullptr) return;
    s_cur_md->Finish();
    s_cur_md = nullptr;
}

void CursorBlinkTick(lv_timer_t*) {
    if (s_cur_md != nullptr) s_cur_md->BlinkCursor();
}

// ---------------------------------------------------------------------------
// per-turn lifecycle
// ---------------------------------------------------------------------------
void ResetTurnState() {
    s_cur_think_row = s_cur_think_dot = s_cur_think_lbl = nullptr;
    s_cur_tool_card = s_cur_tool_dot = s_cur_tool_fn_lbl = nullptr;
    s_cur_tool_ret_lbl = s_cur_tool_body_args_lbl = s_cur_tool_body_partial_row = nullptr;
    FinalizeMdView();
    s_turn_start_ms = lv_tick_get();
    s_think_start_ms = 0;
    s_tool_start_ms = 0;
    s_turn_tool_count = 0;
    s_turn_last_tool_output.clear();
    s_out_tokens = 0;
    s_in_tokens = 0;
    s_in_tokens_known = false;
    s_ttfb_ms = -1;
    s_tool_cache.clear();
    s_turn_had_thinking = false;
    s_turn_thinking_secs = 0.0f;
    s_zen_turn_done = false;
    s_zen_peeking = false;
    if (s_peek_container != nullptr) {
        lv_obj_delete(s_peek_container);
        s_peek_container = nullptr;
    }
    if (s_think_timer != nullptr) { lv_timer_delete(s_think_timer); s_think_timer = nullptr; }
    if (s_tool_running_timer != nullptr) { lv_timer_delete(s_tool_running_timer); s_tool_running_timer = nullptr; }

    lv_label_set_text(s_act_text, "connecting...");
    StopBreath(s_act_dot);
    lv_obj_set_style_bg_color(s_act_dot, lv_color_hex(kAmber), LV_PART_MAIN);
    StartBreath(s_act_dot, 550);
    lv_label_set_text(s_act_peek, "");
    UpdateDockStat();
    ShowStopBtn();
}

void ScrollFeedToBottom() {
    if (s_feed == nullptr) return;
    // scroll_to_view(last child) only aligns the child's TOP edge once it is
    // taller than the viewport -- a streaming MdView reply quickly is -- so
    // follow the stream by scrolling to the real content bottom instead.
    // This update_layout is the tick's ONE forced layout pass; MdView's
    // SyncCursor below piggybacks on it.
    lv_obj_update_layout(s_feed);
    int32_t below = lv_obj_get_scroll_bottom(s_feed);
    // While streaming, ANIM_ON would restart a 200-400ms scroll animation
    // every 80ms tick -- it never finishes, so every 16ms refresh has a
    // scroll step and (in FULL render mode) a full-screen repaint. Content
    // already grows tick by tick; snapping is visually equivalent.
    if (below > 0) {
        lv_obj_scroll_to_y(s_feed, lv_obj_get_scroll_y(s_feed) + below,
                           s_cur_md != nullptr ? LV_ANIM_OFF : LV_ANIM_ON);
    }
}

// ---------------------------------------------------------------------------
// pi_ai event -> queue -> widget drain (LVGL thread, 80ms; no adapter lock
// needed here -- see blueprint R8)
// ---------------------------------------------------------------------------
void ThinkTimerTick(lv_timer_t*) {
    float secs = (lv_tick_get() - s_think_start_ms) / 1000.0f;
    s_turn_thinking_secs = secs;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "thinking \xc2\xb7 %s", FormatSecs1(secs).c_str());
    if (s_cur_think_lbl != nullptr) lv_label_set_text(s_cur_think_lbl, buf);
    if (s_zen) lv_label_set_text(s_act_text, buf);
}

void ToolRunningTimerTick(lv_timer_t*) {
    if (s_cur_tool_ret_lbl == nullptr) return;
    float secs = (lv_tick_get() - s_tool_start_ms) / 1000.0f;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "RUNNING %s", FormatSecs1(secs).c_str());
    lv_label_set_text(s_cur_tool_ret_lbl, buf);
}

// "^"/"v" stand in for "↑"/"↓" (not in the mono font's ASCII+·°-only
// subset). ^ only ever shows a real number once DONE has reported real
// usage.input at least once this turn; before that (and forever, if the
// bridge never sends it) it shows "--" rather than a fabricated count.
void UpdateDockStat() {
    if (s_dock_stat_lbl == nullptr) return;
    char stat[80];
    char in_part[24];
    if (s_in_tokens_known) {
        std::snprintf(in_part, sizeof(in_part), "%d tok", s_in_tokens);
    } else {
        std::snprintf(in_part, sizeof(in_part), "-- tok");
    }
    if (s_ttfb_ms >= 0) {
        std::snprintf(stat, sizeof(stat), "^ %s \xc2\xb7 v %d tok\nttfb %dms", in_part,
                      s_out_tokens, static_cast<int>(s_ttfb_ms));
    } else {
        std::snprintf(stat, sizeof(stat), "^ %s \xc2\xb7 v %d tok", in_part, s_out_tokens);
    }
    lv_label_set_text(s_dock_stat_lbl, stat);
}

void DrainQueueTick(lv_timer_t*) {
    pi_ui_evt_t evt;
    QueueHandle_t q = pi_ui_queue();
    std::string batched_text;
    int budget = 64;
    bool touched = false;
    bool stat_dirty = false;
    while (budget-- > 0 && xQueueReceive(q, &evt, 0) == pdTRUE) {
        touched = true;
        switch (evt.kind) {
            case UI_AGENT_START:
                ShowStopBtn();
                break;
            case UI_THINKING_START:
                // A card is about to enter the feed: flush pending text into
                // the current reply stream and close it, so post-card text
                // starts a new stream below the card (correct visual order).
                if (!batched_text.empty()) {
                    AppendAssistantText(batched_text.c_str());
                    batched_text.clear();
                }
                FinalizeMdView();
                s_turn_had_thinking = true;
                s_think_start_ms = lv_tick_get();
                if (!s_zen && s_cur_think_row == nullptr) {
                    // CreateThinkRow appends at the end, so at this point the
                    // new row is already the last child. Re-pin s_act_line to
                    // the (new) last slot instead of moving the row to
                    // s_act_line's old index -- s_act_line is only ever
                    // created once (BuildActLine) and never otherwise moved,
                    // so its index is stale after the first turn; moving new
                    // rows there instead of the reverse used to insert every
                    // turn's content back at the top of the feed, above the
                    // user's own message.
                    lv_obj_t* row = CreateThinkRow(s_feed);
                    lv_obj_move_to_index(s_act_line, lv_obj_get_child_count(s_feed) - 1);
                    s_cur_think_row = row;
                }
                if (s_think_timer == nullptr) {
                    s_think_timer = lv_timer_create(ThinkTimerTick, 100, nullptr);
                }
                break;
            case UI_THINKING_END:
                if (s_think_timer != nullptr) {
                    lv_timer_delete(s_think_timer);
                    s_think_timer = nullptr;
                }
                break;
            case UI_TOOL_START: {
                if (!batched_text.empty()) {
                    AppendAssistantText(batched_text.c_str());
                    batched_text.clear();
                }
                FinalizeMdView();
                s_turn_tool_count++;
                const char* name = evt.s1 != nullptr ? evt.s1 : "tool";
                if (!s_zen) {
                    // Same fix as the thinking row above: pin s_act_line to
                    // the end instead of moving the card to s_act_line's
                    // stale index.
                    lv_obj_t* card = CreateToolCard(s_feed, name);
                    lv_obj_move_to_index(s_act_line, lv_obj_get_child_count(s_feed) - 1);
                    s_cur_tool_card = card;
                }
                lv_label_set_text(s_act_text, name);
                s_tool_start_ms = lv_tick_get();
                if (s_tool_running_timer == nullptr) {
                    s_tool_running_timer = lv_timer_create(ToolRunningTimerTick, 100, nullptr);
                }
                break;
            }
            case UI_TOOL_ARGS:
                if (!s_zen && s_cur_tool_body_args_lbl != nullptr && evt.s1 != nullptr) {
                    char buf[320];
                    std::snprintf(buf, sizeof(buf), "#5F5849 args#    %s", evt.s1);
                    lv_label_set_text(s_cur_tool_body_args_lbl, buf);
                }
                break;
            case UI_TOOL_END: {
                if (s_tool_running_timer != nullptr) {
                    lv_timer_delete(s_tool_running_timer);
                    s_tool_running_timer = nullptr;
                }
                const char* name = evt.s1 != nullptr ? evt.s1 : "tool";
                const char* output = evt.s2 != nullptr ? evt.s2 : "";
                if (!s_zen && s_cur_tool_dot != nullptr) {
                    StopBreath(s_cur_tool_dot);
                    lv_obj_set_style_bg_color(s_cur_tool_dot, lv_color_hex(kOk), LV_PART_MAIN);
                    char ret[128];
                    std::snprintf(ret, sizeof(ret), "%s \xc2\xb7 %s", output,
                                  FormatSecs1(evt.i1 / 1000.0f).c_str());
                    lv_label_set_text(s_cur_tool_ret_lbl, ret);
                    SetLabelFont(s_cur_tool_ret_lbl, &font_pi_mono_17, kOk);
                    // Card stays in the feed as history; only the RUNNING
                    // footer line goes away, not the whole body (args stay
                    // readable if the user has it expanded).
                    if (s_cur_tool_body_partial_row != nullptr) {
                        lv_obj_add_flag(s_cur_tool_body_partial_row, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                char act[160];
                std::snprintf(act, sizeof(act), "%s -> %s", name, output);
                lv_label_set_text(s_act_text, act);
                s_turn_last_tool_output = output;
                if (s_tool_cache.size() < kNvsNamespaceMaxTools) {
                    ToolCacheEntry tc;
                    tc.name = name;
                    tc.output = output;
                    tc.elapsed_ms = evt.i1;
                    s_tool_cache.push_back(tc);
                }
                s_cur_tool_card = s_cur_tool_dot = s_cur_tool_fn_lbl = nullptr;
                s_cur_tool_ret_lbl = s_cur_tool_body_args_lbl = s_cur_tool_body_partial_row = nullptr;
                break;
            }
            case UI_TEXT_DELTA:
                if (s_ttfb_ms < 0) s_ttfb_ms = static_cast<int32_t>(lv_tick_get() - s_turn_start_ms);
                if (evt.s1 != nullptr) batched_text += evt.s1;
                // Streaming approximation (per-turn i2 from TEXT_DELTA); DONE
                // corrects both ^ and v to the real pi_usage_t values. The
                // dock label itself is refreshed once per tick, after the
                // loop -- not per event.
                s_out_tokens = evt.i2;
                stat_dirty = true;
                break;
            case UI_DONE: {
                if (!batched_text.empty()) {
                    AppendAssistantText(batched_text.c_str());
                    batched_text.clear();
                }
                FinalizeMdView();
                ShowTalkBtn();
                s_zen_turn_done = true;
                StopBreath(s_act_dot);
                lv_obj_set_style_bg_color(s_act_dot, lv_color_hex(kOk), LV_PART_MAIN);
                float total_secs = (lv_tick_get() - s_turn_start_ms) / 1000.0f;
                char summary[128];
                const char* last_out =
                    s_turn_last_tool_output.empty() ? "" : s_turn_last_tool_output.c_str();
                if (s_turn_tool_count > 0) {
                    std::snprintf(summary, sizeof(summary), "%d tool \xc2\xb7 %s \xc2\xb7 %s",
                                  s_turn_tool_count, last_out, FormatSecs1(total_secs).c_str());
                } else {
                    std::snprintf(summary, sizeof(summary), "done \xc2\xb7 %s",
                                  FormatSecs1(total_secs).c_str());
                }
                lv_label_set_text(s_act_text, summary);
                if (s_turn_had_thinking || !s_tool_cache.empty()) {
                    lv_label_set_text(s_act_peek, "\xe6\x9f\xa5\xe7\x9c\x8b\xe8\xbf\x87\xe7\xa8\x8b >");
                }
                // DONE carries the real pi_usage_t for this run (bridge
                // contract: i1=usage.input, i2=usage.output) -- correct both
                // the dock stat and the CTX gauge from it instead of the
                // streaming approximation / a fabricated ratio.
                s_in_tokens = evt.i1;
                s_in_tokens_known = true;
                s_out_tokens = evt.i2;
                UpdateDockStat();
                ApplyCtxUsage(static_cast<uint32_t>(evt.i1));
                break;
            }
            case UI_ERROR: {
                // Not a modal -- reading can continue around it. Real
                // transports hit this path on genuine network/API failures,
                // so the banner's retry has to actually work (it resends the
                // prompt already recorded in s_last_user_prompt).
                ShowErrorBanner(evt.s1 != nullptr ? evt.s1 : "unknown error");

                if (s_tool_running_timer != nullptr) {
                    lv_timer_delete(s_tool_running_timer);
                    s_tool_running_timer = nullptr;
                }
                if (s_think_timer != nullptr) {
                    lv_timer_delete(s_think_timer);
                    s_think_timer = nullptr;
                }
                if (!batched_text.empty()) {
                    AppendAssistantText(batched_text.c_str());
                    batched_text.clear();
                }
                FinalizeMdView();
                ShowTalkBtn();
                break;
            }
        }
        if (evt.s1 != nullptr) free(evt.s1);
        if (evt.s2 != nullptr) free(evt.s2);
    }
    if (!batched_text.empty()) AppendAssistantText(batched_text.c_str());
    if (stat_dirty) UpdateDockStat();
    if (touched) {
        ScrollFeedToBottom();  // the tick's single forced layout pass...
        if (s_cur_md != nullptr) s_cur_md->SyncCursor();  // ...which cursor placement reuses
    }
}

// ---------------------------------------------------------------------------
// view/state machine
// ---------------------------------------------------------------------------
void Go(ViewState s) {
    s_state = s;
    lv_obj_add_flag(s_idle_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_listen_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_chat_view, LV_OBJ_FLAG_HIDDEN);
    switch (s) {
        case ViewState::Idle:
            lv_obj_remove_flag(s_idle_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ptt_layer, LV_OBJ_FLAG_HIDDEN);
            break;
        case ViewState::Listen:
            lv_obj_remove_flag(s_listen_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ptt_layer, LV_OBJ_FLAG_HIDDEN);
            break;
        case ViewState::Chat:
            lv_obj_remove_flag(s_chat_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ptt_layer, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

int Utf8CodepointCount(const char* s) {
    int n = 0;
    while (*s != '\0') {
        unsigned char c = static_cast<unsigned char>(*s);
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        s += len;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// S1 real-ASR session engine (volc_asr + mhal::audio_pipeline capture).
//
// Thread model: volc_asr's callbacks fire on its WebSocket task and only
// write the mutex-guarded shared strings/flags below; the LVGL side polls
// them from AsrTick (the old fake-reveal timer slot, 70ms). All blocking
// volc calls (TLS connect in start, final wait in stop, socket close in
// abort) run on a dedicated voice-control task fed by a command queue, so
// the LVGL thread never blocks and start/finish/cancel stay serialized even
// if the user releases before the connection handshake finished.
// ---------------------------------------------------------------------------
SemaphoreHandle_t s_asr_mutex = nullptr;
std::string s_asr_live_text;   // guarded by s_asr_mutex（服务端全量文本）
std::string s_asr_error_text;  // guarded by s_asr_mutex
volatile bool s_asr_final_ready = false;
volatile bool s_asr_failed = false;
bool s_asr_waiting_final = false;  // LVGL 线程：已松开，等服务端 final
bool s_key_heard_speech = false;   // key 进入的聆听：VAD 自动收音的触发臂
std::string s_asr_rendered;        // 上次渲染的文本（LVGL 线程，去重用）

void AsrLock() { xSemaphoreTake(s_asr_mutex, portMAX_DELAY); }
void AsrUnlock() { xSemaphoreGive(s_asr_mutex); }

void OnAsrDelta(const char* text, void*) {
    AsrLock();
    s_asr_live_text = text;
    AsrUnlock();
}

void OnAsrFinal(const char* text, void*) {
    AsrLock();
    s_asr_live_text = text;
    AsrUnlock();
    s_asr_final_ready = true;
}

void OnAsrError(int code, const char* msg, void*) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ASR %d: %s", code, msg != nullptr ? msg : "");
    AsrLock();
    s_asr_error_text = buf;
    AsrUnlock();
    s_asr_failed = true;
}

void PostAsrFailure(const char* msg) {
    AsrLock();
    if (s_asr_error_text.empty()) s_asr_error_text = msg;
    AsrUnlock();
    s_asr_failed = true;
}

enum class VoiceCmd { Start, Finish, Cancel };
QueueHandle_t s_voice_q = nullptr;

void VoiceTask(void*) {
    bool active = false;
    for (;;) {
        VoiceCmd cmd;
        if (xQueueReceive(s_voice_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd) {
            case VoiceCmd::Start: {
                if (active) break;
                volc_tts_stop();  // barge-in：用户开口即打断在播 TTS
                volc_asr_callbacks_t cbs = {};
                cbs.on_delta = OnAsrDelta;
                cbs.on_final = OnAsrFinal;
                cbs.on_error = OnAsrError;
                esp_err_t err = volc_asr_start(&cbs);
                if (err == ESP_OK) {
                    mhal::audio_pipeline::CaptureConfig cfg;  // 20ms 帧 + 能量 VAD
                    mhal::audio_pipeline::CaptureCallbacks ccb;
                    ccb.on_frame = [](const int16_t* pcm, size_t n) {
                        volc_asr_feed(pcm, n);
                    };
                    if (!mhal::audio_pipeline::StartCapture(cfg, ccb)) {
                        volc_asr_abort();
                        err = ESP_FAIL;
                    }
                }
                if (err != ESP_OK) {
                    PostAsrFailure("ASR connect failed (network / keys?)");
                } else {
                    active = true;
                }
                break;
            }
            case VoiceCmd::Finish: {
                if (!active) break;
                active = false;
                mhal::audio_pipeline::StopCapture();
                esp_err_t err = volc_asr_stop(12000);  // final 经 OnAsrFinal 落地
                if (err != ESP_OK && !s_asr_final_ready && !s_asr_failed) {
                    PostAsrFailure("ASR timed out waiting for final");
                }
                break;
            }
            case VoiceCmd::Cancel:
                if (active) {
                    active = false;
                    mhal::audio_pipeline::StopCapture();
                    volc_asr_abort();
                }
                break;
        }
    }
}

// 惰性建 voice 基建；采集/发送路径要跑 gzip + TLS 写，栈给足。
void EnsureVoiceInfra() {
    if (s_voice_q != nullptr) return;
    s_asr_mutex = xSemaphoreCreateMutex();
    s_voice_q = xQueueCreate(8, sizeof(VoiceCmd));
    xTaskCreate(VoiceTask, "pi_voice", 6144, nullptr, 5, nullptr);
}

void VoiceSend(VoiceCmd cmd) {
    EnsureVoiceInfra();
    xQueueSend(s_voice_q, &cmd, 0);
}

void StopListenTimers() {
    if (s_asr_timer != nullptr) { lv_timer_delete(s_asr_timer); s_asr_timer = nullptr; }
    if (s_rec_timer != nullptr) { lv_timer_delete(s_rec_timer); s_rec_timer = nullptr; }
}

void HandleAsrFinal() {
    std::string text;
    AsrLock();
    text = s_asr_live_text;
    AsrUnlock();
    s_asr_final_ready = false;
    s_asr_waiting_final = false;
    StopListenTimers();
    if (text.empty()) {  // 没说话/没听清：安静回到来处，不发空 prompt
        Go(s_return_state);
        return;
    }
    Go(ViewState::Chat);
    AppendUserRow(text);
    ResetTurnState();
    ScrollFeedToBottom();
    s_last_user_prompt = text;
    pi_agent_task_send_prompt(text.c_str());
}

void HandleAsrFailure() {
    std::string msg;
    AsrLock();
    msg = s_asr_error_text;
    AsrUnlock();
    s_asr_failed = false;
    s_asr_waiting_final = false;
    StopListenTimers();
    VoiceSend(VoiceCmd::Cancel);  // 兜底清理采集/半开会话（无会话时是 no-op）
    Go(ViewState::Chat);          // 错误横幅住在 chat feed 里
    ShowErrorBanner(msg.empty() ? "ASR error" : msg.c_str());
    ShowTalkBtn();
    ScrollFeedToBottom();
}

void AsrTick(lv_timer_t*) {
    if (s_asr_lbl == nullptr) return;
    if (s_asr_failed) {
        HandleAsrFailure();
        return;
    }
    if (s_asr_final_ready) {
        HandleAsrFinal();
        return;
    }

    std::string text;
    AsrLock();
    text = s_asr_live_text;
    AsrUnlock();
    if (text != s_asr_rendered) {
        s_asr_rendered = text;
        // 尾部 6 个码点琥珀高亮：沿用假走带的 ".cur" 视觉，数据源换成真 delta
        int total_cp = Utf8CodepointCount(text.c_str());
        int hi_start = total_cp - kAsrHighlightCodepoints;
        if (hi_start < 0) hi_start = 0;
        int plain_bytes = Utf8PrefixBytes(text.c_str(), hi_start);
        std::string markup(text, 0, plain_bytes);
        markup += "#FFAE1F ";
        markup.append(text, plain_bytes, std::string::npos);
        markup += "#";
        lv_label_set_text(s_asr_lbl, markup.c_str());
    }

    // PWR_KEY 进入的聆听没有"松开"信号（原实现是假走带播完自动发送）：改为
    // VAD 听到过人声、又回到静音（600ms 挂起）后自动收音发送；二次按键仍是
    // 取消（OnKeyClickAsync 的 Listen 分支不变）。
    if (s_ptt_via_key && !s_asr_waiting_final) {
        if (mhal::audio_pipeline::IsVoiceDetected()) {
            s_key_heard_speech = true;
        } else if (s_key_heard_speech) {
            s_ptt_via_key = false;
            FinishListenSend();
        }
    }
}

void StartListen(ViewState return_state) {
    s_return_state = return_state;
    s_ptt_via_key = false;
    s_key_heard_speech = false;
    s_asr_waiting_final = false;
    s_asr_final_ready = false;
    s_asr_failed = false;
    s_asr_rendered.clear();
    EnsureVoiceInfra();
    AsrLock();
    s_asr_live_text.clear();
    s_asr_error_text.clear();
    AsrUnlock();
    VoiceSend(VoiceCmd::Start);  // 建连+开采集都在 voice 任务，UI 不阻塞
    if (s_asr_lbl != nullptr) lv_label_set_text(s_asr_lbl, "");
    s_rec_secs = 0;
    if (s_rec_lbl != nullptr) lv_label_set_text(s_rec_lbl, "REC 0:00");
    Go(ViewState::Listen);
    if (s_asr_timer == nullptr) s_asr_timer = lv_timer_create(AsrTick, 70, nullptr);
    if (s_rec_timer == nullptr) s_rec_timer = lv_timer_create(UpdateRecTimer, 1000, nullptr);
}

void CancelListen() {
    StopListenTimers();
    s_asr_waiting_final = false;
    s_ptt_via_key = false;
    VoiceSend(VoiceCmd::Cancel);
    Go(s_return_state);
}

void FinishListenSend() {
    if (s_asr_waiting_final) return;  // 重复松开/自动发送竞态：只收一次
    s_asr_waiting_final = true;
    s_ptt_via_key = false;
    // 停采集与等 final 都在 voice 任务里做；listen 视图原地保留（REC 停表），
    // AsrTick 继续轮询，final/失败落地后再切 chat / 出横幅。
    if (s_rec_timer != nullptr) { lv_timer_delete(s_rec_timer); s_rec_timer = nullptr; }
    VoiceSend(VoiceCmd::Finish);
}

// Error-banner retry: resend the same prompt without re-recording it (the
// user row is already in the feed) or replaying the PTT flow. Real
// (non-mock) transports actually hit UI_ERROR on network failures, so this
// has to work, not just look clickable.
void RetryLastPrompt() {
    if (s_last_user_prompt.empty()) return;
    ResetTurnState();
    ScrollFeedToBottom();
    pi_agent_task_send_prompt(s_last_user_prompt.c_str());
}

void NewSession() {
    if (s_state == ViewState::Listen) CancelListen();
    pi_agent_task_new_session();
    if (s_feed != nullptr) {
        int32_t n = static_cast<int32_t>(lv_obj_get_child_count(s_feed));
        for (int32_t i = n - 1; i >= 0; --i) {
            lv_obj_t* child = lv_obj_get_child(s_feed, i);
            if (child == s_act_line) continue;  // permanent, reused across turns
            lv_obj_delete(child);
        }
    }
    ResetTurnState();
    ApplyCtxUnknown();
    Go(ViewState::Idle);
}

void KeyStartListen(ViewState return_state) {
    StartListen(return_state);
    s_ptt_via_key = true;
}

void OnKeyClickAsync(void*) {
    switch (s_state) {
        case ViewState::Idle:
            KeyStartListen(ViewState::Idle);
            break;
        case ViewState::Listen:
            CancelListen();
            break;
        case ViewState::Chat:
            if (s_stop_btn != nullptr && !lv_obj_has_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN)) {
                pi_agent_task_abort();
            } else {
                KeyStartListen(ViewState::Chat);
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// screen teardown (widgets + timers + queue). IOExpander offClick lives in
// LifecycleCallback() below, matching the screen_register_pwr_key_toggle_*
// convention (register on LOAD, unregister on UNLOAD); everything tied to
// the widget tree/timers/queue is torn down here instead, on
// LV_EVENT_SCREEN_UNLOADED, while the tree is still guaranteed valid
// (screen_util.h's documented UNLOAD contract).
// ---------------------------------------------------------------------------
void OnScreenUnloaded(lv_event_t*) {
    pi_agent_task_abort();  // 也会异步打断 TTS 播报
    if (s_voice_q != nullptr) VoiceSend(VoiceCmd::Cancel);  // 停采集/ASR 会话
    if (s_clock_timer != nullptr) { lv_timer_delete(s_clock_timer); s_clock_timer = nullptr; }
    if (s_asr_timer != nullptr) { lv_timer_delete(s_asr_timer); s_asr_timer = nullptr; }
    if (s_rec_timer != nullptr) { lv_timer_delete(s_rec_timer); s_rec_timer = nullptr; }
    if (s_think_timer != nullptr) { lv_timer_delete(s_think_timer); s_think_timer = nullptr; }
    if (s_tool_running_timer != nullptr) {
        lv_timer_delete(s_tool_running_timer);
        s_tool_running_timer = nullptr;
    }
    if (s_cursor_blink_timer != nullptr) {
        lv_timer_delete(s_cursor_blink_timer);
        s_cursor_blink_timer = nullptr;
    }
    if (s_drain_timer != nullptr) { lv_timer_delete(s_drain_timer); s_drain_timer = nullptr; }

    // Drain and free anything still in flight so agent-thread mallocs never
    // leak just because the screen went away mid-turn.
    pi_ui_evt_t evt;
    while (xQueueReceive(pi_ui_queue(), &evt, 0) == pdTRUE) {
        if (evt.s1 != nullptr) free(evt.s1);
        if (evt.s2 != nullptr) free(evt.s2);
    }

    s_scr = s_idle_view = s_listen_view = s_chat_view = s_ptt_layer = nullptr;
    s_feed = s_act_line = s_act_dot = s_act_text = s_act_peek = s_peek_container = nullptr;
    s_clock_lbl = s_date_lbl = s_idle_breath = s_idle_ctx_fill = s_idle_ctx_lbl = nullptr;
    s_idle_model_lbl = s_chat_model_lbl = nullptr;
    s_wave_row = s_asr_lbl = s_rec_lbl = nullptr;
    s_chat_ctx_fill = s_chat_ctx_lbl = nullptr;
    s_mode_icon_flow = s_mode_icon_zen = s_mode_lbl = s_tts_dot = nullptr;
    s_dock_stat_lbl = s_dock_action_box = s_stop_btn = s_talk_btn = nullptr;
    s_cur_think_row = s_cur_think_dot = s_cur_think_lbl = nullptr;
    s_cur_tool_card = s_cur_tool_dot = s_cur_tool_fn_lbl = nullptr;
    s_cur_tool_ret_lbl = s_cur_tool_body_args_lbl = s_cur_tool_body_partial_row = nullptr;
    s_cur_md = nullptr;  // the MdView frees itself when the LVGL tree is deleted
    s_wave_bars.clear();
    s_tool_cache.clear();
}

}  // namespace

lv_obj_t* PiScreen::Create() {
    Settings settings("pi_screen", false);
    s_zen = settings.GetBool("zen_mode", false);
    s_tts_on = settings.GetBool("tts_on", true);
    pi_agent_tts_set_enabled(s_tts_on);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kW, kH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildIdleView(scr);
    BuildListenView(scr);
    BuildChatView(scr);
    ApplyModeVisual();
    ApplyTtsVisual();
    ApplyCtxUnknown();  // no real usage.input yet -> "CTX --", not a guess
    UpdateDockStat();

    // Persistent PTT touch target, y:[kSbarH, kH). Built last so it sits on
    // top of the idle/listen content in z-order; hidden while in Chat so
    // the feed's scroll/tap and the dock buttons receive touches directly.
    // Deliberately NOT tagged screen_swipe_back_ignore(): a clear rightward
    // swipe across it should still exit to the home menu (a plain
    // press-and-hold in place has dx~0 and never crosses the swipe-back
    // threshold, so the two gestures don't collide in practice).
    s_ptt_layer = lv_obj_create(scr);
    screen_strip_obj_chrome(s_ptt_layer);
    lv_obj_remove_flag(s_ptt_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ptt_layer, kW, kH - kSbarH);
    lv_obj_set_pos(s_ptt_layer, 0, kSbarH);
    lv_obj_set_style_bg_opa(s_ptt_layer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_ptt_layer, LV_OBJ_FLAG_CLICKABLE);
    void* ret_idle = reinterpret_cast<void*>(static_cast<intptr_t>(ViewState::Idle));
    lv_obj_add_event_cb(s_ptt_layer, OnPttPressed, LV_EVENT_PRESSED, ret_idle);
    lv_obj_add_event_cb(s_ptt_layer, OnPttPressing, LV_EVENT_PRESSING, ret_idle);
    lv_obj_add_event_cb(s_ptt_layer, OnPttReleased, LV_EVENT_RELEASED, ret_idle);

    Go(ViewState::Idle);

    s_drain_timer = lv_timer_create(DrainQueueTick, 80, nullptr);
    s_cursor_blink_timer = lv_timer_create(CursorBlinkTick, 500, nullptr);

    // 单 App 固件：无 home 菜单可返回，右滑返回手势不再挂接。
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void PiScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: pi_screen");
        pi_agent_task_start();
        UpdateModelLabels();
        IOExpander::getInstance().onClick(IOExpander::Pin::PWR_KEY, []() {
            lv_async_call(OnKeyClickAsync, nullptr);
        });
    } else {
        ESP_LOGI(TAG, "unload: pi_screen");
        IOExpander::getInstance().offClick(IOExpander::Pin::PWR_KEY);
    }
}
