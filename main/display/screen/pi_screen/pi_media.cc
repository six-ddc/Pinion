#include "pi_media.h"

#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"  // 播控图元画布显式 PSRAM 分配
#include "esp_pthread.h"  // 后台封面读盘 worker 的栈配置（仅真机路径）
#include "freertos/FreeRTOS.h"  // worker 退出握手信号量（设备端不能 join，见 StopCoverWorker）
#include "freertos/semphr.h"
#include "metalio_hal/display.h"  // worker 线程投递 lv_async_call 前须持 LVGL 锁
#endif

#include "cJSON.h"                     // 断点续播记录：JSON 序列化/解析
#include "esp_log.h"
#include "media_player/media_id3.h"
#include "media_player/media_player.h"
#include "media_player/radio_stations.h"  // 电台续播：station index -> 名称/URL
#include "metalio_hal/audio.h"         // Now-Playing 页音量条
#include "metalio_hal/network.h"       // 电台续播的联网判定
#include "pi_card_icons.h"
#include "pi_fonts.h"
#include "pi_theme.h"
#include "pi_ui_bridge.h"              // pi_agent_task_note：关闭播放器静默告知模型
#include "screen_util.h"
#include "settings.h"                  // NVS 持久化「media/last」

// lv_image_decoder_dsc_t 的字段（.header/.decoded）只在私有头里给了完整定义
// （公开头 lv_types.h 只 forward-declare）；LVGL_ROOT_DIR（managed_components/
// lvgl__lvgl）在两端都是 PUBLIC include dir，"src/..." 相对路径可达。只有本文件
// 的封面预解码需要它——不通过 pi_card 渲染器，不污染其它文件。
#include "src/draw/lv_image_decoder_private.h"

// ---------------------------------------------------------------------------
// 见头文件。设计语言严格延续 pi_settings / pi_quick_panel：Bg 深底、Card 面 +
// 1px Line 边、唯一琥珀强调、mono 大写小字 caption、大圆角、克制留白。全程只用
// pi_theme 令牌取色（唯一例外：透明度、canvas 图元里落到令牌颜色时的一次性取色）。
// ---------------------------------------------------------------------------
namespace {

constexpr char TAG[] = "pi_media";

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
#ifdef ESP_PLATFORM
    // 30px 档 (~3.6KB) 低于 SPIRAM_MALLOC_ALWAYSINTERNAL(4096) 会落内部堆——低水位
    // 时拿到 NULL 曾致：按钮画残 + 每帧 esp_cache_msync(NULL) 报错。像素缓冲 CPU
    // 软绘，显式 PSRAM 优先，不占内部 RAM。
    void* buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) buf = malloc(buf_size);
#else
    void* buf = malloc(buf_size);
#endif
    if (buf == nullptr) return cv;  // 极端低内存：无图元退化，绝不给 canvas 空缓冲
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
// 紧凑媒体行（多实例：Chat dock 左列内嵌 + Idle 屏幕级；state != stopped 时浮现）
// ---------------------------------------------------------------------------
lv_obj_t* s_parent = nullptr;    // pi_screen 的 screen 对象（Open() 用）

struct InlineBar {
    lv_obj_t* root = nullptr;
    lv_obj_t* glyph = nullptr;   // play/pause 图元容器
    lv_obj_t* title = nullptr;   // "title · sub" 单行
    lv_obj_t* prog = nullptr;    // 底部 2px 进度线
    bool gated = false;          // true = 受 s_mini_ctx 门控（Idle 屏幕级实例）
    bool shown = false;          // 当前实际可见（用于淡入触发一次）
    int cache_state = -1;
    std::string cache_line;
};
std::vector<InlineBar> s_bars;
bool s_mini_ctx = true;  // 屏幕级实例是否允许显示（Go 设置；仅 Idle 为 true）

constexpr int32_t kInlineH = 36;     // 紧凑行默认高（调用方可覆盖）
constexpr int32_t kInlineGlyph = 22; // play/pause 图元边长

lv_timer_t* s_timer = nullptr;

void OnInlineToggle(lv_event_t*) { MediaController::Instance().Toggle(); }
void OnInlineBody(lv_event_t*) { pi_media::Open(); }

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

// 紧凑行淡入
void InlineFadeIn(lv_obj_t* o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), LV_PART_MAIN);
    });
    lv_anim_start(&a);
}

// 一个紧凑行实例：flex row [play/pause 触区][title 单行]，底部 2px 进度线悬浮。
// 透明底、无卡片 chrome——视觉上融入宿主（Chat dock / Idle 提示带）。
lv_obj_t* CreateInlineBarImpl(lv_obj_t* parent, bool gated) {
    InlineBar b;
    b.gated = gated;
    b.root = lv_obj_create(parent);
    screen_strip_obj_chrome(b.root);
    lv_obj_remove_flag(b.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b.root, LV_PCT(100), kInlineH);
    lv_obj_set_style_bg_opa(b.root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(b.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b.root, OnInlineBody, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(b.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(b.root, LV_FLEX_FLOW_ROW);
    // Idle 屏幕级实例（gated）内容居中——下方「按住说话」提示行是居中的，左对齐会显得
    // 整簇偏左；dock 实例左对齐与 token 统计行同列头。
    lv_obj_set_flex_align(b.root, gated ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b.root, 10, LV_PART_MAIN);

    // 左：play/pause 触区（行高见方，ext_click_area 抬触摸）
    lv_obj_t* btn = lv_obj_create(b.root);
    screen_strip_obj_chrome(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, kInlineH, kInlineH);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 12);
    lv_obj_add_event_cb(btn, OnInlineToggle, LV_EVENT_CLICKED, nullptr);
    b.glyph = lv_obj_create(btn);
    screen_strip_obj_chrome(b.glyph);
    lv_obj_remove_flag(b.glyph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b.glyph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b.glyph, kInlineGlyph, kInlineGlyph);
    lv_obj_center(b.glyph);
    lv_obj_set_style_bg_opa(b.glyph, LV_OPA_TRANSP, LV_PART_MAIN);
    SetGlyph(b.glyph, false, Tok::Dim, kInlineGlyph);

    // 中："title · sub" 单行。dock 实例占满余宽（左对齐）；居中实例按内容收缩、
    // 超长时被 max_width 钳住出省略号。
    b.title = Label(b.root, "--", &font_puhui_20_4, Tok::Tx);
    lv_label_set_long_mode(b.title, LV_LABEL_LONG_DOT);
    if (gated) {
        lv_obj_set_style_max_width(b.title, LV_PCT(85), LV_PART_MAIN);
    } else {
        lv_obj_set_flex_grow(b.title, 1);
    }

    // 底：2px accent 进度线（悬浮，不参与 flex）
    b.prog = lv_obj_create(b.root);
    screen_strip_obj_chrome(b.prog);
    lv_obj_remove_flag(b.prog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b.prog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b.prog, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(b.prog, 0, 2);
    lv_obj_align(b.prog, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    pi_theme::ApplyBg(b.prog, Tok::Accent);
    lv_obj_set_style_bg_opa(b.prog, LV_OPA_COVER, LV_PART_MAIN);

    s_bars.push_back(b);
    return b.root;
}

void RefreshInline() {
    if (s_bars.empty()) return;
    MediaController& mc = MediaController::Instance();
    MediaState st = mc.state();
    bool active = st != MediaState::Stopped;
    MediaItem cur = mc.current();
    // 进度百分比（文件按 pct，直播/未知 -1 = 隐藏）
    int dur = mc.duration_s();
    int pct = -1;
    if (!cur.is_stream && dur > 0) {
        pct = mc.position_s() * 100 / dur;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    std::string line = cur.title.empty() ? "--" : cur.title;
    if (!cur.subtitle.empty()) line += " · " + cur.subtitle;  // "title · sub"
    bool playing = st == MediaState::Playing;

    for (InlineBar& b : s_bars) {
        if (b.root == nullptr) continue;
        bool want = active && (!b.gated || s_mini_ctx);
        if (!want) {
            if (b.shown) {
                lv_obj_add_flag(b.root, LV_OBJ_FLAG_HIDDEN);
                b.shown = false;
            }
            continue;
        }
        if (!b.shown) {
            InlineFadeIn(b.root);
            b.shown = true;
        }
        if (line != b.cache_line) {
            b.cache_line = line;
            lv_label_set_text(b.title, line.c_str());
        }
        if (static_cast<int>(st) != b.cache_state) {
            b.cache_state = static_cast<int>(st);
            SetGlyph(b.glyph, playing, playing ? Tok::Accent : Tok::Dim, kInlineGlyph);
        }
        // 行宽随宿主布局而变（dock 列 flex / Idle 定宽），每 tick 现取
        int32_t w = lv_obj_get_width(b.root);
        lv_obj_set_width(b.prog, (pct < 0 || w <= 0) ? 0 : w * pct / 100);
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
lv_obj_t* s_vol_slider = nullptr;  // Now-Playing 页音量条（任务C）
lv_obj_t* s_vol_val = nullptr;
uint32_t s_vol_last_apply_ms = 0;
lv_obj_t* s_drawer_root = nullptr;
lv_obj_t* s_drawer_list = nullptr;
std::vector<lv_obj_t*> s_drawer_rows;

constexpr int32_t kArt = 300;  // 任务C：为音量条腾出竖向空间（原 330）
constexpr int32_t kProgW = kW - 96;
constexpr uint32_t kVolApplyGapMs = 150;  // 拖动中节流写 NVS（同快捷面板 VOL 语义）

struct PageCache {
    int index = -2;
    int state = -1;
    bool is_stream = false;
    std::string title;
} s_page_cache;

// ---------------------------------------------------------------------------
// 断点续播持久化（体验优化 任务A）：pi_media 层用 Settings 写 NVS "media"/"last"
// 一条 JSON 记录（组件层 media_player 保持无 UI/NVS 依赖）。写磨损保护：值不变
// 不写；pos 只在 60s 周期采样进 JSON，稳态播放约 60s 才落一次盘。NVS 字符串 ~4000B
// 上限——文件路径列表超预算时以当前曲为中心截窗，丢弃两端并记录。
// ---------------------------------------------------------------------------
constexpr char kNvsNs[] = "media";
constexpr char kNvsKey[] = "last";
constexpr size_t kPathBudget = 3400;  // 路径部分字节预算（留 JSON scaffolding 余量）
constexpr int kMaxTracks = 60;        // 记录曲目数上限（兼顾 NVS 尺寸 + 续播时 ID3 回读耗时）

std::string s_last_saved;   // 去重：与上次写入完全一致则不写
std::string s_persist_sig;  // 不含 pos 的签名（state:index:size:path），变则立即存
int s_persist_tick = 0;     // TimerCb 计数，用于 60s 周期采样

std::string BaseNoExt(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}
std::string ParentDirName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return {};
    size_t p = path.find_last_of('/', slash - 1);
    return (p == std::string::npos) ? path.substr(0, slash) : path.substr(p + 1, slash - p - 1);
}
// 与 pi_card_media 的 ApplyId3Meta 等价（那份是另一 TU 的私有 static，不可跨文件复用）。
void ApplyId3(std::string& title, std::string& sub, const std::string& path) {
    media_id3::Tags t = media_id3::ReadTags(path);
    if (!t.title.empty()) title = t.title;
    if (!t.album.empty() && !t.artist.empty())
        sub = t.album + " · " + t.artist;  // "专辑 · 艺人"
    else if (!t.album.empty())
        sub = t.album;
    else if (!t.artist.empty())
        sub = t.artist;
}
// url -> kRadioStations 下标；找不到返回 -1。
int StationIndexOfUrl(const std::string& url) {
    for (size_t i = 0; i < media::kRadioStationCount; i++) {
        if (url == media::kRadioStations[i].url) return static_cast<int>(i);
    }
    return -1;
}

// 序列化当前 MediaController 播放态为 JSON；空列表返回 ""。
std::string BuildLastJson() {
    media::MediaController& mc = media::MediaController::Instance();
    int n = mc.playlist_size();
    if (n <= 0) return {};
    int idx = mc.index();
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    media::MediaItem cur = mc.current();

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return {};

    if (cur.is_stream) {
        cJSON_AddStringToObject(root, "type", "radio");
        cJSON_AddNumberToObject(root, "index", idx);
        cJSON_AddNumberToObject(root, "pos_s", 0);  // 直播无位置
        cJSON* arr = cJSON_AddArrayToObject(root, "stations");
        for (int i = 0; i < n; i++) {
            int sidx = StationIndexOfUrl(mc.item_at(i).path_or_url);
            if (sidx >= 0) cJSON_AddItemToArray(arr, cJSON_CreateNumber(sidx));
        }
    } else {
        std::vector<std::string> paths;
        paths.reserve(n);
        for (int i = 0; i < n; i++) paths.push_back(mc.item_at(i).path_or_url);
        // 以当前曲为中心对称扩窗（字节预算 + 曲目数上限双钳制）。
        int lo = idx, hi = idx;
        size_t used = paths[idx].size() + 8;
        bool grew = true;
        while (grew) {
            grew = false;
            if (hi + 1 < n && (hi - lo + 1) < kMaxTracks &&
                used + paths[hi + 1].size() + 8 <= kPathBudget) {
                used += paths[++hi].size() + 8;
                grew = true;
            }
            if (lo - 1 >= 0 && (hi - lo + 1) < kMaxTracks &&
                used + paths[lo - 1].size() + 8 <= kPathBudget) {
                used += paths[--lo].size() + 8;
                grew = true;
            }
        }
        if (lo > 0 || hi < n - 1) {
            ESP_LOGW(TAG, "media last: playlist windowed [%d,%d]/%d (dropped head=%d tail=%d)", lo, hi,
                     n, lo, n - 1 - hi);
        }
        cJSON_AddStringToObject(root, "type", "file");
        cJSON_AddNumberToObject(root, "index", idx - lo);  // 窗内相对下标
        cJSON_AddNumberToObject(root, "pos_s", mc.position_s());
        cJSON* arr = cJSON_AddArrayToObject(root, "paths");
        for (int i = lo; i <= hi; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(paths[i].c_str()));
    }

    char* txt = cJSON_PrintUnformatted(root);
    std::string out = (txt != nullptr) ? txt : std::string();
    if (txt != nullptr) cJSON_free(txt);
    cJSON_Delete(root);
    return out;
}

void DoSaveLast() {
    std::string j = BuildLastJson();
    if (j.empty() || j == s_last_saved) return;  // 无内容 / 未变化：不写（NVS 磨损保护）
    Settings s(kNvsNs, /*read_write=*/true);
    s.SetString(kNvsKey, j);
    s_last_saved = j;
    ESP_LOGI(TAG, "media last saved (%uB)", (unsigned)j.size());
}

// TimerCb 每秒调：曲目/状态/列表变化立即存；稳态播放每 60s 采样一次 pos。
void PersistPoll() {
    media::MediaController& mc = media::MediaController::Instance();
    if (mc.playlist_size() <= 0) return;  // 无内容可存（不擦除既有记录）
    media::MediaState st = mc.state();
    std::string sig = std::to_string(static_cast<int>(st)) + ":" + std::to_string(mc.index()) + ":" +
                      std::to_string(mc.playlist_size()) + ":" + mc.current().path_or_url;
    bool changed = (sig != s_persist_sig);
    s_persist_tick++;
    bool periodic = (st == media::MediaState::Playing && (s_persist_tick % 60 == 0));
    if (!changed && !periodic) return;
    s_persist_sig = sig;
    DoSaveLast();
}

// ---------------------------------------------------------------------------
// 封面预解码（Stage E）：换曲时把内嵌 APIC（JPEG via tjpgd / PNG via lodepng）
// 一次性解码成静态位图，之后 lv_image 只引用这份位图——不把 raw JPEG 字节直接
// 喂给 lv_image（tjpgd 是 get_area 条带解码器，每次重绘都会重新解码，1Hz 进度
// 刷新会让它每秒重解一次，见工作包设计记录）。JPEG 走 tjpgd 的 tile 循环自己
// 拼图；PNG（lodepng）open() 即整图解码，直接整块拷贝。
// 内存纪律：s_cover 只保留"当前曲"这一份，下次换曲/关页前必须先释放。
struct CoverBitmap {
    lv_image_dsc_t dsc{};
    uint8_t* data = nullptr;  // malloc；随 FreeCoverBitmap/换曲释放
};
CoverBitmap s_cover;
int s_cover_alloc_count = 0;  // Stage E 内存纪律验证用：净分配数应恒为 0 或 1（当前曲）

void FreeCoverBitmap(CoverBitmap* cb) {
    if (cb->data != nullptr) {
        free(cb->data);
        cb->data = nullptr;
        s_cover_alloc_count--;
        ESP_LOGI(TAG, "cover bitmap freed, alloc_count=%d", s_cover_alloc_count);
    }
    cb->dsc = lv_image_dsc_t{};
}

// ---- 封面后台加载（Stage E fix）：把 APIC 读盘（可达 4MB、SD 变长延迟）挪出
// LVGL 线程；只保留"解码 + 叠图"在 LVGL 线程（经 lv_async_call 回调完成——LVGL
// 解码器/位图操作非线程安全，必须在 LVGL 线程做）。generation 计数处理换曲/连跳
// 竞态：过期结果一律释放不上屏。worker 为单线程 latest-wins，避免连跳时多份并发
// 大读盘与内存尖峰。设备端用 esp_pthread 抬栈；sim 端普通 std::thread。
std::atomic<uint32_t> s_cover_gen{0};  // 每次换曲/关页 +1，作废在途封面加载
std::thread s_cover_worker;
std::mutex s_cover_mtx;
std::condition_variable s_cover_cv;
std::string s_cover_req_path;  // 最新一次请求路径（latest-wins）
uint32_t s_cover_req_gen = 0;
bool s_cover_req_pending = false;
bool s_cover_worker_stop = false;
// worker 存活标记（s_cover_mtx 保护）：设备端线程创建后立即 detach，joinable() 恒为
// false，不能再拿它当"worker 在跑"的判据。
bool s_cover_worker_alive = false;
#ifdef ESP_PLATFORM
SemaphoreHandle_t s_cover_exit_sem = nullptr;  // worker 退出信号（懒创建、常驻；见 StopCoverWorker）
#endif

struct CoverReady {
    uint32_t gen;
    uint8_t* bytes;  // ReadCover 的 malloc 缓冲；OnCoverReady 所有分支负责 free
    size_t size;
};

// bytes/len：APIC 原始编码字节（调用方持有，本函数只读不释放）。成功时 out
// 持有一份自己 malloc 的位图（cf/w/h/stride 取自解码器实际结果）。
bool DecodeCoverBytes(const uint8_t* bytes, size_t len, CoverBitmap* out) {
    int w = 0, h = 0;
    if (!media_id3::PeekImageSize(bytes, len, &w, &h)) return false;
    if (w <= 0 || h <= 0 || w > 800 || h > 800) return false;  // 防御性尺寸门控

    // EXIF/非标 APP0 的 baseline JPEG：底层 tjpgd 能解，但 LVGL 包装层 is_jpg() 只
    // 认精确 JFIF 签名。喂解码器前做 JFIF 归一化（插入标准 APP0）。尺寸/progressive
    // 门控已在原始 bytes 上判完——归一化逐字节保留 SOF/扫描段，尺寸不变。
    const uint8_t* dec_bytes = bytes;
    size_t dec_len = len;
    size_t norm_len = 0;
    uint8_t* norm = media_id3::NormalizeJpegHeader(bytes, len, &norm_len);
    if (norm != nullptr) {
        dec_bytes = norm;
        dec_len = norm_len;
    }

    lv_image_dsc_t src{};
    src.data = dec_bytes;
    src.data_size = static_cast<uint32_t>(dec_len);
    src.header.cf = LV_COLOR_FORMAT_RAW;  // 让解码器按魔数自行识别 JPEG/PNG
    src.header.w = static_cast<uint32_t>(w);
    src.header.h = static_cast<uint32_t>(h);

    lv_image_decoder_dsc_t dsc{};
    if (lv_image_decoder_open(&dsc, &src, nullptr) != LV_RESULT_OK) {
        if (norm != nullptr) free(norm);
        return false;
    }
    // 兜底防御：若没有真正的解码器接手（tjpgd/lodepng 都不认——如归一化也救不了的
    // 畸形/progressive-被-info-放行-但-open-失败之外的变体），LVGL 会退到 bin_decoder，
    // 它把 RAW 原样包成一份 decoded（cf 仍为 RAW）。此时绝不能把未解码的编码字节当
    // 位图整块拷出去（会显示成花屏），一律当解码失败处理，退回生成式母题。
    if (dsc.header.cf == LV_COLOR_FORMAT_RAW) {
        lv_image_decoder_close(&dsc);
        if (norm != nullptr) free(norm);
        return false;
    }

    uint8_t* buf = nullptr;
    size_t buf_size = 0;
    uint32_t out_w = dsc.header.w;
    uint32_t out_h = dsc.header.h;
    uint32_t cf = dsc.header.cf;  // lv_image_header_t::cf 是 8-bit 位域，非枚举类型
    uint32_t stride = 0;

    if (dsc.decoded != nullptr) {
        // 整图一次性解码（PNG/lodepng）：dsc.decoded 已是完整位图，整块拷贝出来
        // 自持一份（decoder_close 会销毁 dsc.decoded 本身）。
        const lv_draw_buf_t* db = dsc.decoded;
        buf_size = db->data_size;
        buf = static_cast<uint8_t*>(malloc(buf_size));
        if (buf != nullptr) std::memcpy(buf, db->data, buf_size);
        cf = db->header.cf;
        out_w = db->header.w;
        out_h = db->header.h;
        stride = db->header.stride;
    } else {
        // 条带解码（JPEG/tjpgd）：循环取 tile 拼进自持缓冲，驱动方式与 LVGL 内部
        // lv_draw_image.c 的 img_decode_and_draw 一致（decoded_area 哨兵初值
        // LV_COORD_MIN，每轮读回归一化后的 tile 矩形，直到解码器耗尽返回非 OK）。
        stride = out_w * 3;  // tjpgd 固定输出 RGB888
        buf_size = static_cast<size_t>(stride) * out_h;
        buf = static_cast<uint8_t*>(malloc(buf_size));
        if (buf != nullptr) {
            std::memset(buf, 0, buf_size);
            lv_area_t full_area{0, 0, static_cast<int32_t>(out_w) - 1, static_cast<int32_t>(out_h) - 1};
            lv_area_t tile{LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
            lv_result_t res = LV_RESULT_OK;
            // tile 数上限按最小 MCU 8×8 估 ceil(w/8)*ceil(h/8) + 余量：之前硬编码 4096
            // 会把大尺寸低子采样（4:4:4/灰度，8×8 MCU）JPEG 的下半部截成黑块。
            int max_tiles = ((static_cast<int>(out_w) + 7) / 8) * ((static_cast<int>(out_h) + 7) / 8) + 16;
            int guard = 0;
            while (res == LV_RESULT_OK && guard++ < max_tiles) {  // guard：绝不死循环
                res = lv_image_decoder_get_area(&dsc, &full_area, &tile);
                if (res != LV_RESULT_OK) break;
                const lv_draw_buf_t* db = dsc.decoded;
                if (db == nullptr) break;
                int32_t tw = tile.x2 - tile.x1 + 1;
                int32_t th = tile.y2 - tile.y1 + 1;
                for (int32_t row = 0; row < th; row++) {
                    int32_t dy = tile.y1 + row;
                    if (dy < 0 || dy >= static_cast<int32_t>(out_h)) continue;
                    int32_t dx = tile.x1;
                    if (dx < 0) dx = 0;
                    int32_t copy_w = tw - (dx - tile.x1);
                    if (dx + copy_w > static_cast<int32_t>(out_w)) copy_w = static_cast<int32_t>(out_w) - dx;
                    if (copy_w <= 0) continue;
                    std::memcpy(buf + (static_cast<size_t>(dy) * out_w + dx) * 3,
                               db->data + static_cast<size_t>(row) * db->header.stride,
                               static_cast<size_t>(copy_w) * 3);
                }
            }
        }
        cf = LV_COLOR_FORMAT_RGB888;
    }
    lv_image_decoder_close(&dsc);
    if (norm != nullptr) free(norm);  // 归一化缓冲仅解码期间被 memfs 引用，此刻可释放

    if (buf == nullptr) return false;
    s_cover_alloc_count++;
    ESP_LOGI(TAG, "cover bitmap allocated %uB, alloc_count=%d", (unsigned)buf_size, s_cover_alloc_count);
    out->data = buf;
    out->dsc.header.cf = cf;
    out->dsc.header.w = out_w;
    out->dsc.header.h = out_h;
    out->dsc.header.stride = stride;
    out->dsc.data_size = static_cast<uint32_t>(buf_size);
    out->dsc.data = buf;
    return true;
}

// LVGL 线程（lv_async_call 回调）：解码 worker 读回的封面字节，成功则在 s_art_host
// 内叠一张 LV_IMAGE_ALIGN_COVER 图（裁切铺满 330×330，圆角由 s_art_host 的
// clip_corner 兜底）。全程复核 generation + 页面状态：过期（用户又换了曲）或页面
// 已关则释放不上屏。失败（超尺寸/progressive/解码失败）静默保留生成式母题。
void OnCoverReady(void* p) {
    CoverReady* cr = static_cast<CoverReady*>(p);
    if (cr->gen != s_cover_gen.load() || s_art_host == nullptr) {  // 已过期/页已关
        free(cr->bytes);
        delete cr;
        return;
    }
    CoverBitmap cb;
    bool ok = DecodeCoverBytes(cr->bytes, cr->size, &cb);
    free(cr->bytes);  // 原始编码字节仅解码期间需要
    if (!ok) {
        delete cr;
        return;
    }
    if (cr->gen != s_cover_gen.load() || s_art_host == nullptr) {  // 解码期间又换曲
        FreeCoverBitmap(&cb);  // 丢弃这份，不泄漏、不上屏
        delete cr;
        return;
    }
    FreeCoverBitmap(&s_cover);
    s_cover = cb;

    lv_obj_t* img = lv_image_create(s_art_host);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(img, kArt, kArt);
    lv_image_set_src(img, &s_cover.dsc);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);  // 保持比例铺满+居中裁切
    lv_obj_center(img);
    delete cr;
}

// 后台 worker（非 LVGL 线程）：只做 ReadCover（SD 读盘，可达 4MB、变长延迟），读到
// 后经 lv_async_call 把字节交回 LVGL 线程解码。latest-wins：连跳时旧请求在取出前
// 就被覆盖，读盘期间换曲的结果按 generation 丢弃。
void CoverWorkerRun() {
    for (;;) {
        std::string path;
        uint32_t gen;
        {
            std::unique_lock<std::mutex> lk(s_cover_mtx);
            s_cover_cv.wait(lk, [] { return s_cover_req_pending || s_cover_worker_stop; });
            if (s_cover_worker_stop) return;
            path = s_cover_req_path;
            gen = s_cover_req_gen;
            s_cover_req_pending = false;
        }
        if (gen != s_cover_gen.load()) continue;  // 取出前已被更新请求超越
        uint32_t t0 = lv_tick_get();
        size_t sz = 0;
        std::string mime;
        uint8_t* bytes = media_id3::ReadCover(path, &sz, &mime);
        uint32_t t1 = lv_tick_get();
        if (bytes == nullptr) {
            ESP_LOGI(TAG, "cover: no APIC (read %ums)", static_cast<unsigned>(t1 - t0));
            continue;
        }
        if (gen != s_cover_gen.load()) {  // 读盘期间换曲：丢弃，不泄漏
            free(bytes);
            continue;
        }
        // %zu 是真机地雷：newlib-nano 不认、不消费参数，后面 %s 变参错位把耗时毫秒数当
        // 指针 strlen → Load access fault（真机实测：播带封面 MP3 开全屏必崩）。
        ESP_LOGI(TAG, "cover: read %uB in %ums mime=%s -> decode on LVGL thread", (unsigned)sz,
                 static_cast<unsigned>(t1 - t0), mime.c_str());
        // lv_async_call 操作 LVGL 内部链表，非线程安全：设备端持 esp_lv_adapter 锁，
        // sim 端（LV_USE_OS=PTHREAD）持 lv_lock，与各自的 lv_timer_handler 泵互斥。
#ifdef ESP_PLATFORM
        mhal::display::Lock();
        lv_async_call(OnCoverReady, new CoverReady{gen, bytes, sz});
        mhal::display::Unlock();
#else
        lv_lock();
        lv_async_call(OnCoverReady, new CoverReady{gen, bytes, sz});
        lv_unlock();
#endif
    }
}

// 线程入口包装：跑完线程体后发退出信号（设备端）。信号之后不得再触碰任何 s_cover_* 状态。
void CoverWorkerMain() {
    CoverWorkerRun();
#ifdef ESP_PLATFORM
    xSemaphoreGive(s_cover_exit_sem);
#endif
}

void StopCoverWorker() {
    bool was_alive;
    {
        std::lock_guard<std::mutex> lk(s_cover_mtx);
        was_alive = s_cover_worker_alive;
        s_cover_worker_stop = true;
    }
    s_cover_cv.notify_one();
#ifdef ESP_PLATFORM
    // 设备端不能 join：本函数跑在 LVGL 任务上，esp_pthread 的 join 等在 task notification
    // 槽 0 上，会被 VSync/跨线程唤醒等对 LVGL 任务的无关 notify 假唤醒，随即 vTaskDelete
    // 掉还阻塞在 cv 里的 worker（栈被释放、cv 等待节点悬挂）——与媒体泵"下一台"崩溃同一
    // 根因（详见 media_player/src/media_internal.h Pump::exit_sem 注释）。worker 已
    // detach，这里改等它的退出信号量。
    if (was_alive) xSemaphoreTake(s_cover_exit_sem, portMAX_DELAY);
#else
    (void)was_alive;
    if (s_cover_worker.joinable()) s_cover_worker.join();
#endif
    {
        std::lock_guard<std::mutex> lk(s_cover_mtx);
        s_cover_worker_alive = false;
    }
}

// 换曲时调用（LVGL 线程）：把封面加载请求交给后台 worker（懒创建）。gen 已在
// RefreshPage 换曲分支 +1，这里取当前 gen 作为本请求版本。电台不调本函数。
void TryLoadCover(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(s_cover_mtx);
        if (!s_cover_worker_alive) {
            s_cover_worker_stop = false;
#ifdef ESP_PLATFORM
            if (s_cover_exit_sem == nullptr) s_cover_exit_sem = xSemaphoreCreateBinary();
            if (s_cover_exit_sem == nullptr) return;  // 极端低内存：放弃本次封面加载
            // esp_pthread_set_cfg 影响本任务（LVGL）今后创建的所有 pthread，用完即还原
            //（同 media_controller.cc StartPump 的处理与理由）。
            esp_pthread_cfg_t prev_cfg;
            const bool had_prev_cfg = esp_pthread_get_cfg(&prev_cfg) == ESP_OK;
            esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
            cfg.stack_size = 6144;  // ReadCover fread + std::string；无 minmp3，栈占用小
            cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // 栈放 PSRAM（只做 SD 读）
            cfg.thread_name = "cover_ld";
            esp_pthread_set_cfg(&cfg);
            s_cover_worker = std::thread(CoverWorkerMain);
            s_cover_worker.detach();  // 生死由 s_cover_exit_sem 握手（见 StopCoverWorker）
            if (had_prev_cfg) {
                esp_pthread_set_cfg(&prev_cfg);
            } else {
                esp_pthread_cfg_t def = esp_pthread_get_default_config();
                esp_pthread_set_cfg(&def);
            }
#else
            s_cover_worker = std::thread(CoverWorkerMain);
#endif
            s_cover_worker_alive = true;
        }
        s_cover_req_path = path;
        s_cover_req_gen = s_cover_gen.load();
        s_cover_req_pending = true;
    }
    s_cover_cv.notify_one();
}

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

// 叉掉播放器：停止播放（列表保留、NVS 续播记录不清，快捷面板「音乐」仍可续）并关页；
// 紧凑媒体行随 Stopped 态在下一 tick 自动消失。停止前抓当前曲目名，inject 告知模型
// （空闲时主动起一轮让会话立刻有反应，对话中插到下一轮）——早前用静默 note 用户看不到
// 任何动静，以为通知丢了；且不告知的话模型会一直以为还在播，后续 media.control 全踩
// "nothing playing"。
void OnStopBtn(lv_event_t*) {
    MediaItem cur = MediaController::Instance().current();
    MediaController::Instance().Stop();
    pi_media::Close();
    std::string note = "「播放器」用户手动关闭了播放器，停止播放";
    if (!cur.title.empty()) note += "：" + cur.title;
    // 没对话过（快捷面板直接放歌）就不凭空起一轮，静默即可。
    if (pi_agent_task_has_messages()) pi_agent_task_inject(note.c_str());
}
void OnPrev(lv_event_t*) { MediaController::Instance().Prev(); }
void OnNext(lv_event_t*) { MediaController::Instance().Next(); }
void OnPlay(lv_event_t*) { MediaController::Instance().Toggle(); }

// 音量条（任务C）：拖动即时生效，NVS 持久化收敛到松手 + 拖动中按 kVolApplyGapMs
// 节流（mhal::audio::SetVolume 内部永远持久化，语义同快捷面板 VOL 滑条）。
void OnVolChanged(lv_event_t*) {
    if (s_vol_slider == nullptr) return;
    int v = static_cast<int>(lv_slider_get_value(s_vol_slider));
    if (s_vol_val != nullptr) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", v);
        lv_label_set_text(s_vol_val, buf);
    }
    uint32_t now = lv_tick_get();
    if (now - s_vol_last_apply_ms >= kVolApplyGapMs) {
        s_vol_last_apply_ms = now;
        mhal::audio::SetVolume(v, true);
    }
}
void OnVolReleased(lv_event_t*) {
    if (s_vol_slider == nullptr) return;
    mhal::audio::SetVolume(static_cast<int>(lv_slider_get_value(s_vol_slider)), true);
}

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

    // 顶栏：返回箭头 + 播放列表钮 + 停止叉（最右角；停止播放并关页）
    lv_obj_t* back = MakeTopBtn(s_root, OnBackBtn, 16);
    lv_obj_center(pi_card::MakeIcon(back, "chevron-left", 28, Tok::Dim));

    lv_obj_t* list = MakeTopBtn(s_root, OnListBtn, kW - 52 - 16 - 52 - 12);
    lv_obj_center(pi_card::MakeIcon(list, "list-music", 28, Tok::Dim));

    lv_obj_t* stop = MakeTopBtn(s_root, OnStopBtn, kW - 52 - 16);
    lv_obj_center(pi_card::MakeIcon(stop, "x", 28, Tok::Dim));

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

    // 音量条（任务C）：VOL caption + slider + value，位于时间行与传输排之间。尺寸/
    // 触区同快捷面板 VOL 滑条；滑条起于 x=48（屏内），不触发 edge-swipe 屏缘拦截层
    // （indev 级只拦真正屏缘起手，内部横向拖动归控件本身）。
    lv_obj_t* vol_row = lv_obj_create(s_root);
    screen_strip_obj_chrome(vol_row);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(vol_row, kProgW, 40);
    lv_obj_set_pos(vol_row, 48, prog_y + 40);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, 16, LV_PART_MAIN);

    lv_obj_t* vol_lbl = Label(vol_row, "VOL", &font_pi_mono_14, Tok::Faint);
    lv_obj_set_style_text_letter_space(vol_lbl, 2, LV_PART_MAIN);
    lv_obj_set_width(vol_lbl, 40);

    s_vol_slider = lv_slider_create(vol_row);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_set_height(s_vol_slider, 6);
    lv_obj_set_flex_grow(s_vol_slider, 1);
    pi_theme::ApplyBg(s_vol_slider, Tok::Card2);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_vol_slider, 3, LV_PART_MAIN);
    pi_theme::ApplyBg(s_vol_slider, Tok::Accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    pi_theme::ApplyBg(s_vol_slider, Tok::Accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 14, LV_PART_KNOB);
    lv_obj_set_style_radius(s_vol_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_vol_slider, 22);
    lv_slider_set_value(s_vol_slider, mhal::audio::GetVolume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_vol_slider, OnVolChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_vol_slider, OnVolReleased, LV_EVENT_RELEASED, nullptr);

    s_vol_val = Label(vol_row, "", &font_pi_mono_20, Tok::Tx);
    lv_obj_set_width(s_vol_val, 44);
    lv_obj_set_style_text_align(s_vol_val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    {
        char vbuf[8];
        std::snprintf(vbuf, sizeof(vbuf), "%d", mhal::audio::GetVolume());
        lv_label_set_text(s_vol_val, vbuf);
    }

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
        s_cover_gen.fetch_add(1);             // 作废任何在途封面加载（含切到电台的情形）
        FreeCoverBitmap(&s_cover);            // 换曲：先释放上一曲的解码位图
        BuildArt(cur.is_stream, playing);     // 生成式母题打底（clean 掉旧封面 img）
        ESP_LOGI(TAG, "track_changed idx=%d is_stream=%d path=%s", idx, cur.is_stream,
                 cur.path_or_url.c_str());
        if (!cur.is_stream) TryLoadCover(cur.path_or_url);  // 有真封面则叠图覆盖；电台永远走母题
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

    // 音量条：非拖动时跟随硬件音量（如快捷面板/按键改了音量）保持同步。
    if (s_vol_slider != nullptr && !lv_obj_has_state(s_vol_slider, LV_STATE_PRESSED)) {
        int hv = mhal::audio::GetVolume();
        if (static_cast<int>(lv_slider_get_value(s_vol_slider)) != hv) {
            lv_slider_set_value(s_vol_slider, hv, LV_ANIM_OFF);
            if (s_vol_val != nullptr) {
                char vb[8];
                std::snprintf(vb, sizeof(vb), "%d", hv);
                lv_label_set_text(s_vol_val, vb);
            }
        }
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
    RefreshInline();
    RefreshPage();
    PersistPoll();  // 断点续播：变化即存、稳态播放每 60s 采样一次
}

// 刷新定时器 + 主题监听（CreateInlineBar 可先于 Init 被调，两处都懒起）
void EnsureTicker() {
    if (s_theme_listener < 0) s_theme_listener = pi_theme::AddListener(OnThemeChanged);
    if (s_timer == nullptr) s_timer = lv_timer_create(TimerCb, 1000, nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
namespace pi_media {

void Init(lv_obj_t* screen) {
    s_parent = screen;
    EnsureTicker();
}

lv_obj_t* CreateInlineBar(lv_obj_t* parent, bool gate_by_context) {
    lv_obj_t* root = CreateInlineBarImpl(parent, gate_by_context);
    EnsureTicker();
    RefreshInline();
    return root;
}

void SetMiniBarContext(bool allowed) {
    s_mini_ctx = allowed;
    RefreshInline();
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
        lv_obj_delete(s_root);  // 连带删掉挂在 s_art_host 下的封面 lv_image 子对象
        s_root = nullptr;
        s_eyebrow = s_art_host = s_title = s_sub = nullptr;
        s_prog_track = s_prog_fill = s_time_cur = s_time_dur = s_play_host = nullptr;
        s_vol_slider = s_vol_val = nullptr;
    }
    s_cover_gen.fetch_add(1);   // 作废任何在途封面加载（页面关闭）
    FreeCoverBitmap(&s_cover);  // 页面不可见时不留一份解码位图在内存里
    s_page_cache.index = -2;    // 逼下次 Open() 重新走 track_changed（含重新预解码封面）
}

bool IsOpen() { return s_root != nullptr; }

void Back() {
    if (s_drawer_root != nullptr) {
        CloseDrawer();
        return;
    }
    Close();
}

bool HasResumable() {
    if (media::MediaController::Instance().state() != media::MediaState::Stopped) return true;
    Settings s(kNvsNs);  // 只读
    return !s.GetString(kNvsKey, "").empty();
}

ResumeResult ResumeLast() {
    media::MediaController& mc = media::MediaController::Instance();
    if (mc.state() != media::MediaState::Stopped) {  // 正在播/暂停/加载：直接开页
        Open();
        return ResumeResult::Opened;
    }
    Settings s(kNvsNs);  // 只读
    std::string j = s.GetString(kNvsKey, "");
    if (j.empty()) return ResumeResult::NoRecord;
    cJSON* root = cJSON_Parse(j.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "ResumeLast: corrupt record, ignoring");
        return ResumeResult::NoRecord;
    }

    cJSON* jtype = cJSON_GetObjectItem(root, "type");
    cJSON* jindex = cJSON_GetObjectItem(root, "index");
    const char* type = cJSON_IsString(jtype) ? jtype->valuestring : "";
    int saved_index = cJSON_IsNumber(jindex) ? jindex->valueint : 0;
    ResumeResult result = ResumeResult::NoRecord;

    if (std::strcmp(type, "radio") == 0) {
        // 电台是外拨 http，4G/WiFi 皆可（不像文件后台需 WiFi）；只判有无联网。
        if (!mhal::network::IsConnected()) {
            cJSON_Delete(root);
            return ResumeResult::NoNetwork;
        }
        cJSON* arr = cJSON_GetObjectItem(root, "stations");
        int cnt = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        std::vector<media::MediaItem> items;
        for (int i = 0; i < cnt; i++) {
            cJSON* e = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsNumber(e)) continue;
            int sidx = e->valueint;
            if (sidx < 0 || sidx >= static_cast<int>(media::kRadioStationCount)) continue;
            const media::RadioStation& rs = media::kRadioStations[sidx];
            media::MediaItem m;
            m.title = rs.name;
            m.subtitle = rs.genre;
            m.path_or_url = rs.url;
            m.is_stream = true;
            m.duration_s = 0;
            items.push_back(m);
        }
        if (items.empty()) {
            cJSON_Delete(root);
            return ResumeResult::NoRecord;
        }
        if (saved_index < 0 || saved_index >= static_cast<int>(items.size())) saved_index = 0;
        mc.StagePlaylist(items, saved_index);
        result = ResumeResult::Opened;
    } else {  // file
        cJSON* arr = cJSON_GetObjectItem(root, "paths");
        int cnt = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        std::vector<media::MediaItem> items;
        int new_idx = 0;
        bool idx_set = false;
        uint32_t t0 = lv_tick_get();
        for (int i = 0; i < cnt; i++) {
            cJSON* e = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsString(e) || e->valuestring == nullptr) continue;
            std::string p = e->valuestring;
            struct stat stt;
            if (::stat(p.c_str(), &stt) != 0 || !S_ISREG(stt.st_mode)) continue;  // 已删除，跳过
            if (!idx_set && i >= saved_index) {
                new_idx = static_cast<int>(items.size());  // 存点或其后第一个幸存曲
                idx_set = true;
            }
            media::MediaItem m;
            m.title = BaseNoExt(p);
            m.subtitle = ParentDirName(p);
            ApplyId3(m.title, m.subtitle, p);  // 走现有 ID3 回填（同 pi_card_media 语义）
            m.path_or_url = p;
            m.is_stream = false;
            m.duration_s = media_id3::ProbeDurationS(p);  // 0 = 未知，UI 显示 --:--
            items.push_back(m);
        }
        if (items.empty()) {
            cJSON_Delete(root);
            return ResumeResult::FilesGone;
        }
        if (!idx_set) new_idx = static_cast<int>(items.size()) - 1;  // 存点在幸存曲之后
        ESP_LOGI(TAG,
                 "ResumeLast: file %d survivors start=%d id3=%ums (公有 API 无 seek，pos_s 仅记录，"
                 "从曲首起播)",
                 static_cast<int>(items.size()), new_idx,
                 static_cast<unsigned>(lv_tick_get() - t0));
        mc.StagePlaylist(items, new_idx);  // start_index>=0 立即起播
        result = ResumeResult::Opened;
    }

    cJSON_Delete(root);
    if (result == ResumeResult::Opened) Open();
    return result;
}

void OnScreenUnloaded() {
    DoSaveLast();  // 断点续播：屏卸载前最后落一次盘
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    // widget 树随 screen 删除；只清静态指针与图元登记。
    s_glyphs.clear();
    s_drawer_root = s_drawer_list = nullptr;
    s_drawer_rows.clear();
    s_root = nullptr;
    s_art_host = nullptr;  // widget 随 screen 删除；防止在途 OnCoverReady 误用（gen 双保险）
    s_bars.clear();
    s_mini_ctx = true;
    s_cover_gen.fetch_add(1);  // 作废在途封面加载
    StopCoverWorker();         // 停后台 worker 并 join，避免其在 teardown 后再投递 async
    FreeCoverBitmap(&s_cover);
}

}  // namespace pi_media
