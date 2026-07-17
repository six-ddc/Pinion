#include "pi_media.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "media_player/media_player.h"
#include "pi_fonts.h"
#include "pi_theme.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// 见头文件。设计语言严格延续 pi_settings / pi_quick_panel：Bg 深底、Card 面 +
// 1px Line 边、唯一琥珀强调、mono 大写小字 caption、大圆角、克制留白。全程只用
// pi_theme 令牌取色（唯一例外：透明度、canvas 图元里落到令牌颜色时的一次性取色）。
// ---------------------------------------------------------------------------
namespace {

using media::MediaController;
using media::MediaItem;
using media::MediaState;
using pi_theme::Tok;

constexpr int32_t kW = 720;
constexpr int32_t kH = 720;
constexpr int32_t kSbarH = 56;
constexpr int32_t kBandH = 112;  // 底部提示/输入带高度（与 pi_screen kHintH/kDockH 同）

// ---- canvas 图元（填充三角形；主题切换经令牌重绘） ------------------------
// Bar/Circle/Arc 类线条图元用 lv_obj + Tok 共享样式即可（自动随主题翻转），只有
// 填充三角形（播放/上一首/下一首）需要 canvas；它们的颜色令牌记入 s_glyphs，
// 主题回调里重绘。canvas buffer 随 canvas 删除时在 DELETE 事件里 free。
struct Glyph {
    lv_obj_t* canvas;
    int32_t size;
    int dir;  // 0=右（play/next），1=左（prev）
    Tok tok;
};
std::vector<Glyph> s_glyphs;
int s_theme_listener = -1;

void DrawTri(lv_obj_t* canvas, int32_t s, int dir, Tok tok) {
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = pi_theme::Color(tok);
    dsc.opa = LV_OPA_COVER;
    const lv_value_precise_t a = s * 24 / 100;   // 近边
    const lv_value_precise_t b = s * 78 / 100;   // 远边
    const lv_value_precise_t t = s * 22 / 100;   // 顶
    const lv_value_precise_t d = s * 78 / 100;   // 底
    const lv_value_precise_t m = s * 50 / 100;   // 尖端纵向中点
    if (dir == 0) {  // 右指
        dsc.p[0].x = a; dsc.p[0].y = t;
        dsc.p[1].x = a; dsc.p[1].y = d;
        dsc.p[2].x = b; dsc.p[2].y = m;
    } else {  // 左指
        dsc.p[0].x = b; dsc.p[0].y = t;
        dsc.p[1].x = b; dsc.p[1].y = d;
        dsc.p[2].x = a; dsc.p[2].y = m;
    }
    lv_draw_triangle(&layer, &dsc);
    lv_canvas_finish_layer(canvas, &layer);
}

void OnGlyphDeleted(lv_event_t* e) {
    lv_obj_t* canvas = lv_event_get_target_obj(e);
    void* buf = lv_event_get_user_data(e);
    for (size_t i = 0; i < s_glyphs.size(); i++) {
        if (s_glyphs[i].canvas == canvas) {
            s_glyphs.erase(s_glyphs.begin() + i);
            break;
        }
    }
    free(buf);
}

lv_obj_t* MakeTri(lv_obj_t* parent, int32_t size, int dir, Tok tok) {
    lv_obj_t* cv = lv_canvas_create(parent);
    screen_strip_obj_chrome(cv);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    size_t buf_size = 4u * (size + LV_DRAW_BUF_STRIDE_ALIGN - 1) * size + LV_DRAW_BUF_ALIGN;
    void* buf = malloc(buf_size);
    lv_canvas_set_buffer(cv, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_event_cb(cv, OnGlyphDeleted, LV_EVENT_DELETE, buf);
    s_glyphs.push_back({cv, size, dir, tok});
    DrawTri(cv, size, dir, tok);
    return cv;
}

void RedrawGlyphs() {
    for (const Glyph& g : s_glyphs) DrawTri(g.canvas, g.size, g.dir, g.tok);
}

// ---- 线条图元（Tok 共享样式，主题自动翻转） -------------------------------
lv_obj_t* Bar(lv_obj_t* parent, int32_t w, int32_t h, Tok tok, int32_t dx, int32_t dy) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, (w < h ? w : h) / 2, LV_PART_MAIN);
    pi_theme::ApplyBg(o, tok);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
    return o;
}

lv_obj_t* Circle(lv_obj_t* parent, int32_t d, Tok tok, int32_t dx, int32_t dy, lv_opa_t opa) {
    lv_obj_t* o = Bar(parent, d, d, tok, dx, dy);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
    return o;
}

// 圆角方块（art 底与内层辉光；opa 控层次）。
lv_obj_t* RoundBox(lv_obj_t* parent, int32_t w, int32_t h, int32_t radius, Tok tok, lv_opa_t opa) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    pi_theme::ApplyBg(o, tok);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
    lv_obj_center(o);
    return o;
}

// 一段弧（电波母题用），一次性取色（随 art 整体重建覆盖主题切换）。
lv_obj_t* ArcSeg(lv_obj_t* box, int32_t d, Tok tok, int32_t bw, int32_t s_deg, int32_t e_deg) {
    lv_obj_t* a = lv_arc_create(box);
    lv_obj_add_flag(a, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(a, d, d);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, s_deg, e_deg);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_width(a, bw, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, pi_theme::Color(tok), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
    lv_obj_center(a);
    return a;
}

lv_obj_t* Label(lv_obj_t* parent, const char* txt, const lv_font_t* font, Tok tok) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    pi_theme::ApplyText(l, tok);
    return l;
}

void FmtTime(int secs, char* out, size_t n) {
    if (secs < 0) secs = 0;
    std::snprintf(out, n, "%d:%02d", secs / 60, secs % 60);
}

// ---------------------------------------------------------------------------
// mini 播放条（常驻，隐藏态；state != stopped 时浮现）
// ---------------------------------------------------------------------------
lv_obj_t* s_parent = nullptr;    // pi_screen 的 screen 对象
lv_obj_t* s_mini = nullptr;      // mini 条根（Card）
lv_obj_t* s_mini_title = nullptr;
lv_obj_t* s_mini_sub = nullptr;
lv_obj_t* s_mini_btn = nullptr;      // 右侧 play-pause 触区
lv_obj_t* s_mini_glyph = nullptr;    // 该触区内的图元容器
lv_obj_t* s_mini_prog = nullptr;     // 底部 2px 进度线
bool s_mini_ctx = true;              // 当前视图是否允许显示（Go 设置）
bool s_mini_shown = false;           // 当前实际可见（用于淡入触发一次）

constexpr int32_t kMiniH = 60;
constexpr int32_t kMiniMargin = 16;
constexpr int32_t kMiniInnerW = kW - 2 * kMiniMargin;

lv_timer_t* s_timer = nullptr;

void OnMiniToggle(lv_event_t*) { MediaController::Instance().Toggle(); }
void OnMiniBody(lv_event_t*) { pi_media::Open(); }

// play/pause 图元：clear host 后按 playing 重建（play=填充三角，pause=双竖条）。
void SetGlyph(lv_obj_t* host, bool playing, Tok tok, int32_t s) {
    lv_obj_clean(host);
    if (playing) {
        Bar(host, s * 16 / 100, s * 56 / 100, tok, -s * 11 / 100, 0);
        Bar(host, s * 16 / 100, s * 56 / 100, tok, s * 11 / 100, 0);
    } else {
        lv_obj_t* tri = MakeTri(host, s, 0, tok);
        lv_obj_align(tri, LV_ALIGN_CENTER, s * 6 / 100, 0);  // 视觉重心右移
    }
}

void CreateMiniBarImpl(lv_obj_t* parent) {
    s_mini = lv_obj_create(parent);
    screen_strip_obj_chrome(s_mini);
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_mini, kMiniInnerW, kMiniH);
    lv_obj_set_pos(s_mini, kMiniMargin, kH - kBandH - kMiniH - 10);
    lv_obj_set_style_radius(s_mini, 16, LV_PART_MAIN);
    pi_theme::ApplyBg(s_mini, Tok::Card);
    lv_obj_set_style_bg_opa(s_mini, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_mini, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(s_mini, Tok::Line);
    lv_obj_set_style_clip_corner(s_mini, true, LV_PART_MAIN);  // 底部进度线不越圆角
    lv_obj_add_flag(s_mini, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mini, OnMiniBody, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);

    // 左：40px 圆形小 art + 迷你音符
    lv_obj_t* art = lv_obj_create(s_mini);
    screen_strip_obj_chrome(art);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(art, 40, 40);
    lv_obj_set_pos(art, 14, (kMiniH - 40) / 2);
    lv_obj_set_style_radius(art, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    pi_theme::ApplyBg(art, Tok::AccentDim);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, LV_PART_MAIN);
    Circle(art, 12, Tok::Bg, -4, 6, LV_OPA_COVER);            // 音符头
    Bar(art, 3, 18, Tok::Bg, 3, -1);                          // 符干

    // 中：title + subtitle（两行紧凑，flex 列，占中段）
    lv_obj_t* col = lv_obj_create(s_mini);
    screen_strip_obj_chrome(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, kMiniInnerW - 40 - 14 - 12 - 44 - 16 - 14, kMiniH);
    lv_obj_set_pos(col, 14 + 40 + 12, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 2, LV_PART_MAIN);
    s_mini_title = Label(col, "--", &font_puhui_20_4, Tok::Tx);
    lv_label_set_long_mode(s_mini_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_mini_title, LV_PCT(100));
    s_mini_sub = Label(col, "", &font_puhui_20_4, Tok::Dim);
    lv_label_set_long_mode(s_mini_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_mini_sub, LV_PCT(100));

    // 右：44px play-pause
    s_mini_btn = lv_obj_create(s_mini);
    screen_strip_obj_chrome(s_mini_btn);
    lv_obj_remove_flag(s_mini_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_mini_btn, 44, 44);
    lv_obj_set_pos(s_mini_btn, kMiniInnerW - 44 - 12, (kMiniH - 44) / 2);
    lv_obj_set_style_bg_opa(s_mini_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_mini_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mini_btn, 10);
    lv_obj_add_event_cb(s_mini_btn, OnMiniToggle, LV_EVENT_CLICKED, nullptr);
    s_mini_glyph = lv_obj_create(s_mini_btn);
    screen_strip_obj_chrome(s_mini_glyph);
    lv_obj_remove_flag(s_mini_glyph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_mini_glyph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_mini_glyph, 30, 30);
    lv_obj_center(s_mini_glyph);
    lv_obj_set_style_bg_opa(s_mini_glyph, LV_OPA_TRANSP, LV_PART_MAIN);
    SetGlyph(s_mini_glyph, false, Tok::Dim, 30);

    // 底部 2px accent 进度线（clip_corner 保证不越圆角）
    s_mini_prog = lv_obj_create(s_mini);
    screen_strip_obj_chrome(s_mini_prog);
    lv_obj_remove_flag(s_mini_prog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_mini_prog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_mini_prog, 0, 2);
    lv_obj_align(s_mini_prog, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    pi_theme::ApplyBg(s_mini_prog, Tok::Accent);
    lv_obj_set_style_bg_opa(s_mini_prog, LV_OPA_COVER, LV_PART_MAIN);
}

// mini 条淡入
void MiniFadeIn() {
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_mini, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mini);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(o), static_cast<lv_opa_t>(v), LV_PART_MAIN);
    });
    lv_anim_start(&a);
}

// mini 条状态缓存（避免每秒重设标题）
struct MiniCache {
    int state = -1;
    std::string title;
} s_mini_cache;

void RefreshMini() {
    if (s_mini == nullptr) return;
    MediaController& mc = MediaController::Instance();
    MediaState st = mc.state();
    bool want = s_mini_ctx && st != MediaState::Stopped;
    if (!want) {
        if (s_mini_shown) {
            lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
            s_mini_shown = false;
        }
        return;
    }
    if (!s_mini_shown) {
        MiniFadeIn();
        s_mini_shown = true;
    }

    MediaItem cur = mc.current();
    if (cur.title != s_mini_cache.title) {
        s_mini_cache.title = cur.title;
        lv_label_set_text(s_mini_title, cur.title.empty() ? "--" : cur.title.c_str());
    }
    lv_label_set_text(s_mini_sub, cur.subtitle.c_str());

    bool playing = st == MediaState::Playing;
    if (static_cast<int>(st) != s_mini_cache.state) {
        s_mini_cache.state = static_cast<int>(st);
        SetGlyph(s_mini_glyph, playing, playing ? Tok::Accent : Tok::Dim, 30);
    }

    // 进度线：文件按 pct，直播（duration 0）隐藏
    int dur = mc.duration_s();
    if (cur.is_stream || dur <= 0) {
        lv_obj_set_width(s_mini_prog, 0);
    } else {
        int pct = mc.position_s() * 100 / dur;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_obj_set_width(s_mini_prog, kMiniInnerW * pct / 100);
    }
}

// ---------------------------------------------------------------------------
// 全屏 Now-Playing 页
// ---------------------------------------------------------------------------
lv_obj_t* s_root = nullptr;
lv_obj_t* s_eyebrow = nullptr;
lv_obj_t* s_art_host = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_sub = nullptr;
lv_obj_t* s_prog_track = nullptr;
lv_obj_t* s_prog_fill = nullptr;
lv_obj_t* s_time_cur = nullptr;
lv_obj_t* s_time_dur = nullptr;
lv_obj_t* s_play_host = nullptr;  // 88 圆内的图元容器
lv_obj_t* s_drawer_root = nullptr;
lv_obj_t* s_drawer_list = nullptr;
std::vector<lv_obj_t*> s_drawer_rows;

constexpr int32_t kArt = 330;
constexpr int32_t kProgW = kW - 96;

struct PageCache {
    int index = -2;
    int state = -1;
    bool is_stream = false;
    std::string title;
} s_page_cache;

void BuildArt(bool is_stream, bool playing) {
    lv_obj_clean(s_art_host);
    if (is_stream) {
        // 电波母题：三段同心弧（Accent/AccentDim/Faint）+ 中心圆点。
        lv_obj_t* waves = lv_obj_create(s_art_host);
        screen_strip_obj_chrome(waves);
        lv_obj_remove_flag(waves, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(waves, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(waves, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(waves, kArt, kArt);
        lv_obj_set_style_bg_opa(waves, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_center(waves);
        ArcSeg(waves, 110, Tok::Accent, 5, 210, 330);
        ArcSeg(waves, 180, Tok::AccentDim, 5, 210, 330);
        ArcSeg(waves, 250, Tok::Faint, 5, 210, 330);
        Circle(waves, 22, Tok::Accent, 0, -20, LV_OPA_COVER);
        if (playing) {  // 呼吸：整组电波 2s 透明度脉动
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, waves);
            lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
            lv_anim_set_duration(&a, 1400);
            lv_anim_set_playback_duration(&a, 1400);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(o), static_cast<lv_opa_t>(v),
                                     LV_PART_MAIN);
            });
            lv_anim_start(&a);
        }
    } else {
        // 音乐母题：两层琥珀辉光方块 + 中心音符。
        RoundBox(s_art_host, 220, 220, 44, Tok::Accent, LV_OPA_20);
        RoundBox(s_art_host, 150, 150, 32, Tok::Accent, LV_OPA_30);
        // 音符：符头 + 符干 + 符尾。
        lv_obj_t* note = lv_obj_create(s_art_host);
        screen_strip_obj_chrome(note);
        lv_obj_remove_flag(note, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(note, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(note, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(note, 120, 140);
        lv_obj_set_style_bg_opa(note, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_center(note);
        Circle(note, 46, Tok::Accent, -22, 44, LV_OPA_COVER);  // 符头
        Bar(note, 7, 128, Tok::Accent, 22, -6);                // 符干
        Bar(note, 30, 8, Tok::Accent, 40, -66);                // 符尾
    }
}

void SetPlayGlyph(bool playing) { SetGlyph(s_play_host, playing, Tok::Bg, 44); }

void CloseDrawer() {
    if (s_drawer_root != nullptr) {
        lv_obj_delete(s_drawer_root);
        s_drawer_root = nullptr;
        s_drawer_list = nullptr;
        s_drawer_rows.clear();
    }
}

void OnDrawerRow(lv_event_t* e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    MediaController::Instance().PlayIndex(static_cast<int>(idx));
    CloseDrawer();
}

void BuildDrawer() {
    if (s_root == nullptr || s_drawer_root != nullptr) return;
    MediaController& mc = MediaController::Instance();
    int n = mc.playlist_size();
    if (n <= 0) return;

    s_drawer_root = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_drawer_root);
    lv_obj_remove_flag(s_drawer_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_drawer_root, kW, kH);
    lv_obj_set_pos(s_drawer_root, 0, 0);
    lv_obj_set_style_bg_opa(s_drawer_root, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t* scrim = lv_obj_create(s_drawer_root);
    screen_strip_obj_chrome(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scrim, kW, kH);
    pi_theme::ApplyScrim(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, [](lv_event_t*) { CloseDrawer(); }, LV_EVENT_CLICKED, nullptr);

    const int32_t sheet_h = kH * 55 / 100;
    lv_obj_t* sheet = lv_obj_create(s_drawer_root);
    screen_strip_obj_chrome(sheet);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sheet, kW, sheet_h);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(sheet, 24, LV_PART_MAIN);
    lv_obj_set_style_border_side(sheet, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    pi_theme::ApplyBg(sheet, Tok::Card);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sheet, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(sheet, Tok::Line);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);  // 挡住透传到 scrim

    lv_obj_t* cap = Label(sheet, "PLAYLIST", &font_pi_mono_17, Tok::Faint);
    lv_obj_set_style_text_letter_space(cap, 2, LV_PART_MAIN);
    lv_obj_set_pos(cap, 28, 22);

    s_drawer_list = lv_obj_create(sheet);
    screen_strip_obj_chrome(s_drawer_list);
    lv_obj_set_size(s_drawer_list, kW, sheet_h - 60);
    lv_obj_set_pos(s_drawer_list, 0, 56);
    lv_obj_set_style_bg_opa(s_drawer_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_drawer_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_drawer_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_drawer_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_drawer_list, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_drawer_list, 2, LV_PART_MAIN);

    int cur_idx = mc.index();
    char tbuf[16];
    for (int i = 0; i < n; i++) {
        MediaItem it = mc.item_at(i);
        bool active = (i == cur_idx);
        lv_obj_t* row = lv_obj_create(s_drawer_list);
        screen_strip_obj_chrome(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_PCT(100), 72);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        pi_theme::ApplyBg(row, Tok::Card2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnDrawerRow, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        s_drawer_rows.push_back(row);

        if (active) {  // 4px accent 左边条
            lv_obj_t* edge = lv_obj_create(row);
            screen_strip_obj_chrome(edge);
            lv_obj_remove_flag(edge, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(edge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(edge, 4, 40);
            lv_obj_set_style_radius(edge, 2, LV_PART_MAIN);
            pi_theme::ApplyBg(edge, Tok::Accent);
            lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(edge, LV_ALIGN_LEFT_MID, 4, 0);
        }

        std::snprintf(tbuf, sizeof(tbuf), "%02d", i + 1);
        lv_obj_t* num = Label(row, tbuf, &font_pi_mono_17, active ? Tok::Accent : Tok::Faint);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t* title = Label(row, it.title.empty() ? "--" : it.title.c_str(), &font_puhui_20_4,
                                active ? Tok::Accent : Tok::Tx);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, kW - 40 - 56 - 80);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 64, 0);

        if (it.is_stream) {
            std::snprintf(tbuf, sizeof(tbuf), "LIVE");
        } else if (it.duration_s <= 0) {
            std::snprintf(tbuf, sizeof(tbuf), "--:--");
        } else {
            FmtTime(it.duration_s, tbuf, sizeof(tbuf));
        }
        lv_obj_t* dur = Label(row, tbuf, &font_pi_mono_14, it.is_stream ? Tok::Accent : Tok::Dim);
        lv_obj_align(dur, LV_ALIGN_RIGHT_MID, -22, 0);
    }
}

void OnListBtn(lv_event_t*) {
    if (s_drawer_root != nullptr)
        CloseDrawer();
    else
        BuildDrawer();
}
void OnBackBtn(lv_event_t*) { pi_media::Back(); }
void OnPrev(lv_event_t*) { MediaController::Instance().Prev(); }
void OnNext(lv_event_t*) { MediaController::Instance().Next(); }
void OnPlay(lv_event_t*) { MediaController::Instance().Toggle(); }

// 顶栏图元小钮（back / list），48px 触区、透明底。
lv_obj_t* MakeTopBtn(lv_obj_t* parent, lv_event_cb_t cb, int32_t x) {
    lv_obj_t* b = lv_obj_create(parent);
    screen_strip_obj_chrome(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_pos(b, x, 16);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

// 传输圆钮：filled=true 为琥珀填充主锚点，否则透明描边。返回图元容器。
lv_obj_t* MakeTransport(lv_obj_t* parent, int32_t d, bool filled, lv_event_cb_t cb, int32_t cx,
                        int32_t cy, lv_obj_t** out_host) {
    lv_obj_t* b = lv_obj_create(parent);
    screen_strip_obj_chrome(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    if (filled) {
        pi_theme::ApplyBg(b, Tok::Accent);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        pi_theme::ApplyBg(b, Tok::AccentDim, LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
        pi_theme::ApplyBg(b, Tok::Card2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 12);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* host = lv_obj_create(b);
    screen_strip_obj_chrome(host);
    lv_obj_remove_flag(host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(host, d * 55 / 100, d * 55 / 100);
    lv_obj_center(host);
    lv_obj_set_style_bg_opa(host, LV_OPA_TRANSP, LV_PART_MAIN);
    if (out_host != nullptr) *out_host = host;
    return b;
}

void BuildPage() {
    s_root = lv_obj_create(s_parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_root, kW, kH);
    lv_obj_set_pos(s_root, 0, 0);
    pi_theme::ApplyBg(s_root, Tok::Bg);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // edge-nav 兜底命中
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_PRESS_LOCK);

    // 顶栏：返回箭头（< 双短臂）+ 列表钮（三横线）
    lv_obj_t* back = MakeTopBtn(s_root, OnBackBtn, 16);
    Bar(back, 3, 20, Tok::Dim, 2, -7);   // 「<」上臂：/ （左端在下）
    lv_obj_t* ba1 = lv_obj_get_child(back, lv_obj_get_child_count(back) - 1);
    lv_obj_set_style_transform_pivot_x(ba1, 1, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(ba1, 10, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(ba1, 450, LV_PART_MAIN);
    Bar(back, 3, 20, Tok::Dim, 2, 7);    // 下臂：\ （左端在上）
    lv_obj_t* ba2 = lv_obj_get_child(back, lv_obj_get_child_count(back) - 1);
    lv_obj_set_style_transform_pivot_x(ba2, 1, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(ba2, 10, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(ba2, -450, LV_PART_MAIN);

    lv_obj_t* list = MakeTopBtn(s_root, OnListBtn, kW - 52 - 16);
    Bar(list, 22, 3, Tok::Dim, 0, -6);
    Bar(list, 22, 3, Tok::Dim, 0, 0);
    Bar(list, 22, 3, Tok::Dim, 0, 6);

    s_eyebrow = Label(s_root, "NOW PLAYING", &font_pi_mono_14, Tok::Faint);
    lv_obj_set_style_text_letter_space(s_eyebrow, 4, LV_PART_MAIN);
    lv_obj_align(s_eyebrow, LV_ALIGN_TOP_MID, 0, 70);

    // art host
    s_art_host = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_art_host);
    lv_obj_remove_flag(s_art_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_art_host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_art_host, kArt, kArt);
    lv_obj_align(s_art_host, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_radius(s_art_host, 24, LV_PART_MAIN);
    pi_theme::ApplyBg(s_art_host, Tok::Card2);
    lv_obj_set_style_bg_opa(s_art_host, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_art_host, true, LV_PART_MAIN);

    // 标题 / 副题
    s_title = Label(s_root, "--", &font_puhui_30_4, Tok::Tx);
    lv_obj_set_width(s_title, kW - 96);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 118 + kArt + 22);

    s_sub = Label(s_root, "", &font_puhui_20_4, Tok::Dim);
    lv_obj_set_width(s_sub, kW - 96);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 118 + kArt + 64);

    // 进度条 track + fill
    const int32_t prog_y = 118 + kArt + 108;
    s_prog_track = lv_obj_create(s_root);
    screen_strip_obj_chrome(s_prog_track);
    lv_obj_remove_flag(s_prog_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_prog_track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_prog_track, kProgW, 6);
    lv_obj_set_pos(s_prog_track, 48, prog_y);
    lv_obj_set_style_radius(s_prog_track, 3, LV_PART_MAIN);
    pi_theme::ApplyBg(s_prog_track, Tok::Card2);
    lv_obj_set_style_bg_opa(s_prog_track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_prog_track, true, LV_PART_MAIN);
    s_prog_fill = lv_obj_create(s_prog_track);
    screen_strip_obj_chrome(s_prog_fill);
    lv_obj_remove_flag(s_prog_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_prog_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_prog_fill, 0, 6);
    lv_obj_align(s_prog_fill, LV_ALIGN_LEFT_MID, 0, 0);
    pi_theme::ApplyBg(s_prog_fill, Tok::Accent);
    lv_obj_set_style_bg_opa(s_prog_fill, LV_OPA_COVER, LV_PART_MAIN);

    s_time_cur = Label(s_root, "0:00", &font_pi_mono_14, Tok::Faint);
    lv_obj_set_pos(s_time_cur, 48, prog_y + 14);
    s_time_dur = Label(s_root, "--:--", &font_pi_mono_14, Tok::Faint);
    lv_obj_align(s_time_dur, LV_ALIGN_TOP_RIGHT, -48, prog_y + 14);

    // 传输排：prev(64) / play(88) / next(64)，居中于底部带
    const int32_t cy = kH - kBandH / 2 - 6;
    lv_obj_t* ph = nullptr;
    MakeTransport(s_root, 64, false, OnPrev, kW / 2 - 128, cy, &ph);
    lv_obj_center(MakeTri(ph, 26, 1, Tok::Tx));
    Bar(ph, 3, 22, Tok::Tx, -14, 0);  // |◀

    MakeTransport(s_root, 88, true, OnPlay, kW / 2, cy, &s_play_host);
    SetPlayGlyph(false);

    lv_obj_t* nh = nullptr;
    MakeTransport(s_root, 64, false, OnNext, kW / 2 + 128, cy, &nh);
    lv_obj_center(MakeTri(nh, 26, 0, Tok::Tx));
    Bar(nh, 3, 22, Tok::Tx, 14, 0);  // ▶|
}

void RefreshPage() {
    if (s_root == nullptr) return;
    MediaController& mc = MediaController::Instance();
    MediaState st = mc.state();
    MediaItem cur = mc.current();
    bool playing = st == MediaState::Playing;
    int idx = mc.index();

    bool track_changed = (idx != s_page_cache.index) || (cur.title != s_page_cache.title) ||
                         (cur.is_stream != s_page_cache.is_stream);
    if (track_changed) {
        s_page_cache.index = idx;
        s_page_cache.title = cur.title;
        s_page_cache.is_stream = cur.is_stream;
        lv_label_set_text(s_eyebrow, cur.is_stream ? "LIVE RADIO" : "NOW PLAYING");
        lv_label_set_text(s_title, cur.title.empty() ? "--" : cur.title.c_str());
        lv_label_set_text(s_sub, cur.subtitle.c_str());
        BuildArt(cur.is_stream, playing);
    }
    if (static_cast<int>(st) != s_page_cache.state) {
        // 播放态变化：切图元 + 电波呼吸随播放态起停（重建 art）。
        bool was_playing = s_page_cache.state == static_cast<int>(MediaState::Playing);
        s_page_cache.state = static_cast<int>(st);
        SetPlayGlyph(playing);
        if (cur.is_stream && was_playing != playing) BuildArt(cur.is_stream, playing);
    }

    // 进度 / 时间
    int dur = mc.duration_s();
    int pos = mc.position_s();
    char buf[16];
    FmtTime(pos, buf, sizeof(buf));
    lv_label_set_text(s_time_cur, buf);
    if (cur.is_stream) {
        lv_label_set_text(s_time_dur, "LIVE");
        pi_theme::ApplyText(s_time_dur, Tok::Accent);
        lv_obj_set_width(s_prog_fill, 0);
    } else if (dur <= 0) {
        lv_label_set_text(s_time_dur, "--:--");
        pi_theme::ApplyText(s_time_dur, Tok::Faint);
        lv_obj_set_width(s_prog_fill, 0);
    } else {
        FmtTime(dur, buf, sizeof(buf));
        lv_label_set_text(s_time_dur, buf);
        pi_theme::ApplyText(s_time_dur, Tok::Faint);
        int pct = pos * 100 / dur;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_obj_set_width(s_prog_fill, kProgW * pct / 100);
    }
}

void OnThemeChanged() {
    RedrawGlyphs();
    if (s_root != nullptr) {
        // art（arc 一次性取色）随主题重建；其余 Tok 控件共享样式自动翻转。
        s_page_cache.index = -2;  // 逼下一 tick 重建 art + 刷标题
        RefreshPage();
    }
}

void TimerCb(lv_timer_t*) {
    RefreshMini();
    RefreshPage();
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
namespace pi_media {

void CreateMiniBar(lv_obj_t* parent) {
    if (s_mini != nullptr) return;
    s_parent = parent;
    CreateMiniBarImpl(parent);
    if (s_theme_listener < 0) s_theme_listener = pi_theme::AddListener(OnThemeChanged);
    if (s_timer == nullptr) s_timer = lv_timer_create(TimerCb, 1000, nullptr);
    RefreshMini();
}

void SetMiniBarContext(bool allowed) {
    s_mini_ctx = allowed;
    RefreshMini();
}

void Open() {
    if (s_root != nullptr || s_parent == nullptr) return;
    s_page_cache = PageCache{};
    s_page_cache.index = -2;
    BuildPage();
    RefreshPage();
}

void Close() {
    CloseDrawer();
    if (s_root != nullptr) {
        lv_obj_delete(s_root);
        s_root = nullptr;
        s_eyebrow = s_art_host = s_title = s_sub = nullptr;
        s_prog_track = s_prog_fill = s_time_cur = s_time_dur = s_play_host = nullptr;
    }
}

bool IsOpen() { return s_root != nullptr; }

void Back() {
    if (s_drawer_root != nullptr) {
        CloseDrawer();
        return;
    }
    Close();
}

void OnScreenUnloaded() {
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    // widget 树随 screen 删除；只清静态指针与图元登记。
    s_glyphs.clear();
    s_drawer_root = s_drawer_list = nullptr;
    s_drawer_rows.clear();
    s_root = nullptr;
    s_mini = s_mini_title = s_mini_sub = s_mini_btn = s_mini_glyph = s_mini_prog = nullptr;
    s_mini_shown = false;
    s_mini_cache = MiniCache{};
}

}  // namespace pi_media
