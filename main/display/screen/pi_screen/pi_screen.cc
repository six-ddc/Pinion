#include "pi_screen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "IOExpander.hpp"
#include "lv_markdown/md_inline.h"
#include "lv_markdown/md_view.h"
#include "pi_card/pi_card_cmd.h"
#include "pi_card/pi_card_data.h"
#include "pi_card/pi_card_host.h"
#include "metalio_hal/audio_pipeline.h"
#include "metalio_hal/bluetooth.h"
#include "metalio_hal/gps.h"
#include "metalio_hal/imu.h"
#include "metalio_hal/network.h"
#include "metalio_hal/power.h"
#include "metalio_hal/storage.h"
#include "metalio_hal/sysmon.h"
#include "media_player/media_player.h"  // 熄屏门控只读查询 MediaController 播放态（不调用任何变更态 API）
#include "pi_fonts.h"
#include "pi_media_focus.h"
#include "pi_net_events.h"
#include "pi_quick_panel.h"
#include "pi_media.h"
#include "pi_settings.h"
#include "pi_sleep.h"
#include "pi_theme.h"
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

// ----- palette（P2 起收编进 pi_theme 双主题令牌表；本文件不再持有色值） -----
// 静态配色一律走 pi_theme 共享样式（MakeRect/SetLabelFont 等按 Tok 挂载），
// 主题切换时由 pi_theme::Set 统一刷新；动态点位（recolor 内嵌 hex）经
// pi_theme::AddListener 重涂。
using pi_theme::Tok;

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
// 待起聆听阶段（按住未达阈值时长）里，水平位移越过此值且横向占优 => 判定为"滑动"
// 而非"按住说话"：撤销待起的聆听、放行屏幕手势（Idle 左滑回对话 / 生成后 Chat 右滑）。
constexpr int32_t kHSwipeDisarmPx = 40;
// 触屏"按住说话"阈值：按住不足此时长的轻点不进聆听（不再闪一下），与实体键一致。
constexpr uint32_t kTouchHoldToTalkMs = 240;
constexpr int32_t kNvsNamespaceMaxTools = 4;   // cached tool cards per turn (ZEN peek)
// pin 常驻组件在场时大时钟区整体隐藏（时间/日期上移到状态栏中央迷你时钟），
// pin host 占满状态栏以下整片区域并纵向居中承载卡片——不再收缩共存。

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
lv_obj_t* s_sbar_clock_lbl = nullptr;  // pin 在场时状态栏中央的迷你时钟（默认隐藏）
lv_obj_t* s_date_lbl = nullptr;
lv_obj_t* s_idle_mid = nullptr;  // 时钟容器（DIM 防烧屏移位的对象；pin 出现时收缩到 kPinClockH）
lv_obj_t* s_idle_breath = nullptr;
lv_obj_t* s_idle_ctx_fill = nullptr;
lv_obj_t* s_idle_ctx_lbl = nullptr;
lv_obj_t* s_idle_hint = nullptr;   // "按住说话" 提示条（pin 出现时隐藏，给常驻组件让位）
lv_obj_t* s_idle_hrule = nullptr;  // 提示条上方的分隔线（随 hint 一起隐藏）
lv_timer_t* s_clock_timer = nullptr;
lv_timer_t* s_burnin_timer = nullptr;  // DIM 期间每 60s 平移时钟容器

// ---- Phase3：常驻小组件（display:'standby'，单槽，固定 id "pin"）----
lv_obj_t* s_pin_host = nullptr;  // pin 卡的父容器：scr 子对象，仅 Idle 可见，非 clickable/scrollable
bool s_has_pin = false;

// ----- P1: 状态栏网络真状态 -------------------------------------------------
// 网络链路 UI 态。事件回调（网络栈任务线程）只写原子快照 + dirty，LVGL 侧
// 由 NetTick 轮询落地——与 pi_settings 的封送惯例同款。
enum class NetLinkState { Connecting, Connected, Down, NoCred };

struct WifiWidget {               // 三条状态栏（idle/listen/chat）各持有一份
    lv_obj_t* g4_lbl = nullptr;   // 4G 模式左侧 mono 小字
    lv_obj_t* bars[3] = {};       // 3 格信号
    lv_obj_t* err_dot = nullptr;  // 未连接/无凭据的 err 色小点
    lv_obj_t* bat_lbl = nullptr;  // 信号后的电量百分比（充电 Ok 色 / 低电 Err 色 / 平时 Dim）
};
std::vector<WifiWidget> s_wifi_widgets;
std::atomic<int> s_net_link{static_cast<int>(NetLinkState::Connecting)};
std::atomic<bool> s_net_dirty{false};
int s_net_listener_id = -1;
int s_theme_listener_id = -1;  // P2：主题切换时重涂 recolor 内嵌 hex 等动态点位
lv_timer_t* s_net_timer = nullptr;
uint32_t s_net_ticks = 0;
// 上次渲染的 (state, lit, wifi_mode)，避免周期 tick 反复重启呼吸动画
int s_net_last_render = -1;

// listen view widgets
lv_obj_t* s_wave_row = nullptr;
lv_obj_t* s_asr_lbl = nullptr;
// 聆听态"微信式"居中动作浮块：波形正下方的 pill（松开发送 / 上滑取消两态）+
// 其下一行更小更暗的滑动取消说明。抬到视线中央，不再钉在被手挡住的屏底两角。
lv_obj_t* s_listen_pill = nullptr;
lv_obj_t* s_listen_pill_lbl = nullptr;
lv_obj_t* s_listen_cancel_hint = nullptr;
lv_obj_t* s_rec_lbl = nullptr;
lv_timer_t* s_asr_timer = nullptr;  // 70ms：轮询真 ASR 共享态并渲染（AsrTick）
lv_timer_t* s_rec_timer = nullptr;
int s_rec_secs = 0;
std::vector<lv_obj_t*> s_wave_bars;
lv_timer_t* s_wave_timer = nullptr;  // ~40ms：从真实采集电平渲染对称起伏波形

// 波形环形缓冲：采集任务（on_frame）算每帧电平写入，WaveTick（LVGL 线程）读出
// 驱动柱高。单写单读，用一把轻量互斥保护（延用 s_asr_mutex 同款 semaphore）。
constexpr int kWaveBars = 26;                 // 与 BuildListenView 柱数一致
// 单一"此刻电平"（0..1）：采集任务写、LVGL 线程读。不再存时间序列——所有柱子
// 由当前电平驱动，波形原地随音量起伏，而不是按时间横向滚动（2026-07 按用户
// 反馈从环形缓冲改为此刻电平：滚动的录音机观感很怪）。
float s_wave_level = 0.0f;
SemaphoreHandle_t s_wave_mutex = nullptr;

// idle/chat sbar model-name labels (filled in from pi_agent_model_name()
// once the agent is up -- see UpdateModelLabels())
lv_obj_t* s_idle_model_lbl = nullptr;
lv_obj_t* s_chat_model_lbl = nullptr;

// chat view widgets
lv_obj_t* s_feed = nullptr;
// 消息流「贴底跟随」（sticky bottom）：用户往上翻看历史时，模型的新输出绝不该把视口抢回底部
//（同网页/聊天 App 的惯例）。判据 = 用户**自己**是否停在底部附近；用户自身的动作（发消息、
// 重试、错误横幅）仍然强制回底。由 feed 的 LV_EVENT_SCROLL_END 维护——注意流式期的程序化滚动
// 走 ANIM_OFF，LVGL 不为它发 SCROLL_END（该事件只在 indev 松手或滚动动画结束时发），故不会
// 污染这个标志。
constexpr int32_t kFeedStickyPx = 40;  // 容差 ≈ 一行正文；差几像素不该判成「脱离底部」
bool s_feed_stick = true;
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
// 会话内全部存活的 MdView（含已 Finish 的历史回复）：颜色烘焙在控件/recolor
// 标记里，不走 pi_theme 共享样式，主题切换时逐个 Retheme 重涂。root 删除
// （ClearFeed/整树销毁）时经 LV_EVENT_DELETE 自动出表。
std::vector<lvmd::MdView*> s_md_views;
uint32_t s_turn_start_ms = 0;
uint32_t s_think_start_ms = 0;
uint32_t s_tool_start_ms = 0;
int s_turn_tool_count = 0;
std::string s_turn_last_tool_output;
int s_out_tokens = 0;
bool s_out_tokens_known = false;  // false until a real DONE.i2 exists (stream carries no count)
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
lv_timer_t* s_pickup_timer = nullptr;  // P4-b 拿起唤醒：息屏时轮询 imu 检测运动

// P4-b 拿起唤醒：仅在息屏（非 Awake）时看加速度计，检测到明显运动即像 PWR_KEY 一样唤醒。
// imu 未焊/未采到（GetSnapshot 返回 false）时空转，绝不误唤醒（优雅降级）。阈值需真机整定。
void PickupWatchTick(lv_timer_t*) {
    static bool have_base = false;
    static int base_x = 0, base_y = 0, base_z = 0;
    if (pi_sleep::IsAwake()) {
        have_base = false;  // 醒着不监测，出息屏即丢基线
        return;
    }
    int x = 0, y = 0, z = 0, pitch = 0, roll = 0;
    if (!mhal::imu::GetSnapshot(x, y, z, pitch, roll)) {
        have_base = false;  // 无 imu 数据：不动作
        return;
    }
    if (!have_base) {  // 息屏后首帧设基线
        base_x = x;
        base_y = y;
        base_z = z;
        have_base = true;
        return;
    }
    const int dx = x - base_x, dy = y - base_y, dz = z - base_z;
    const long mag2 = (long)dx * dx + (long)dy * dy + (long)dz * dz;
    constexpr long kPickupThreshMg2 = 300L * 300L;  // ~300mg 合成位移；真机整定
    if (mag2 > kPickupThreshMg2) {
        have_base = false;
        pi_sleep::ConsumeKeyWake();  // 与按键唤醒同路：唤醒 + 重置无操作计时
    } else {
        base_x += dx / 4;  // 缓慢跟随基线，抵消姿态漂移，避免桌面微动累积成误唤醒
        base_y += dy / 4;
        base_z += dz / 4;
    }
}

// P4-c GPS：十进制度格式化到 5 位小数。规避 newlib-nano 无 %f 浮点打印——手动整数
// 拆分（180*1e5 < 2^31，long 安全），只用整数格式化。供 gps.lat / gps.lon String 路径。
std::string FormatDegE5(double deg) {
    bool neg = deg < 0;
    if (neg) deg = -deg;
    long scaled = static_cast<long>(deg * 100000.0 + 0.5);
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%ld.%05ld", neg ? "-" : "", scaled / 100000, scaled % 100000);
    return std::string(buf);
}

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
int32_t s_ptt_start_x = 0;  // 按下 x，用于待起阶段的水平滑动判定（放行屏幕左/右滑手势）
bool s_ptt_cancel_armed = false;  // 触屏 PTT：已上滑越过阈值、松手即取消（微信式）
ViewState s_ptt_ret = ViewState::Idle;   // 触屏 PTT 本次按压的返回态（按下时捕获）
bool s_ptt_listening = false;            // 触屏 PTT：已过按住阈值、StartListen 已触发
lv_timer_t* s_ptt_hold_timer = nullptr;  // "按住达阈值才进聆听"的一次性计时

// 谁拥有本次聆听（= 谁负责收尾）。触屏与实体键都能进聆听，先按者拥有本次，
// 后按者的 press/release 变纯 no-op，消除交叉打断与"谁来收尾"的歧义。
enum class ListenOwner { None, Touch, Key };
ListenOwner s_listen_owner = ListenOwner::None;

// 实体键"按住说话"护栏（onPress/onLongPress/onRelease 三个 async 分处不同
// 轮询 tick，靠这两个闩保证语义正确）：
//   s_key_ignore_until_release —— 本次物理按压整体作废（息屏唤醒/覆盖层打开/
//     生成中打断三种早退都置位），随后到达的 hold/release async 一律失效。
//   s_key_finish_pending —— release 的 async 若先于 StartListen 到达（极端调度
//     下的兜底），标记待收，OnKeyHoldAsync 起聆听后立即消费并发送。
bool s_key_ignore_until_release = false;
bool s_key_finish_pending = false;

// ----- P0: 快捷面板 / 新对话确认 sheet / 手势 --------------------------------
constexpr int32_t kSbarPullThreshold = 60;  // 状态栏下拉呼出快捷面板的位移
// PWR_KEY 按住达此时长才进聆听。低于此的快速单击完全无反应（"按住说话"）。
// 取 260ms：高于 IOExpander 50ms 轮询的 ±50ms 抖动，也避开常见轻点时长。
constexpr uint32_t kKeyHoldToTalkMs = 260;

// 三个视图各自的状态栏（MakeSbarBase 登记），Create() 末尾统一开 EVENT_BUBBLE，
// 让 IdBox / mode / TTS 等可点击子件上的按压也能冒泡到 screen 供下拉追踪。
std::vector<lv_obj_t*> s_sbars;
bool s_sbar_pull_tracking = false;
int32_t s_sbar_pull_start_y = 0;

// 新对话确认 sheet（底部弹层，圆角24顶边）
lv_obj_t* s_sheet_root = nullptr;
lv_obj_t* s_sheet_meta_lbl = nullptr;
lv_obj_t* s_sheet_confirm_lbl = nullptr;
bool s_sheet_open = false;

// ---- Phase3：通用参数化确认 sheet（invoke-confirm 与 pin ✕ 手势共用）----
// 与新对话 sheet 同款底部弹层风格，但 title/body/confirm_label/on_confirm 由调用方参数化——
// CommandRegistry 的 confirm 级命令与 pi_card 的 pin 移除都走它，不必各自造一张 sheet。
lv_obj_t* s_confirm_sheet_root = nullptr;
lv_obj_t* s_confirm_title_lbl = nullptr;
lv_obj_t* s_confirm_body_lbl = nullptr;
lv_obj_t* s_confirm_confirm_lbl = nullptr;
bool s_confirm_sheet_open = false;
std::function<void()> s_confirm_on_confirm;

// 会话级统计（新对话 sheet 的 meta 行）：轮数 = 已发送的 prompt 数；
// 时长从会话建立（Create/NewSession）起算。
int s_session_turns = 0;
uint32_t s_session_start_ms = 0;

// 「正在生成」真值。不能拿 s_stop_btn 的 HIDDEN 标志当依据——BuildChatView
// 构建期就 ShowStopBtn() 了，首轮之前它在隐藏的 chat 视图里也算"可见"。
bool s_agent_busy = false;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
lv_obj_t* MakeRect(lv_obj_t* parent, int32_t w, int32_t h, Tok color) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    pi_theme::ApplyBg(o, color);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

lv_obj_t* MakeCircle(lv_obj_t* parent, int32_t d, Tok color) {
    lv_obj_t* o = MakeRect(parent, d, d, color);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    return o;
}

lv_obj_t* MakeRing(lv_obj_t* parent, int32_t d, Tok border_color, int32_t border_w) {
    lv_obj_t* o = lv_obj_create(parent);
    screen_strip_obj_chrome(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, border_w, LV_PART_MAIN);
    pi_theme::ApplyBorder(o, border_color);
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

void SetLabelFont(lv_obj_t* label, const lv_font_t* font, Tok color) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    pi_theme::ApplyText(label, color);
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
bool UserDraggingFeed();
void FeedScrollEndCb(lv_event_t* e);
void ScrollFeedToBottom(bool force);  // force=false 只在贴底时跟随，见定义处
void UpdateDockStat();
void StartListen(ViewState return_state, ListenOwner owner);
void FinishListenSend();
void RetryLastPrompt();
void ShowErrorBanner(const char* message);
void OpenNewSessionSheet();
void CloseNewSessionSheet();
void OpenQuickPanel();

// ---------------------------------------------------------------------------
// shared status-bar pieces
// ---------------------------------------------------------------------------

// "|pi" id box: a small filled rect standing in for the "▮" glyph (not in the
// mono font's ASCII+·° subset) + a "pi" label. Tapping it opens the
// new-session confirm sheet (P0: 不再直接 NewSession，先确认).
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

    lv_obj_t* mark = MakeRect(box, 8, 18, Tok::Accent);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    (void)mark;

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, "pi");
    SetLabelFont(lbl, &font_pi_mono_20, Tok::Tx);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(box, [](lv_event_t*) { OpenNewSessionSheet(); }, LV_EVENT_CLICKED, nullptr);
    return box;
}

// 状态栏网络指示（P1 起为真状态，不再是静态装饰）：可选 "4G" 小字 + 3 格
// 信号 + err 小点。每条状态栏一份，登记进 s_wifi_widgets，由
// RefreshWifiWidgets() 统一刷新。
lv_obj_t* BuildWifi(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    screen_strip_obj_chrome(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    // 高度自适应（原定高 14 为信号条调的，加入 17px 电量文字后会裁字）；纵向仍底对齐，
    // 信号条/err 点用 margin_bottom 抬升到与文字视觉中线对齐（光学居中）。
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(box, 2, LV_PART_MAIN);

    WifiWidget w;

    w.g4_lbl = lv_label_create(box);
    lv_label_set_text(w.g4_lbl, "4G");
    SetLabelFont(w.g4_lbl, &font_pi_mono_17, Tok::Dim);
    lv_obj_set_style_pad_right(w.g4_lbl, 4, LV_PART_MAIN);
    lv_obj_add_flag(w.g4_lbl, LV_OBJ_FLAG_HIDDEN);  // WiFi 模式隐藏

    static const int32_t kBarH[3] = {5, 9, 13};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* bar = MakeRect(box, 3, kBarH[i], Tok::Faint);  // 初始全暗，刷新落真值
        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_margin_bottom(bar, 3, LV_PART_MAIN);  // 抬到与电量文字视觉中线对齐
        w.bars[i] = bar;
    }

    w.err_dot = MakeCircle(box, 6, Tok::Err);
    lv_obj_set_style_margin_left(w.err_dot, 3, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(w.err_dot, 5, LV_PART_MAIN);
    lv_obj_add_flag(w.err_dot, LV_OBJ_FLAG_HIDDEN);

    // 信号后统一跟电量百分比（RefreshBatteryWidgets 周期落真值；三条状态栏同款）。
    w.bat_lbl = lv_label_create(box);
    lv_label_set_text(w.bat_lbl, "--%");
    SetLabelFont(w.bat_lbl, &font_pi_mono_17, Tok::Dim);
    lv_obj_set_style_pad_left(w.bat_lbl, 8, LV_PART_MAIN);

    s_wifi_widgets.push_back(w);
    return box;
}

// LVGL 线程。三条状态栏一次刷齐；渲染键（state|lit|mode）不变时跳过，
// 避免周期 tick 反复重启"连接中"的呼吸动画。
void RefreshWifiWidgets(bool force = false) {
    bool wifi_mode = mhal::network::GetType() == mhal::network::Type::WiFi;
    NetLinkState st = static_cast<NetLinkState>(s_net_link.load());

    int lit = 0;  // 已连接时的亮格数
    if (st == NetLinkState::Connected) {
        if (wifi_mode) {
            // RSSI 三档：>-60 三格、>-75 两格、否则一格
            int rssi = mhal::network::GetWifiRssi();
            lit = rssi > -60 ? 3 : (rssi > -75 ? 2 : 1);
        } else {
            // CSQ(0-31)：>20 三格、>12 两格、>5 一格、否则全暗
            int csq = mhal::network::GetSignalStrength();
            lit = csq > 20 ? 3 : (csq > 12 ? 2 : (csq > 5 ? 1 : 0));
        }
    }

    int render_key = (static_cast<int>(st) << 4) | (lit << 1) | (wifi_mode ? 1 : 0);
    if (!force && render_key == s_net_last_render)
        return;
    s_net_last_render = render_key;

    bool err = (st == NetLinkState::Down || st == NetLinkState::NoCred);
    for (auto& w : s_wifi_widgets) {
        if (wifi_mode)
            lv_obj_add_flag(w.g4_lbl, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(w.g4_lbl, LV_OBJ_FLAG_HIDDEN);

        if (err)
            lv_obj_remove_flag(w.err_dot, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(w.err_dot, LV_OBJ_FLAG_HIDDEN);

        for (int i = 0; i < 3; i++) {
            StopBreath(w.bars[i]);  // 清掉旧的"连接中"呼吸并恢复不透明
            bool on = (st == NetLinkState::Connected && i < lit);
            pi_theme::ApplyBg(w.bars[i], on ? Tok::Tx : Tok::Faint);
        }
        if (st == NetLinkState::Connecting) {
            // 连接中：中格呼吸
            pi_theme::ApplyBg(w.bars[1], Tok::Dim);
            StartBreath(w.bars[1], 600);
        }
    }
}

// 状态栏电量：GetBatterySnapshot 非阻塞原子读（sysmon 1Hz 发布），渲染键
// (level|charging|valid) 不变时跳过，避免每 tick 重排版。充电 Ok 色、≤20% Err 色。
int s_bat_last_render = -1;
void RefreshBatteryWidgets() {
    int level = 0;
    bool charging = false, discharging = false;
    bool valid = mhal::power::GetBatterySnapshot(level, charging, discharging);
    int render_key = valid ? ((level << 2) | (charging ? 2 : 0) | 1) : 0;
    if (render_key == s_bat_last_render)
        return;
    s_bat_last_render = render_key;

    char buf[8];
    if (valid)
        std::snprintf(buf, sizeof(buf), "%d%%", level);
    else
        std::snprintf(buf, sizeof(buf), "--%%");
    Tok tok = !valid ? Tok::Faint : (charging ? Tok::Ok : (level <= 20 ? Tok::Err : Tok::Dim));
    for (auto& w : s_wifi_widgets) {
        if (w.bat_lbl == nullptr) continue;
        lv_label_set_text(w.bat_lbl, buf);
        pi_theme::ApplyText(w.bat_lbl, tok);
    }
}

// 1s tick：事件 dirty 即刷；已连接后按模式周期轮询信号档位（WiFi RSSI 是
// 即取即回缓存，5s 一刷；4G CSQ 走 AT 通道，15s 一刷别刷太勤）。
void NetTick(lv_timer_t*) {
    s_net_ticks++;
    bool dirty = s_net_dirty.exchange(false);
    bool wifi_mode = mhal::network::GetType() == mhal::network::Type::WiFi;
    uint32_t period = wifi_mode ? 5 : 15;
    if (dirty || s_net_ticks % period == 0)
        RefreshWifiWidgets(dirty);
    RefreshBatteryWidgets();  // 键控跳过，无变化零开销
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

    lv_obj_t* rule = MakeRect(parent, kW, 1, Tok::Line);
    lv_obj_set_pos(rule, 0, kSbarH - 1);
    s_sbars.push_back(sbar);  // Create() 末尾统一开 EVENT_BUBBLE（状态栏下拉）
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
    unsigned faint = static_cast<unsigned>(pi_theme::Hex(Tok::Faint));
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= 2025 - 1900) {
        // ":" recolored faint, matching the design's <small> colon treatment.
        std::snprintf(buf, sizeof(buf), "%02d#%06X :#%02d", tm_info.tm_hour, faint, tm_info.tm_min);
    } else {
        std::snprintf(buf, sizeof(buf), "--#%06X :#--", faint);
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

    // pin 在场时的状态栏迷你时钟：时间 + 日期压成一行（"12:34 · THU · JUL 17"）。
    if (s_sbar_clock_lbl != nullptr) {
        static const char* kWd2[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        static const char* kMo2[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char sbuf[48];
        if (tm_info.tm_year >= 2025 - 1900) {
            std::snprintf(sbuf, sizeof(sbuf), "%02d:%02d \xc2\xb7 %s \xc2\xb7 %s %d",
                          tm_info.tm_hour, tm_info.tm_min, kWd2[tm_info.tm_wday],
                          kMo2[tm_info.tm_mon], tm_info.tm_mday);
        } else {
            std::snprintf(sbuf, sizeof(sbuf), "--:--");
        }
        lv_label_set_text(s_sbar_clock_lbl, sbuf);
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
    SetLabelFont(s_idle_model_lbl, &font_pi_mono_20, Tok::Dim);
    MakeSpacer(sbar);
    // 待机页同样不放 CTX——它是"本轮对话消耗"，只在聊天页底部坞的一行文本里
    // 显示（见 UpdateDockStat）。s_idle_ctx_* 保持 null，RenderCtxGauge/SetCtxFill
    // 均已 null-guard。与聊天页状态栏保持一致的留白。
    BuildWifi(sbar);

    // pin 在场时的迷你时钟：大时钟区整体让位给 pin 卡，时间/日期上移到状态栏顶部中央
    //（ApplyPinLayout 控制显隐，默认隐藏）。挂 s_idle_view 而非 sbar flex 行——绝对
    // 居中，不受状态栏左右内容宽度不对称影响。
    s_sbar_clock_lbl = lv_label_create(s_idle_view);
    lv_label_set_text(s_sbar_clock_lbl, "--:--");
    SetLabelFont(s_sbar_clock_lbl, &font_pi_mono_20, Tok::Tx);
    lv_obj_set_style_text_letter_space(s_sbar_clock_lbl, 2, LV_PART_MAIN);
    lv_obj_align(s_sbar_clock_lbl, LV_ALIGN_TOP_MID, 0, (kSbarH - 20) / 2);
    lv_obj_add_flag(s_sbar_clock_lbl, LV_OBJ_FLAG_HIDDEN);

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
    s_idle_mid = mid;  // DIM 防烧屏：pi_sleep 的 on_dim 钩子周期平移这只容器

    s_clock_lbl = lv_label_create(mid);
    lv_label_set_recolor(s_clock_lbl, true);
    lv_label_set_text(s_clock_lbl, "--:--");
    SetLabelFont(s_clock_lbl, &font_pi_clock_132, Tok::Tx);

    s_date_lbl = lv_label_create(mid);
    lv_label_set_text(s_date_lbl, "");
    SetLabelFont(s_date_lbl, &font_pi_mono_20, Tok::Dim);
    lv_obj_set_style_text_letter_space(s_date_lbl, 3, LV_PART_MAIN);

    UpdateIdleClock(nullptr);

    s_idle_breath = MakeCircle(mid, 12, Tok::Accent);
    lv_obj_remove_flag(s_idle_breath, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_top(s_idle_breath, 34, LV_PART_MAIN);
    StartBreath(s_idle_breath, 1600);

    lv_obj_t* hint = lv_obj_create(s_idle_view);
    s_idle_hint = hint;
    screen_strip_obj_chrome(hint);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hint, kW, kHintH);
    lv_obj_set_pos(hint, 0, kSbarH + kMidH);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t* hrule = MakeRect(s_idle_view, kW, 1, Tok::Line);
    s_idle_hrule = hrule;
    lv_obj_set_pos(hrule, 0, kSbarH + kMidH);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint, 16, LV_PART_MAIN);

    lv_obj_t* h1 = lv_label_create(hint);
    lv_label_set_text(h1, "\xe6\x8c\x89\xe4\xbd\x8f\xe8\xaf\xb4\xe8\xaf\x9d");  // "按住说话"
    SetLabelFont(h1, &font_puhui_24_4, Tok::Faint);
    lv_obj_t* h2 = lv_label_create(hint);
    lv_label_set_text(h2, "HOLD KEY / TOUCH TO TALK");
    SetLabelFont(h2, &font_pi_mono_17, Tok::Faint);
    lv_obj_set_style_text_letter_space(h2, 2, LV_PART_MAIN);

    s_clock_timer = lv_timer_create(UpdateIdleClock, 1000, nullptr);
}

// ---------------------------------------------------------------------------
// S1 -- listen view (PTT + real ASR + real-signal waveform)
// ---------------------------------------------------------------------------

// —— 波形：真实采集电平驱动 ——
// 柱高范围（s_wave_row 高 120）与感知映射；增益把语音 RMS（远低于满幅）拉到
// 可见区间。三者都是现场可调的视觉常量。
constexpr int32_t kWaveMinH = 8;      // 静默基线高度
constexpr int32_t kWaveMaxH = 112;    // 满幅高度
constexpr float kWaveGain = 8.0f;     // RMS→归一化增益
constexpr float kWaveGamma = 0.6f;    // 感知曲线（<1：小信号更显眼）

// 采集任务上下文：算本帧 RMS、做快起慢落包络、写入环形缓冲。禁阻塞。
void PushWaveLevel(const int16_t* pcm, size_t n) {
    if (s_wave_mutex == nullptr || pcm == nullptr || n == 0) return;
    uint64_t sumsq = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = pcm[i];
        sumsq += static_cast<uint64_t>(s * s);
    }
    float rms = std::sqrt(static_cast<float>(sumsq) / static_cast<float>(n));  // 0..32767
    float norm = rms / 32768.0f * kWaveGain;
    if (norm > 1.0f) norm = 1.0f;
    // 快起慢落，压抖动（env 属采集任务，单线程安全）。
    static float env = 0.0f;
    env = (norm > env) ? (env * 0.4f + norm * 0.6f) : (env * 0.8f + norm * 0.2f);
    xSemaphoreTake(s_wave_mutex, portMAX_DELAY);
    s_wave_level = env;
    xSemaphoreGive(s_wave_mutex);
}

// LVGL 线程：读"此刻电平"，用固定的"中间高两边低"包络 × 当前电平驱动所有
// 柱子——原地随音量起伏，不横向滚动。采集已停（收音等 final）时把电平朝 0
// 衰减，波形自然落回基线。
void WaveTick(lv_timer_t*) {
    if (s_wave_bars.empty() || s_wave_mutex == nullptr) return;
    xSemaphoreTake(s_wave_mutex, portMAX_DELAY);
    if (!mhal::audio_pipeline::IsCapturing()) s_wave_level *= 0.6f;
    float level = s_wave_level;
    xSemaphoreGive(s_wave_mutex);
    if (level < 0.0f) level = 0.0f;

    const float amp = std::pow(level, kWaveGamma);
    const float center = (kWaveBars - 1) / 2.0f;
    for (size_t i = 0; i < s_wave_bars.size() && i < kWaveBars; i++) {
        float d = std::fabs(static_cast<float>(i) - center) / center;       // 0 中心 .. 1 两端
        float shape = 1.0f - 0.70f * d;                                     // 中间 1，两端 ~0.30
        float jitter = 0.80f + static_cast<float>(std::rand() % 40) / 100.0f;  // 0.80..1.19 让它活
        float a = amp * shape * jitter;
        if (a > 1.0f) a = 1.0f;
        int32_t h = kWaveMinH + static_cast<int32_t>(a * (kWaveMaxH - kWaveMinH));
        lv_obj_set_height(s_wave_bars[i], h);
        lv_obj_set_style_opa(s_wave_bars[i], static_cast<lv_opa_t>(115 + a * 140), LV_PART_MAIN);
    }
}

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
    lv_obj_t* rec_dot = MakeCircle(sbar, 8, Tok::Accent);
    lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_CLICKABLE);
    s_rec_lbl = lv_label_create(sbar);
    lv_label_set_text(s_rec_lbl, "REC 0:00");
    SetLabelFont(s_rec_lbl, &font_pi_mono_20, Tok::Accent);
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

    // 柱子只建静态外形；高度/透明度由 WaveTick 从真实采集电平每帧刷新（不再
    // 用假 sin 无限动画）。初始为静默基线。
    for (int i = 0; i < kWaveBars; i++) {
        lv_obj_t* bar = MakeRect(s_wave_row, 8, kWaveMinH, Tok::Accent);
        lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
        lv_obj_set_style_opa(bar, 115, LV_PART_MAIN);
        s_wave_bars.push_back(bar);
    }

    s_asr_lbl = lv_label_create(mid);
    lv_label_set_recolor(s_asr_lbl, true);  // trailing revealed fragment -> amber
    lv_label_set_text(s_asr_lbl, "");
    lv_label_set_long_mode(s_asr_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_asr_lbl, kW - 128);
    lv_obj_set_style_text_align(s_asr_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    SetLabelFont(s_asr_lbl, &font_puhui_30_4, Tok::Tx);
    lv_obj_set_style_text_line_space(s_asr_lbl, 8, LV_PART_MAIN);

    // 微信式动作浮块：跟波形/ASR 同在中部 mid 列里居中，抬进视线中央——不再把
    // "松开发送/上滑取消"钉在屏底被手挡住的两角。pill 默认 amber，上滑越阈值
    // 由 SetListenCancelState 整体转红成"取消"态。
    s_listen_pill = lv_obj_create(mid);
    screen_strip_obj_chrome(s_listen_pill);
    lv_obj_remove_flag(s_listen_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_listen_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_listen_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_listen_pill, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_listen_pill, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(s_listen_pill, Tok::AccentDim);
    pi_theme::ApplyBg(s_listen_pill, Tok::Card);
    lv_obj_set_style_bg_opa(s_listen_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_listen_pill, 30, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_listen_pill, 14, LV_PART_MAIN);
    s_listen_pill_lbl = lv_label_create(s_listen_pill);
    lv_label_set_text(s_listen_pill_lbl, "\xe6\x9d\xbe\xe5\xbc\x80\xe5\x8f\x91\xe9\x80\x81");  // "松开发送"
    SetLabelFont(s_listen_pill_lbl, &font_puhui_24_4, Tok::Accent);
    lv_obj_set_style_text_letter_space(s_listen_pill_lbl, 2, LV_PART_MAIN);

    s_listen_cancel_hint = lv_label_create(mid);
    lv_label_set_text(s_listen_cancel_hint,
                      "^ \xe4\xb8\x8a\xe6\xbb\x91\xe5\x8f\xaf\xe5\x8f\x96\xe6\xb6\x88");  // "^ 上滑可取消"
    SetLabelFont(s_listen_cancel_hint, &font_puhui_20_4, Tok::Faint);
}

// 松开发送 <-> 上滑取消 两态切换：pill 边框/文案在 amber 与 err 间翻转，下方
// 说明行同步。物理键聆听（无手指在屏）永远停在"松开发送"态。
void SetListenCancelState(bool armed) {
    if (s_listen_pill == nullptr) return;
    pi_theme::ApplyBorder(s_listen_pill, armed ? Tok::Err : Tok::AccentDim);
    if (s_listen_pill_lbl != nullptr) {
        lv_label_set_text(s_listen_pill_lbl,
                          armed ? "\xe6\x9d\xbe\xe5\xbc\x80\xe6\x89\x8b\xe6\x8c\x87\xef\xbc\x8c\xe5\x8f\x96\xe6\xb6\x88"   // "松开手指，取消"
                                : "\xe6\x9d\xbe\xe5\xbc\x80\xe5\x8f\x91\xe9\x80\x81");                                     // "松开发送"
        SetLabelFont(s_listen_pill_lbl, &font_puhui_24_4, armed ? Tok::Err : Tok::Accent);
        lv_obj_set_style_text_letter_space(s_listen_pill_lbl, 2, LV_PART_MAIN);
    }
    if (s_listen_cancel_hint != nullptr) {
        lv_label_set_text(s_listen_cancel_hint,
                          armed ? "\xe6\x9d\xbe\xe5\xbc\x80\xe5\x8f\x96\xe6\xb6\x88 \xc2\xb7 \xe7\xa7\xbb\xe5\x9b\x9e\xe7\xbb\xa7\xe7\xbb\xad"  // "松开取消 · 移回继续"
                                : "^ \xe4\xb8\x8a\xe6\xbb\x91\xe5\x8f\xaf\xe5\x8f\x96\xe6\xb6\x88");                                          // "^ 上滑可取消"
        SetLabelFont(s_listen_cancel_hint, &font_puhui_20_4, armed ? Tok::Err : Tok::Faint);
    }
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
    // 回合中途 FLOW→ZEN：活动行上可能残留 FLOW 期间写入的 "name -> output"（含工具
    // 输出 JSON），显示出来前先清成中性文案，别让 ZEN 第一眼就泄漏。
    if (s_zen && s_act_text != nullptr) {
        lv_label_set_text(s_act_text, "working...");
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
    pi_theme::ApplyBg(s_tts_dot, s_tts_on ? Tok::Accent : Tok::Line2);
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
    SetLabelFont(s_chat_model_lbl, &font_pi_mono_20, Tok::Dim);
    MakeSpacer(sbar);
    // CTX 不再占状态栏——它是"本轮消耗"数据，和底部坞的 IN/OUT token 同族，
    // 统一挪到 dock 的一行文本里（见 UpdateDockStat）。s_chat_ctx_* 保持 null，
    // RenderCtxGauge/SetCtxFill 均已 null-guard。状态栏腾出的位置刻意留白。
    lv_obj_t* mode_btn = lv_obj_create(sbar);
    screen_strip_obj_chrome(mode_btn);
    lv_obj_remove_flag(mode_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mode_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(mode_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(mode_btn, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(mode_btn, Tok::Line);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(mode_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(mode_btn, 4, LV_PART_MAIN);
    lv_obj_add_flag(mode_btn, LV_OBJ_FLAG_CLICKABLE);
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
    for (int i = 0; i < 3; i++)
        MakeRect(flow_icon, 14, 2, Tok::Faint);
    s_mode_icon_flow = flow_icon;

    // ZEN icon: a small ring ("◎" isn't in the mono subset).
    lv_obj_t* zen_icon = MakeRing(mode_btn, 12, Tok::Faint, 2);
    lv_obj_add_flag(zen_icon, LV_OBJ_FLAG_HIDDEN);
    s_mode_icon_zen = zen_icon;

    s_mode_lbl = lv_label_create(mode_btn);
    lv_label_set_text(s_mode_lbl, "FLOW");
    SetLabelFont(s_mode_lbl, &font_pi_mono_17, Tok::Faint);
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
    lv_obj_set_flex_flow(tts_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tts_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tts_btn, 6, LV_PART_MAIN);
    s_tts_dot = MakeCircle(tts_btn, 8, Tok::Accent);
    lv_obj_remove_flag(s_tts_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* tts_lbl = lv_label_create(tts_btn);
    lv_label_set_text(tts_lbl, "TTS");
    SetLabelFont(tts_lbl, &font_pi_mono_17, Tok::Faint);
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
// 用户消息：靠右的静默卡片（Tok::Card 底 + 1px Tok::Line2 边 + 圆角），与
// AI 全宽靠左的 markdown 形成"左右分区 + 有框/无框"的强区分。卡片内容自适应
// 宽度、超过约 74% 视口宽才换行；短句不顶满。原来的内联"YOU"标签取消，顺带
// 消掉了它与中文正文基线不齐的老问题。
void AppendUserRow(const std::string& text) {
    lv_obj_t* row = lv_obj_create(s_feed);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    // 主轴靠右：卡片贴右边缘，代表"从右侧发出"。
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* card = lv_obj_create(row);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(card, LV_SIZE_CONTENT);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(card, (kW - 32 * 2) * 74 / 100, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(card, Tok::Line2);
    pi_theme::ApplyBg(card, Tok::Card);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(card, 14, LV_PART_MAIN);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(t, (kW - 32 * 2) * 74 / 100 - 44, LV_PART_MAIN);
    lv_label_set_text(t, text.c_str());
    SetLabelFont(t, &font_puhui_30_4, Tok::Tx);
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
    pi_theme::ApplyBorder(row, Tok::Err);
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
    SetLabelFont(msg, &font_puhui_20_4, Tok::Err);

    if (!s_last_user_prompt.empty()) {
        lv_obj_t* retry = lv_label_create(row);
        lv_label_set_text(retry, "\xe9\x87\x8d\xe8\xaf\x95");  // "重试"
        SetLabelFont(retry, &font_puhui_20_4, Tok::Accent);
        lv_obj_add_flag(retry, LV_OBJ_FLAG_CLICKABLE);
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

    lv_obj_t* dot = MakeRing(row, 10, Tok::AccentDim, 1);
    s_cur_think_dot = dot;
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, "thinking");
    SetLabelFont(lbl, &font_pi_mono_14, Tok::Faint);
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
    pi_theme::ApplyBorder(card, Tok::Line);
    pi_theme::ApplyBg(card, Tok::Card);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
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

    lv_obj_t* dot = MakeCircle(head, 10, Tok::Accent);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    StartBreath(dot, 500);
    s_cur_tool_dot = dot;

    lv_obj_t* fn = lv_label_create(head);
    lv_label_set_text(fn, name);
    SetLabelFont(fn, &font_pi_mono_17, Tok::Tx);
    s_cur_tool_fn_lbl = fn;

    MakeSpacer(head);
    lv_obj_t* ret = lv_label_create(head);
    lv_label_set_text(ret, "RUNNING");
    SetLabelFont(ret, &font_pi_mono_17, Tok::Accent);
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
    lv_obj_t* body_rule = MakeRect(body, 1, 1, Tok::Line);
    lv_obj_set_width(body_rule, LV_PCT(100));
    lv_obj_align(body_rule, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* args = lv_label_create(body);
    lv_label_set_recolor(args, true);
    lv_label_set_long_mode(args, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(args, LV_PCT(100));
    char args_buf[48];
    std::snprintf(args_buf, sizeof(args_buf), "#%06X args#    (streaming...)",
                  static_cast<unsigned>(pi_theme::Hex(Tok::Faint)));
    lv_label_set_text(args, args_buf);
    SetLabelFont(args, &font_pi_mono_14, Tok::Dim);
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
    lv_obj_t* mark = MakeRect(partial_row, 6, 16, Tok::Faint);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* partial_lbl = lv_label_create(partial_row);
    lv_label_set_text(partial_lbl, "partial \xe7\xad\x89\xe5\xbe\x85\xe6\xb5\x81\xe5\xbc\x8f\xe8\xbf\x94\xe5\x9b\x9e");
    SetLabelFont(partial_lbl, &font_puhui_20_4, Tok::Faint);
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

    s_act_dot = MakeCircle(s_act_line, 8, Tok::Accent);
    lv_obj_remove_flag(s_act_dot, LV_OBJ_FLAG_CLICKABLE);
    StartBreath(s_act_dot, 550);

    s_act_text = lv_label_create(s_act_line);
    lv_label_set_text(s_act_text, "connecting...");
    SetLabelFont(s_act_text, &font_pi_mono_17, Tok::Faint);
    lv_obj_set_style_text_letter_space(s_act_text, 1, LV_PART_MAIN);

    MakeSpacer(s_act_line);
    s_act_peek = lv_label_create(s_act_line);
    lv_label_set_text(s_act_peek, "");
    // Contains CJK ("查看过程"/"收起"): must use the CJK font, not the
    // ASCII+·°-only mono subset, even though the design calls this row out
    // as mono-styled -- see the font-subset note atop this file.
    SetLabelFont(s_act_peek, &font_puhui_20_4, Tok::AccentDim);
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
            pi_theme::ApplyBg(s_cur_tool_dot, Tok::Ok);
            char ret[64];
            std::snprintf(ret, sizeof(ret), "%s \xc2\xb7 %s", tc.output.c_str(),
                          FormatSecs1(tc.elapsed_ms / 1000.0f).c_str());
            lv_label_set_text(s_cur_tool_ret_lbl, ret);
            SetLabelFont(s_cur_tool_ret_lbl, &font_pi_mono_17, Tok::Ok);
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
void StartListen(ViewState return_state, ListenOwner owner);
void CancelListen();
void FinishListenSend();

// Shared PTT gesture handlers -- registered on both s_ptt_layer (idle/listen
// zone) and the chat dock's TALK button (continue-conversation case).
// user_data carries the ViewState to return to on cancel/send (Idle or
// Chat); hiding a pressed object (never deleting it) keeps LVGL delivering
// PRESSING/RELEASED to the same target, so this works unchanged even though
// StartListen() swaps which view is visible mid-gesture.
void PttCancelHoldTimer() {
    if (s_ptt_hold_timer != nullptr) { lv_timer_delete(s_ptt_hold_timer); s_ptt_hold_timer = nullptr; }
}

// 触屏按住达阈值：此刻才真正进聆听（此前只是 arm，快速轻点不会闪进）。
void PttHoldFire(lv_timer_t*) {
    s_ptt_hold_timer = nullptr;  // 一次性 timer：触发后 LVGL 自删，仅清指针
    if (!s_ptt_tracking) return;  // 已松开/取消
    StartListen(s_ptt_ret, ListenOwner::Touch);
    s_ptt_listening = true;
}

void OnPttPressed(lv_event_t* e) {
    // 实体键已在聆听（owner=Key）时，触屏按压不抢占，避免交叉打断。
    if (s_state == ViewState::Listen) return;
    s_ptt_ret = static_cast<ViewState>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    lv_indev_t* indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_ptt_start_y = p.y;
    s_ptt_start_x = p.x;
    s_ptt_tracking = true;
    s_ptt_listening = false;
    // 不立即进聆听：按住达 kTouchHoldToTalkMs 才由 PttHoldFire 进（轻点无反应）。
    PttCancelHoldTimer();
    s_ptt_hold_timer = lv_timer_create(PttHoldFire, kTouchHoldToTalkMs, nullptr);
    lv_timer_set_repeat_count(s_ptt_hold_timer, 1);
}

void OnPttPressing(lv_event_t* e) {
    if (!s_ptt_tracking) return;
    lv_indev_t* indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_ptt_start_y;
    int32_t dx = p.x - s_ptt_start_x;
    bool up = dy < -kSwipeCancelThreshold;
    if (!s_ptt_listening) {
        // 还没真正进聆听（按住不足阈值时长）：上滑直接撤销待起的聆听。
        if (up) {
            s_ptt_tracking = false;
            lv_indev_wait_release(indev);
            PttCancelHoldTimer();
            return;
        }
        // 横向占优且越过阈值：这是一次滑动（可能是边缘导航），不是按住说话。撤销
        // 待起的聆听但**不** wait_release——wait_release 会让 indev 停摆到松手，
        // 边缘导航的 GESTURE 快路径与 RELEASED 兜底会被双杀（indev 级 edge-nav 也
        // 依赖这两个事件，见 screen_util）。松手时 OnPttReleased 因
        // s_ptt_tracking=false 而不发送，手势交给 edge-nav 层判定。
        if (LV_ABS(dx) > kHSwipeDisarmPx && LV_ABS(dx) > LV_ABS(dy)) {
            s_ptt_tracking = false;
            PttCancelHoldTimer();
        }
        return;
    }
    // 已在录音：微信式"预备取消"——上滑越阈值只是 arm（pill 转红提示），松手
    // 才真正取消；手指移回阈值内则 disarm，回到"松开发送"。不再一越阈值就立刻
    // 取消，给用户反悔的机会。
    if (up != s_ptt_cancel_armed) {
        s_ptt_cancel_armed = up;
        SetListenCancelState(up);
    }
}

void OnPttReleased(lv_event_t* e) {
    (void)e;
    if (!s_ptt_tracking) return;
    s_ptt_tracking = false;
    if (!s_ptt_listening) {   // 未过阈值：快速轻点 → 无反应
        PttCancelHoldTimer();
        return;
    }
    s_ptt_listening = false;
    if (s_listen_owner == ListenOwner::Touch) {
        if (s_ptt_cancel_armed)
            CancelListen();      // 上滑预备取消态松手：丢弃
        else
            FinishListenSend();  // 常规松手：发送
    }
    s_ptt_cancel_armed = false;
}

void BuildDock(lv_obj_t* parent) {
    lv_obj_t* dock = lv_obj_create(parent);
    screen_strip_obj_chrome(dock);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dock, kW, kDockH);
    lv_obj_set_pos(dock, 0, kSbarH + kFeedH);
    lv_obj_set_style_bg_opa(dock, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t* rule = MakeRect(parent, kW, 1, Tok::Line);
    lv_obj_set_pos(rule, 0, kSbarH + kFeedH);
    lv_obj_set_style_pad_left(dock, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dock, 28, LV_PART_MAIN);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dock, 24, LV_PART_MAIN);

    s_dock_stat_lbl = lv_label_create(dock);
    lv_label_set_text(s_dock_stat_lbl, "");
    SetLabelFont(s_dock_stat_lbl, &font_pi_mono_17, Tok::Faint);
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
    pi_theme::ApplyBorder(s_stop_btn, Tok::Line2);
    pi_theme::ApplyBg(s_stop_btn, Tok::Card);
    lv_obj_set_style_bg_opa(s_stop_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_stop_btn, 30, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_stop_btn, 18, LV_PART_MAIN);
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s_stop_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_stop_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_stop_btn, 14, LV_PART_MAIN);
    lv_obj_t* stop_icon = MakeRect(s_stop_btn, 16, 16, Tok::Err);
    lv_obj_set_style_radius(stop_icon, 3, LV_PART_MAIN);
    lv_obj_t* stop_lbl = lv_label_create(s_stop_btn);
    lv_label_set_text(stop_lbl, "STOP \xc2\xb7 \xe7\x9f\xad\xe6\x8c\x89 KEY");  // "STOP · 短按 KEY"
    // Mixed ASCII+CJK: needs the CJK font (mono has no "短按" glyphs).
    SetLabelFont(stop_lbl, &font_puhui_24_4, Tok::Tx);
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
    pi_theme::ApplyBorder(s_talk_btn, Tok::AccentDim);
    pi_theme::ApplyBg(s_talk_btn, Tok::Card);
    lv_obj_set_style_bg_opa(s_talk_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_talk_btn, 34, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_talk_btn, 18, LV_PART_MAIN);
    lv_obj_add_flag(s_talk_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_talk_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(s_talk_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_talk_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_talk_btn, 14, LV_PART_MAIN);
    MakeRing(s_talk_btn, 14, Tok::Accent, 2);
    lv_obj_t* talk_lbl = lv_label_create(s_talk_btn);
    lv_label_set_text(talk_lbl, "\xe6\x8c\x89\xe4\xbd\x8f\xe8\xaf\xb4\xe8\xaf\x9d");  // "按住说话"
    SetLabelFont(talk_lbl, &font_puhui_24_4, Tok::Accent);  // CJK text needs the CJK font
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
    lv_obj_add_event_cb(s_feed, FeedScrollEndCb, LV_EVENT_SCROLL_END, nullptr);  // 维护贴底跟随
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
// Snapshot of the CURRENT pi_theme palette -- rebuild (not cache) on every
// call: MdView bakes colors into widgets/recolor markup, so the theme
// listener re-themes live views with a fresh snapshot instead of relying on
// the shared-style flip.
lvmd::MdTheme PiMdTheme() {
    using pi_theme::Hex;
    const bool light = pi_theme::IsLight();
    lvmd::MdTheme t = lvmd::MdThemeDefaultDark();
    t.body = &font_puhui_30_4;
    t.heading = &font_puhui_30_4;
    t.mono = &font_pi_mono_17;
    t.mono_cjk = &font_puhui_20_4;  // code containing non-ASCII: readable beats monospaced
    t.code_info_font = &font_pi_mono_14;
    t.text = Hex(Tok::Tx);
    // bold/italic have no Tok: no CJK bold/italic glyphs exist, so both are
    // emphasis-by-color -- accent pulled toward Tx (dark: lighter amber,
    // light: deeper amber) and full-contrast white/black respectively.
    t.bold = light ? 0x7A4E00 : 0xFFD584;
    t.italic = light ? 0x000000 : 0xFFFFFF;
    t.strike = Hex(Tok::Faint);  // per-span strikethrough is impossible; deleted = faded
    t.inline_code = Hex(Tok::Accent);
    t.link = Hex(Tok::Accent);
    t.task_done_text = Hex(Tok::Dim);
    t.table_header = Hex(Tok::AccentDim);
    t.heading_color[0] = Hex(Tok::Accent);
    t.heading_color[1] = Hex(Tok::Accent);
    t.heading_color[2] = Hex(Tok::AccentDim);
    t.quote_text = Hex(Tok::Dim);
    t.quote_bar = Hex(Tok::AccentDim);
    t.rule = Hex(Tok::Line);
    t.marker = Hex(Tok::Accent);
    t.code_text = Hex(Tok::Tx);
    t.code_bg = Hex(Tok::Card);
    t.code_border = Hex(Tok::Line);
    t.code_info = Hex(Tok::Faint);
    return t;
}

void EnsureMdView() {
    if (s_cur_md != nullptr) return;
    s_cur_md = lvmd::MdView::Create(s_feed, PiMdTheme(), kW - 32 * 2);
    s_md_views.push_back(s_cur_md);
    // The MdView deletes itself with its root (ClearFeed / screen delete);
    // drop our references the same moment so retheme/finalize never touch a
    // dead view.
    lv_obj_add_event_cb(
        s_cur_md->root(),
        [](lv_event_t* e) {
            auto* v = static_cast<lvmd::MdView*>(lv_event_get_user_data(e));
            s_md_views.erase(std::remove(s_md_views.begin(), s_md_views.end(), v), s_md_views.end());
            if (s_cur_md == v) s_cur_md = nullptr;
        },
        LV_EVENT_DELETE, s_cur_md);
}

// ---------------------------------------------------------------------------
// TTS 朗读文本提取：把流式 markdown 剥成"渲染后纯文本"再按句喂 TTS，使朗读内容
// == 屏幕显示内容，并根治 ** / # / URL 被读出来。复用渲染 UI 那套解析器
//（lvmd::StreamParser + lvmd::ParseInline），与显示各持一份、喂同样的 delta、
// 输出一致。会阻塞的 volc_tts 调用都在 pi_agent_task 的 pump 任务上，这里全非阻塞。
// ---------------------------------------------------------------------------
lvmd::StreamParser s_tts_parser;
std::string s_tts_closed;  // 已闭合块的纯文本（每块后加 '\n' 作软边界），append-only
size_t s_tts_fed = 0;      // 已喂 TTS 的字节游标（相对 full = s_tts_closed + open block）

// 一个块的朗读纯文本（块级前缀已被 StreamParser 剥掉；行内符号由 ParseInline 剥）。
// 代码块/分割线/表格行不朗读，返回空串。
std::string TtsBlockPlain(const lvmd::Block& b) {
    if (b.type == lvmd::BlockType::kFence || b.type == lvmd::BlockType::kRule ||
        b.type == lvmd::BlockType::kTableRow) {
        return std::string();
    }
    std::string out;
    std::string_view t = b.text;
    size_t start = 0;
    for (;;) {
        size_t nl = t.find('\n', start);
        std::string_view line =
            (nl == std::string_view::npos) ? t.substr(start) : t.substr(start, nl - start);
        for (const lvmd::Span& sp : lvmd::ParseInline(line)) out += sp.text;
        if (nl == std::string_view::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

// full[from:] 里最后一个句/子句边界之后的字节位置（找不到则返回 from）。边界含中英文
// 句末标点、分号、换行，以及逗号（逗号作软停顿，降低首句发声延迟）。只喂到边界前的
// 完整片段，绝不喂尾部——避免把还没闭合的行内标记（如半个 **）当字面读出来。
size_t TtsLastBoundary(const std::string& full, size_t from) {
    static const char* const kEnders[] = {"\xe3\x80\x82" /*。*/, "\xef\xbc\x81" /*！*/,
                                          "\xef\xbc\x9f" /*？*/, "\xef\xbc\x9b" /*；*/,
                                          "\xef\xbc\x8c" /*，*/, "\xe3\x80\x81" /*、*/,
                                          "\n", ".", "!", "?", ";", ","};
    size_t best = from;
    for (const char* e : kEnders) {
        size_t pos = full.rfind(e);
        if (pos == std::string::npos || pos < from) continue;
        size_t after = pos + std::strlen(e);
        if (after > best) best = after;
    }
    return best;
}

void TtsExtractReset() {
    s_tts_parser = lvmd::StreamParser{};
    s_tts_closed.clear();
    s_tts_fed = 0;
}

// 消费已闭合块的纯文本累积到 s_tts_closed（各块后加 '\n' 软边界，使标题/段落各自成句）。
void TtsDrainClosed(std::vector<lvmd::Block>&& blocks) {
    for (const lvmd::Block& b : blocks) {
        std::string p = TtsBlockPlain(b);
        if (!p.empty()) {
            s_tts_closed += p;
            s_tts_closed += '\n';
        }
    }
}

// 喂一段原始 markdown delta：更新解析器，把新形成的完整句喂给 TTS pump。
void TtsExtractFeed(const char* delta) {
    if (delta == nullptr || delta[0] == '\0') return;
    s_tts_parser.Feed(delta);
    TtsDrainClosed(s_tts_parser.TakeClosed());
    const lvmd::Block* open = s_tts_parser.Open();
    std::string full = s_tts_closed;
    if (open != nullptr) full += TtsBlockPlain(*open);
    size_t b = TtsLastBoundary(full, s_tts_fed);
    if (b > s_tts_fed) {
        pi_agent_task_tts_feed(full.substr(s_tts_fed, b - s_tts_fed).c_str());
        s_tts_fed = b;
    }
}

// 回复结束：强制收尾解析器，把尾部残余（无终止符的最后一句）冲刷出去。
void TtsExtractFlush() {
    TtsDrainClosed(s_tts_parser.Finish());
    if (s_tts_closed.size() > s_tts_fed) {
        pi_agent_task_tts_feed(s_tts_closed.substr(s_tts_fed).c_str());
        s_tts_fed = s_tts_closed.size();
    }
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
    s_out_tokens_known = false;
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
    pi_theme::ApplyBg(s_act_dot, Tok::Accent);
    StartBreath(s_act_dot, 550);
    lv_label_set_text(s_act_peek, "");
    UpdateDockStat();
    ShowStopBtn();
}

// 用户松手（或甩滑的惯性结束）时记一次：视口还贴着底吗？
//
// 必须只认**手指驱动**的滚动。程序化滚动里，非流式那条走 ANIM_ON（见 ScrollFeedToBottom），
// LVGL 会在滚动动画结束时照样发 SCROLL_END——而那一刻内容往往又长了一截，scroll_bottom 已经
// 大于阈值，于是跟随态被误判成「用户翻上去了」，此后再不跟随（sim 实测过这个坑：scroll_y 卡在
// 189 一动不动，新输出全部不跟）。lv_indev_active() 为空即非用户操作，直接忽略。
// 用户的手指此刻是否正在滚 feed（含松手后的惯性——LVGL 在 throw 走完前一直把 scroll_obj
// 指着它）。这是区分「用户滚动」与「程序化滚动」的唯一可靠办法：SCROLL / SCROLL_END 事件里
// lv_indev_active() **恒为 null**（LVGL 发这些事件时已不在 indev 上下文里），sim 实测确认过，
// 别再试图用它来判断。
bool UserDraggingFeed() {
    for (lv_indev_t* d = lv_indev_get_next(nullptr); d != nullptr; d = lv_indev_get_next(d)) {
        if (lv_indev_get_scroll_obj(d) == s_feed) return true;
    }
    return false;
}

// 用户松手（含甩滑惯性走完）时记一次：视口还贴着底吗？
//
// 必须挡掉程序化滚动的收尾：非流式那条走 ANIM_ON（见 ScrollFeedToBottom），LVGL 会在滚动动画
// 结束时照样发 SCROLL_END——而那一刻内容往往又长了一截，bottom 已经大于阈值，于是跟随态被误判
// 成「用户翻上去了」，此后再也不跟随。
void FeedScrollEndCb(lv_event_t*) {
    if (s_feed == nullptr || !UserDraggingFeed()) return;
    s_feed_stick = lv_obj_get_scroll_bottom(s_feed) <= kFeedStickyPx;
}

// force=true：用户自己的动作（发消息/重试/错误横幅）——无条件回到底部，并恢复跟随。
// force=false：模型侧的输出（流式回复、卡片渲染）——只在用户本来就贴底时才跟。
void ScrollFeedToBottom(bool force) {
    if (s_feed == nullptr) return;
    // scroll_to_view(last child) only aligns the child's TOP edge once it is
    // taller than the viewport -- a streaming MdView reply quickly is -- so
    // follow the stream by scrolling to the real content bottom instead.
    // This update_layout is the tick's ONE forced layout pass; MdView's
    // SyncCursor below piggybacks on it —— 故它必须**无条件**跑在 sticky 判断之前，
    // 哪怕这次不滚。
    lv_obj_update_layout(s_feed);
    if (force) {
        s_feed_stick = true;  // 用户主动发言/重试 = 重新跟上直播
    } else if (!s_feed_stick) {
        return;  // 用户正在上面看历史——别抢他的视口
    }
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
// pi_card 声明式 UI 卡片的消息流接入点（pi_card::SetFeedHooks 注册；LVGL 线程）。
// 在 feed 末尾建一个全宽透明行给卡片渲染，并把常驻 act_line 钉回最后（同 tool
// card 的插入惯例）；渲染完滚到底。卡片 root 的 depth0 会自带卡片外观，故这里
// 的行仅作占位包装、透明。ClearFeed 会连同删除，卡片经 root 的 DELETE 自清理。
// ---------------------------------------------------------------------------
lv_obj_t* CardBeginRow() {
    if (s_feed == nullptr) return nullptr;
    lv_obj_t* row = lv_obj_create(s_feed);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_move_to_index(s_act_line, lv_obj_get_child_count(s_feed) - 1);
    return row;
}
void CardEndRow() { ScrollFeedToBottom(false); }  // 卡片是模型的输出 → 跟随，不抢

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
    float secs = (lv_tick_get() - s_tool_start_ms) / 1000.0f;
    if (s_cur_tool_ret_lbl != nullptr) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "RUNNING %s", FormatSecs1(secs).c_str());
        lv_label_set_text(s_cur_tool_ret_lbl, buf);
    }
    // ZEN：活动行只给中性活性（无工具名/无输出——ZEN 契约是过程完全收起，事后 peek）。
    if (s_zen && s_act_text != nullptr) {
        char zb[48];
        std::snprintf(zb, sizeof(zb), "working \xc2\xb7 %s", FormatSecs1(secs).c_str());
        lv_label_set_text(s_act_text, zb);
    }
}

// 单行消耗簇：IN 输入 token · OUT 输出 token · CTX 上下文占用率。三者都在 DONE
// 上报真实 pi_usage_t 前显示 "--"（不编造）：流式不携带真实 token 计数，OUT 尤
// 其不能拿流式序号充数（bridge 契约里 i2 已保留不发序号），CTX 也在拿到真实占用
// 率前显示 "--"。ttfb（首字延迟）是开发指标、对使用无意义，已按用户反馈去掉；CTX
// 也不再画进度条，跟 IN/OUT 一样只写数字/百分比（原 "^"/"v" 亦改为直白的 IN/OUT）。
void UpdateDockStat() {
    if (s_dock_stat_lbl == nullptr) return;
    char stat[80];
    char in_part[16];
    char out_part[16];
    char ctx_part[16];
    if (s_in_tokens_known) {
        std::snprintf(in_part, sizeof(in_part), "%d", s_in_tokens);
    } else {
        std::snprintf(in_part, sizeof(in_part), "--");
    }
    if (s_out_tokens_known) {
        std::snprintf(out_part, sizeof(out_part), "%d", s_out_tokens);
    } else {
        std::snprintf(out_part, sizeof(out_part), "--");
    }
    if (s_ctx_known && pi_agent_context_window() > 0) {
        std::snprintf(ctx_part, sizeof(ctx_part), "%d%%", s_ctx_pct);
    } else {
        std::snprintf(ctx_part, sizeof(ctx_part), "--");
    }
    std::snprintf(stat, sizeof(stat), "IN %s \xc2\xb7 OUT %s tok \xc2\xb7 CTX %s", in_part,
                  out_part, ctx_part);
    lv_label_set_text(s_dock_stat_lbl, stat);
}

void DrainQueueTick(lv_timer_t*) {
    pi_ui_evt_t evt;
    QueueHandle_t q = pi_ui_queue();
    std::string batched_text;
    int budget = 64;
    bool touched = false;
    bool stat_dirty = false;
    // 会话代次过滤：new_session 后旧 run 仍在 unwind，其残余 TEXT_DELTA/TOOL_*
    // 携带旧代次——直接丢弃，不污染新会话（也避免触碰已被 ClearFeed 清空的游标）。
    uint32_t cur_gen = pi_agent_task_session_gen();
    while (budget-- > 0 && xQueueReceive(q, &evt, 0) == pdTRUE) {
        if (evt.gen != cur_gen) {
            if (evt.s1 != nullptr)
                free(evt.s1);
            if (evt.s2 != nullptr)
                free(evt.s2);
            if (evt.s3 != nullptr)  // 卡片事件的 s3（root/props JSON）也需回收，别漏
                free(evt.s3);
            continue;
        }
        touched = true;
        switch (evt.kind) {
            case UI_AGENT_START:
                s_agent_busy = true;
                ShowStopBtn();
                pi_agent_task_tts_clear_cut();  // 新 run 自带 run_start，无需也不该再切
                TtsExtractReset();              // 新回复：重置朗读文本提取器
                pi_agent_task_tts_run_start();  // 开启本 run 的 TTS 缓冲
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
                // ZEN 不透出工具名（"完全不展示工具调用"契约）；FLOW 的活动行本就隐藏，
                // 写名字只为将来调试可见性，保持原样。
                lv_label_set_text(s_act_text, s_zen ? "working..." : name);
                s_tool_start_ms = lv_tick_get();
                if (s_tool_running_timer == nullptr) {
                    s_tool_running_timer = lv_timer_create(ToolRunningTimerTick, 100, nullptr);
                }
                break;
            }
            case UI_TOOL_ARGS:
                if (!s_zen && s_cur_tool_body_args_lbl != nullptr && evt.s1 != nullptr) {
                    char buf[320];
                    std::snprintf(buf, sizeof(buf), "#%06X args#    %s",
                                  static_cast<unsigned>(pi_theme::Hex(Tok::Faint)), evt.s1);
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
                    pi_theme::ApplyBg(s_cur_tool_dot, Tok::Ok);
                    char ret[128];
                    std::snprintf(ret, sizeof(ret), "%s \xc2\xb7 %s", output,
                                  FormatSecs1(evt.i1 / 1000.0f).c_str());
                    lv_label_set_text(s_cur_tool_ret_lbl, ret);
                    SetLabelFont(s_cur_tool_ret_lbl, &font_pi_mono_17, Tok::Ok);
                    // Card stays in the feed as history; only the RUNNING
                    // footer line goes away, not the whole body (args stay
                    // readable if the user has it expanded).
                    if (s_cur_tool_body_partial_row != nullptr) {
                        lv_obj_add_flag(s_cur_tool_body_partial_row, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                char act[160];
                if (s_zen) {
                    // ZEN 泄漏修复：原来这里无条件写 "name -> output"，而 ui_render/stock 的
                    // output 是大段 JSON，在 ZEN 可见的活动行上整坨铺开＝工具结果照样展示。
                    // ZEN 只给中性完成信号；完整过程仍进 s_tool_cache 供回合结束后 peek。
                    std::snprintf(act, sizeof(act), "done \xc2\xb7 %s",
                                  FormatSecs1(evt.i1 / 1000.0f).c_str());
                } else {
                    std::snprintf(act, sizeof(act), "%s -> %s", name, output);
                }
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
                if (evt.s1 != nullptr) {
                    batched_text += evt.s1;
                    // 卡片操作（report）注入的那一轮走 steer，没有 AGENT_START，TTS run 不会自己
                    // 重开——新文本会排在上一轮尚未播完的音频后面。**此刻**（新一轮真的吐出第一个
                    // 字）才是切旧音频的正确时机：零静音衔接。标志无论 TTS 开没开都要取走，否则
                    // 会漏到下一轮。
                    const bool cut_stale_audio = pi_agent_task_tts_take_cut();
                    // 显示不变；另外把 delta 剥成纯文本后按句喂 TTS（仅开 TTS 时）。
                    if (pi_agent_tts_enabled()) {
                        if (cut_stale_audio) {
                            // 必须是 tts_cancel：pump 此刻正卡在 volc_tts_feed_text 的限速阀里
                            // （60B/s，等音频播到 <8s 存量才放行），只在循环顶部查代次——只有
                            // stop 置的 discard_audio 能把它放出来。且 cancel 已含 run_start 的
                            // 全部语义（pending_reset + 翻代次），**绝不能**再补一次 run_start：
                            // stop worker 有代次戳守卫，代次再变会让它判 stale 直接跳过停播。
                            pi_agent_task_tts_cancel();
                            TtsExtractReset();  // 旧缓冲已作废，提取器别留半句
                        }
                        TtsExtractFeed(evt.s1);
                    }
                }
                // 流式期间没有真实输出 token 计数（bridge 契约：i2 保留不发序号），
                // 故不更新 s_out_tokens——dock 的 OUT 显示占位 "-- tok"，直到 DONE
                // 用真实 usage 校正。这里仍标脏，好让首个 delta 触发的 ttfb 刷上去。
                stat_dirty = true;
                break;
            case UI_DONE: {
                // Symmetric with UI_ERROR: defensively stop the think/tool
                // timers here too. Normally THINKING_END / TOOL_END already
                // deleted them, but a turn can finish while one is still open
                // (e.g. no matching END arrived) -- leaving a live timer would
                // keep ticking a stale row into the next turn.
                if (s_think_timer != nullptr) {
                    lv_timer_delete(s_think_timer);
                    s_think_timer = nullptr;
                }
                if (s_tool_running_timer != nullptr) {
                    lv_timer_delete(s_tool_running_timer);
                    s_tool_running_timer = nullptr;
                }
                if (!batched_text.empty()) {
                    AppendAssistantText(batched_text.c_str());
                    batched_text.clear();
                }
                FinalizeMdView();
                pi_agent_task_tts_clear_cut();  // run 收尾：注入轮若没产文本，标志别漏给下一轮
                TtsExtractFlush();              // 冲刷尾句
                pi_agent_task_tts_run_end();    // pump 排空后 speak_end，余音继续播完
                // Stage D 焦点仲裁兜底：回合彻底结束，安排一次去抖 Resume 检查——覆盖
                // 本轮从未真正触发 TTS 音频的情况（纯工具调用回复 / TTS 被用户关闭）；
                // 若本轮确实说了话，TTS 自己的 on_finished 早已排过一次，这次多半是
                // no-op（gen 已前进或 state 已是 Playing）。
                pi_media_focus_turn_ended();
                s_agent_busy = false;
                ShowTalkBtn();
                s_zen_turn_done = true;
                StopBreath(s_act_dot);
                pi_theme::ApplyBg(s_act_dot, Tok::Ok);
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
                s_out_tokens_known = true;
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
                pi_agent_task_tts_cancel();  // 出错即作废本 run 朗读并停播
                pi_media_focus_turn_ended();  // Stage D 兜底：回合出错也算结束，安排 Resume 检查
                s_agent_busy = false;
                ShowTalkBtn();
                break;
            }
            case UI_CARD_RENDER: {
                // 卡片即将进 feed：先冲刷/收尾当前文本流，卡片后的文本另起新流在
                // 卡片下方（与 tool card 同款正确视觉顺序）。ZEN 也照常渲染——卡片
                // 是内容而非过程，用户要的控件不该被隐藏。
                if (!batched_text.empty()) {
                    AppendAssistantText(batched_text.c_str());
                    batched_text.clear();
                }
                FinalizeMdView();
                pi_card::OnRenderEvent(evt.s1, evt.s2, evt.i1, evt.i2, evt.s3);
                break;
            }
            case UI_CARD_UPDATE:
                pi_card::OnUpdateEvent(evt.s1, evt.s3);
                break;
            case UI_CARD_CLOSE:
                pi_card::OnCloseEvent(evt.s1);
                break;
        }
        if (evt.s1 != nullptr) free(evt.s1);
        if (evt.s2 != nullptr) free(evt.s2);
        if (evt.s3 != nullptr) free(evt.s3);
    }
    if (!batched_text.empty()) AppendAssistantText(batched_text.c_str());
    if (stat_dirty) UpdateDockStat();
    if (touched) {
        // 模型侧输出 → 跟随而非强制：用户往上翻看历史时不抢他的视口。注意 update_layout 仍会
        // 无条件跑（就在 ScrollFeedToBottom 里），所以下面的 SyncCursor 照旧搭它的便车。
        ScrollFeedToBottom(false);  // the tick's single forced layout pass...
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
            if (s_pin_host != nullptr) {
                if (s_has_pin) lv_obj_remove_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case ViewState::Listen:
            s_ptt_cancel_armed = false;
            SetListenCancelState(false);  // 每次进聆听都从"松开发送"态起
            lv_obj_remove_flag(s_listen_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ptt_layer, LV_OBJ_FLAG_HIDDEN);
            if (s_pin_host != nullptr) lv_obj_add_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);
            break;
        case ViewState::Chat:
            lv_obj_remove_flag(s_chat_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ptt_layer, LV_OBJ_FLAG_HIDDEN);
            if (s_pin_host != nullptr) lv_obj_add_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);
            break;
    }
    // mini 播放条：Idle / Chat 可见，Listen 隐藏（Stage C）。
    pi_media::SetMiniBarContext(s == ViewState::Idle || s == ViewState::Chat);
}

// pin 常驻组件的待机布局：出现时大时钟区整体隐藏（时间/日期上移到状态栏中央的迷你
// 时钟），隐 breath、隐"按住说话"提示条——腾出的整片区域交给 pin host（flex 纵向居中
// 承载卡片）；Idle 态下显示 pin host；消失后完全复原（D2）。
void ApplyPinLayout(bool has) {
    s_has_pin = has;
    if (s_idle_mid != nullptr) {
        if (has) lv_obj_add_flag(s_idle_mid, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_idle_mid, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_sbar_clock_lbl != nullptr) {
        if (has) lv_obj_remove_flag(s_sbar_clock_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sbar_clock_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    // 迷你时钟占据状态栏中央，待机页模型名让位（会与之重叠；左侧 pi 标识保留）。
    if (s_idle_model_lbl != nullptr) {
        if (has) lv_obj_add_flag(s_idle_model_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_idle_model_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_idle_breath != nullptr) {
        if (has) lv_obj_add_flag(s_idle_breath, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_idle_breath, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_idle_hint != nullptr) {
        if (has) lv_obj_add_flag(s_idle_hint, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_idle_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_idle_hrule != nullptr) {
        if (has) lv_obj_add_flag(s_idle_hrule, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_idle_hrule, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pin_host != nullptr) {
        if (has && s_state == ViewState::Idle) lv_obj_remove_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// P1 -- 息屏视觉钩子（状态机本体在 pi_sleep.cc；这里只做 pi_screen 侧的
// 视觉/降耗动作）。DIM：呼吸点暂停 + 时钟容器每 60s 在 ±24px 内平移防烧屏
// （时钟字号只有 132 一档、无小号子集，字号不缩——见工作包报告）；OFF：再
// 暂停时钟 timer 与移位 timer（屏已全黑，省掉每秒的布局/渲染）。
// ---------------------------------------------------------------------------
void BurninShiftTick(lv_timer_t*) {
    if (s_idle_mid == nullptr)
        return;
    int32_t dx = static_cast<int32_t>(lv_rand(0, 48)) - 24;
    int32_t dy = static_cast<int32_t>(lv_rand(0, 48)) - 24;
    lv_obj_set_pos(s_idle_mid, dx, kSbarH + dy);
}

void ApplySleepDimVisual(bool dim) {
    if (dim) {
        if (s_idle_breath != nullptr) {
            StopBreath(s_idle_breath);
            lv_obj_add_flag(s_idle_breath, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_burnin_timer == nullptr)
            s_burnin_timer = lv_timer_create(BurninShiftTick, 60 * 1000, nullptr);
    } else {
        if (s_burnin_timer != nullptr) {
            lv_timer_delete(s_burnin_timer);
            s_burnin_timer = nullptr;
        }
        if (s_idle_mid != nullptr)
            lv_obj_set_pos(s_idle_mid, 0, kSbarH);  // 复位防烧屏位移
        if (s_idle_breath != nullptr) {
            lv_obj_remove_flag(s_idle_breath, LV_OBJ_FLAG_HIDDEN);
            StartBreath(s_idle_breath, 1600);
        }
    }
}

void ApplySleepOffVisual(bool off) {
    if (off) {
        if (s_clock_timer != nullptr)
            lv_timer_pause(s_clock_timer);
        if (s_burnin_timer != nullptr)
            lv_timer_pause(s_burnin_timer);
    } else {
        if (s_clock_timer != nullptr) {
            lv_timer_resume(s_clock_timer);
            UpdateIdleClock(nullptr);  // 亮屏瞬间时间就是对的，不等下一秒
        }
        if (s_burnin_timer != nullptr)
            lv_timer_resume(s_burnin_timer);
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
size_t s_asr_committed_bytes = VOLC_ASR_COMMITTED_UNKNOWN;  // 已定稿前缀字节数（同 s_asr_mutex）
std::string s_asr_error_text;  // guarded by s_asr_mutex
volatile bool s_asr_final_ready = false;
volatile bool s_asr_failed = false;
bool s_asr_waiting_final = false;  // LVGL 线程：已松开，等服务端 final
std::string s_asr_rendered;        // 上次渲染的文本（LVGL 线程，去重用）

void AsrLock() { xSemaphoreTake(s_asr_mutex, portMAX_DELAY); }
void AsrUnlock() { xSemaphoreGive(s_asr_mutex); }

void OnAsrDelta(const char* text, size_t committed_bytes, void*) {
    AsrLock();
    s_asr_live_text = text;
    s_asr_committed_bytes = committed_bytes;
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
                // barge-in：用户开口即打断在播 TTS。必须走 pi_agent_task_tts_cancel
                // 而非裸 volc_tts_stop——后者不会作废 TTS pump 缓冲里未播的旧文本。
                pi_agent_task_tts_cancel();
                // Stage D 焦点仲裁：开始聆听 = 立即给音乐让路（no-op 若本来就没在播）。
                pi_media_focus_asr_start();
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
                        PushWaveLevel(pcm, n);  // 顺带算电平驱动真实波形
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
                // 聆听结束：安排去抖 Resume 检查——多半会被紧随而来的 agent 回合/TTS
                // 重新 Suspend 顶掉（gen 前进），不会闪一下就恢复音乐。
                pi_media_focus_asr_ended();
                break;
            }
            case VoiceCmd::Cancel:
                if (active) {
                    active = false;
                    mhal::audio_pipeline::StopCapture();
                    volc_asr_abort();
                    pi_media_focus_asr_ended();
                }
                break;
        }
    }
}

// 惰性建 voice 基建；采集/发送路径要跑 gzip + TLS 写，栈给足。
void EnsureVoiceInfra() {
    if (s_voice_q != nullptr) return;
    s_asr_mutex = xSemaphoreCreateMutex();
    s_wave_mutex = xSemaphoreCreateMutex();
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
    if (s_wave_timer != nullptr) { lv_timer_delete(s_wave_timer); s_wave_timer = nullptr; }
}

void HandleAsrFinal() {
    std::string text;
    AsrLock();
    text = s_asr_live_text;
    s_asr_error_text.clear();
    AsrUnlock();
    s_asr_final_ready = false;
    s_asr_failed = false;  // final 已定局，teardown 期间迟到的 error 是噪声
    s_asr_waiting_final = false;
    s_listen_owner = ListenOwner::None;
    StopListenTimers();
    if (text.empty()) {  // 没说话/没听清：安静回到来处，不发空 prompt
        Go(s_return_state);
        return;
    }
    Go(ViewState::Chat);
    AppendUserRow(text);
    ResetTurnState();
    ScrollFeedToBottom(true);  // 用户刚发言 → 强制回底
    s_last_user_prompt = text;
    s_session_turns++;  // 新对话 sheet 的 meta 轮数
    s_agent_busy = true;
    pi_agent_task_send_prompt(text.c_str());
}

void HandleAsrFailure() {
    std::string msg;
    AsrLock();
    msg = s_asr_error_text;
    AsrUnlock();
    s_asr_failed = false;
    s_asr_final_ready = false;  // 错误定局后丢弃悬空的 final，防串到下次会话
    s_asr_waiting_final = false;
    s_listen_owner = ListenOwner::None;
    StopListenTimers();
    VoiceSend(VoiceCmd::Cancel);  // 兜底清理采集/半开会话（无会话时是 no-op）
    Go(ViewState::Chat);          // 错误横幅住在 chat feed 里
    ShowErrorBanner(msg.empty() ? "ASR error" : msg.c_str());
    ShowTalkBtn();
    ScrollFeedToBottom(true);  // 错误横幅必须被看见 → 强制回底
}

void AsrTick(lv_timer_t*) {
    if (s_asr_lbl == nullptr) return;
    // final 优先于 failed：final 已到说明识别成功，同 tick 并存的 error
    // 只可能是收尾噪声，不能让它抢走识别文本
    if (s_asr_final_ready) {
        HandleAsrFinal();
        return;
    }
    if (s_asr_failed) {
        HandleAsrFailure();
        return;
    }

    std::string text;
    size_t committed;
    AsrLock();
    text = s_asr_live_text;
    committed = s_asr_committed_bytes;
    AsrUnlock();
    if (text != s_asr_rendered) {
        s_asr_rendered = text;
        // 未定稿尾部琥珀高亮。有 definite 信息时高亮 [committed, end)——服务端尚
        // 未锁定、仍可能回改的分句；无信息时（sim / 无 utterances）退回旧的尾部
        // 固定 6 码点视觉（沿用假走带的 ".cur" 观感）。
        int plain_bytes;
        if (committed == VOLC_ASR_COMMITTED_UNKNOWN) {
            int total_cp = Utf8CodepointCount(text.c_str());
            int hi_start = total_cp - kAsrHighlightCodepoints;
            if (hi_start < 0) hi_start = 0;
            plain_bytes = Utf8PrefixBytes(text.c_str(), hi_start);
        } else {
            // committed 来自 utterances 拼接字节数，与 result.text 可能因标点/ITN
            // 略有出入：clamp 到文本长度，再往前吸附到 UTF-8 字符边界。
            plain_bytes = committed < text.size() ? static_cast<int>(committed)
                                                  : static_cast<int>(text.size());
            while (plain_bytes > 0 &&
                   (static_cast<unsigned char>(text[plain_bytes]) & 0xC0) == 0x80) {
                plain_bytes--;
            }
        }
        char hi_tag[12];
        std::snprintf(hi_tag, sizeof(hi_tag), "#%06X ",
                      static_cast<unsigned>(pi_theme::Hex(Tok::Accent)));
        std::string markup(text, 0, plain_bytes);
        markup += hi_tag;
        markup.append(text, plain_bytes, std::string::npos);
        markup += "#";
        lv_label_set_text(s_asr_lbl, markup.c_str());
    }
    // 键 PTT 现在有真"松开"信号（onRelease），不再需要 VAD 自动收尾/超时兜底。
}

void StartListen(ViewState return_state, ListenOwner owner) {
    s_return_state = return_state;
    s_listen_owner = owner;
    s_asr_waiting_final = false;
    s_asr_final_ready = false;
    s_asr_failed = false;
    s_asr_rendered.clear();
    EnsureVoiceInfra();
    AsrLock();
    s_asr_live_text.clear();
    s_asr_committed_bytes = VOLC_ASR_COMMITTED_UNKNOWN;
    s_asr_error_text.clear();
    AsrUnlock();
    VoiceSend(VoiceCmd::Start);  // 建连+开采集都在 voice 任务，UI 不阻塞
    if (s_asr_lbl != nullptr) lv_label_set_text(s_asr_lbl, "");
    s_rec_secs = 0;
    if (s_rec_lbl != nullptr) lv_label_set_text(s_rec_lbl, "REC 0:00");
    // 波形电平清零，从静默基线起。
    if (s_wave_mutex != nullptr) {
        xSemaphoreTake(s_wave_mutex, portMAX_DELAY);
        s_wave_level = 0.0f;
        xSemaphoreGive(s_wave_mutex);
    }
    Go(ViewState::Listen);
    if (s_asr_timer == nullptr) s_asr_timer = lv_timer_create(AsrTick, 70, nullptr);
    if (s_rec_timer == nullptr) s_rec_timer = lv_timer_create(UpdateRecTimer, 1000, nullptr);
    if (s_wave_timer == nullptr) s_wave_timer = lv_timer_create(WaveTick, 40, nullptr);
}

void CancelListen() {
    StopListenTimers();
    s_asr_waiting_final = false;
    s_listen_owner = ListenOwner::None;
    s_key_finish_pending = false;
    VoiceSend(VoiceCmd::Cancel);
    Go(s_return_state);
}

void FinishListenSend() {
    if (s_asr_waiting_final) return;  // 重复松开/自动发送竞态：只收一次
    s_asr_waiting_final = true;
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
    ScrollFeedToBottom(true);  // 用户主动重试 → 强制回底
    s_agent_busy = true;
    pi_agent_task_send_prompt(s_last_user_prompt.c_str());
}

// 清空 chat feed（act line 常驻保留）。被删子对象里可能有 s_peek_container /
// 各 s_cur_* 游标——全部置空，防 ResetTurnState 二次删除；s_cur_md 与
// s_md_views 由 MdView root 的 LV_EVENT_DELETE 回调自动清理。
void ClearFeed() {
    if (s_feed != nullptr) {
        int32_t n = static_cast<int32_t>(lv_obj_get_child_count(s_feed));
        for (int32_t i = n - 1; i >= 0; --i) {
            lv_obj_t* child = lv_obj_get_child(s_feed, i);
            if (child == s_act_line) continue;  // permanent, reused across turns
            lv_obj_delete(child);
        }
    }
    // feed 空了，视口自然贴底——必须恢复跟随，否则「用户翻到一半 → 开新会话」会把
    // 脱离态带进新会话，新回复看着像卡住不动。
    s_feed_stick = true;
    s_peek_container = nullptr;
    s_cur_think_row = s_cur_think_dot = s_cur_think_lbl = nullptr;
    s_cur_tool_card = s_cur_tool_dot = s_cur_tool_fn_lbl = nullptr;
    s_cur_tool_ret_lbl = s_cur_tool_body_args_lbl = s_cur_tool_body_partial_row = nullptr;
}

void NewSession() {
    if (s_state == ViewState::Listen)
        CancelListen();
    pi_agent_task_new_session();
    ClearFeed();
    ResetTurnState();
    ApplyCtxUnknown();
    s_session_turns = 0;
    s_session_start_ms = lv_tick_get();
    s_agent_busy = false;
    Go(ViewState::Idle);
}

// 供 CommandRegistry 的 session.new（Confirm 级 invoke 命令）确认后调用：与新对话 sheet 的
// 确认按钮同一套语义（生成中先 abort 再新建），只是入口从触屏 sheet 换成卡片 invoke 动作。
void DoNewSession() {
    if (s_agent_busy) pi_agent_task_abort();  // 生成中先 abort 再新建（同新对话 sheet 的确认按钮）
    NewSession();
}

// ---------------------------------------------------------------------------
// P0 -- 新对话确认 sheet（底部弹层）。IdBox 点按与快捷面板的「新对话」都走
// 这里；确认后才调 NewSession()。正在生成（STOP 可见）时主按钮变「停止并
// 新建」，确认时先 abort 再新建。
// ---------------------------------------------------------------------------
std::string FormatSessionDuration(uint32_t secs) {
    char buf[24];
    if (secs < 60) {
        std::snprintf(buf, sizeof(buf), "%us", static_cast<unsigned>(secs));
    } else if (secs < 3600) {
        std::snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>(secs / 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%uh%02um", static_cast<unsigned>(secs / 3600),
                      static_cast<unsigned>((secs % 3600) / 60));
    }
    return buf;
}

bool IsGenerating() { return s_agent_busy; }

lv_obj_t* MakeSheetButton(lv_obj_t* parent, Tok border_color, lv_obj_t** out_label) {
    lv_obj_t* btn = lv_obj_create(parent);
    screen_strip_obj_chrome(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(btn, 96);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(btn, border_color);
    pi_theme::ApplyBg(btn, Tok::Card);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    pi_theme::ApplyBg(btn, Tok::Card2, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "");
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    *out_label = lbl;
    return btn;
}

void BuildNewSessionSheet(lv_obj_t* parent) {
    s_sheet_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_sheet_root);
    lv_obj_remove_flag(s_sheet_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_sheet_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_sheet_root, kW, kH);
    lv_obj_set_pos(s_sheet_root, 0, 0);
    lv_obj_set_style_bg_opa(s_sheet_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_sheet_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scrim = lv_obj_create(s_sheet_root);
    screen_strip_obj_chrome(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scrim, kW, kH);
    pi_theme::ApplyScrim(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        scrim, [](lv_event_t*) { CloseNewSessionSheet(); }, LV_EVENT_CLICKED, nullptr);

    // sheet 本体：贴底、radius 24；下边缘下沉 24px 把底部圆角藏出屏外，
    // 视觉上只有"圆角24顶边"（与快捷面板顶部同一手法，方向相反）。
    lv_obj_t* sheet = lv_obj_create(s_sheet_root);
    screen_strip_obj_chrome(sheet);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sheet, kW);
    lv_obj_set_height(sheet, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(sheet, 24, LV_PART_MAIN);
    pi_theme::ApplyBg(sheet, Tok::Card);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sheet, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(sheet, Tok::Line);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);  // 挡住透传到 scrim 的点击
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(sheet, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_top(sheet, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(sheet, 28 + 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sheet, 14, LV_PART_MAIN);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 24);

    lv_obj_t* title = lv_label_create(sheet);
    // "开始新对话？"
    lv_label_set_text(title,
                      "\xe5\xbc\x80\xe5\xa7\x8b\xe6\x96\xb0\xe5\xaf\xb9\xe8\xaf\x9d\xef\xbc\x9f");
    SetLabelFont(title, &font_puhui_30_4, Tok::Tx);

    s_sheet_meta_lbl = lv_label_create(sheet);
    lv_label_set_text(s_sheet_meta_lbl, "");
    SetLabelFont(s_sheet_meta_lbl, &font_pi_mono_17, Tok::Dim);
    lv_obj_set_style_text_letter_space(s_sheet_meta_lbl, 1, LV_PART_MAIN);

    lv_obj_t* note = lv_label_create(sheet);
    // "当前对话将结束。"（P2 才有归档，本期不提找回）
    lv_label_set_text(note,
                      "\xe5\xbd\x93\xe5\x89\x8d\xe5\xaf\xb9\xe8\xaf\x9d\xe5\xb0\x86\xe7\xbb\x93\xe6"
                      "\x9d\x9f\xe3\x80\x82");
    SetLabelFont(note, &font_puhui_20_4, Tok::Faint);

    lv_obj_t* btn_row = lv_obj_create(sheet);
    screen_strip_obj_chrome(btn_row);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn_row, LV_PCT(100), 96 + 10);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 20, LV_PART_MAIN);

    lv_obj_t* cancel_lbl = nullptr;
    lv_obj_t* cancel_btn = MakeSheetButton(btn_row, Tok::Line2, &cancel_lbl);
    lv_label_set_text(cancel_lbl, "\xe5\x8f\x96\xe6\xb6\x88");  // "取消"
    SetLabelFont(cancel_lbl, &font_puhui_24_4, Tok::Dim);
    lv_obj_add_event_cb(
        cancel_btn, [](lv_event_t*) { CloseNewSessionSheet(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* confirm_btn = MakeSheetButton(btn_row, Tok::Accent, &s_sheet_confirm_lbl);
    SetLabelFont(s_sheet_confirm_lbl, &font_puhui_24_4, Tok::Accent);
    lv_obj_add_event_cb(
        confirm_btn,
        [](lv_event_t*) {
            bool generating = IsGenerating();
            CloseNewSessionSheet();
            if (generating)
                pi_agent_task_abort();
            NewSession();
        },
        LV_EVENT_CLICKED, nullptr);
}

void OpenNewSessionSheet() {
    if (s_sheet_root == nullptr || s_sheet_open)
        return;
    if (s_state == ViewState::Listen)
        CancelListen();  // 聆听中先取消再谈新会话
    if (pi_quick_panel::IsOpen())
        pi_quick_panel::Close();

    // meta 行：轮数 · CTX · 时长（mono；取不到 CTX 显示 --）
    char ctx[20];
    if (s_ctx_known && pi_agent_context_window() > 0) {
        std::snprintf(ctx, sizeof(ctx), "CTX %d%%", s_ctx_pct);
    } else {
        std::snprintf(ctx, sizeof(ctx), "CTX --");
    }
    uint32_t secs = (lv_tick_get() - s_session_start_ms) / 1000;
    char meta[96];
    std::snprintf(meta, sizeof(meta), "%d turns \xc2\xb7 %s \xc2\xb7 %s", s_session_turns, ctx,
                  FormatSessionDuration(secs).c_str());
    lv_label_set_text(s_sheet_meta_lbl, meta);

    // 正在生成 -> 主按钮改「停止并新建」；否则「+ 新对话」（✚ 不在字体子集）
    lv_label_set_text(s_sheet_confirm_lbl,
                      IsGenerating()
                          ? "\xe5\x81\x9c\xe6\xad\xa2\xe5\xb9\xb6\xe6\x96\xb0\xe5\xbb\xba"
                          : "+ \xe6\x96\xb0\xe5\xaf\xb9\xe8\xaf\x9d");

    lv_obj_remove_flag(s_sheet_root, LV_OBJ_FLAG_HIDDEN);
    s_sheet_open = true;
}

void CloseNewSessionSheet() {
    if (s_sheet_root == nullptr || !s_sheet_open)
        return;
    lv_obj_add_flag(s_sheet_root, LV_OBJ_FLAG_HIDDEN);
    s_sheet_open = false;
}

// ---------------------------------------------------------------------------
// Phase3 -- 通用参数化确认 sheet（invoke-confirm 与 pin ✕ 手势共用，见决策摘要）。
// 与新对话 sheet 同款底部弹层风格：仿 BuildNewSessionSheet，但 title/body/confirm_label/
// on_confirm 由调用方参数化，不再各自造一张 sheet。取消=无副作用；确认才跑 on_confirm。
// ---------------------------------------------------------------------------
void CloseConfirmSheet() {
    if (s_confirm_sheet_root == nullptr || !s_confirm_sheet_open)
        return;
    lv_obj_add_flag(s_confirm_sheet_root, LV_OBJ_FLAG_HIDDEN);
    s_confirm_sheet_open = false;
    s_confirm_on_confirm = nullptr;
}

void BuildConfirmSheet(lv_obj_t* parent) {
    s_confirm_sheet_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_confirm_sheet_root);
    lv_obj_remove_flag(s_confirm_sheet_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_confirm_sheet_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_confirm_sheet_root, kW, kH);
    lv_obj_set_pos(s_confirm_sheet_root, 0, 0);
    lv_obj_set_style_bg_opa(s_confirm_sheet_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_confirm_sheet_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scrim = lv_obj_create(s_confirm_sheet_root);
    screen_strip_obj_chrome(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scrim, kW, kH);
    pi_theme::ApplyScrim(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        scrim, [](lv_event_t*) { CloseConfirmSheet(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sheet = lv_obj_create(s_confirm_sheet_root);
    screen_strip_obj_chrome(sheet);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sheet, kW);
    lv_obj_set_height(sheet, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(sheet, 24, LV_PART_MAIN);
    pi_theme::ApplyBg(sheet, Tok::Card);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sheet, 1, LV_PART_MAIN);
    pi_theme::ApplyBorder(sheet, Tok::Line);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(sheet, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_top(sheet, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(sheet, 28 + 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sheet, 14, LV_PART_MAIN);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 24);

    s_confirm_title_lbl = lv_label_create(sheet);
    lv_label_set_text(s_confirm_title_lbl, "");
    SetLabelFont(s_confirm_title_lbl, &font_puhui_30_4, Tok::Tx);

    s_confirm_body_lbl = lv_label_create(sheet);
    lv_label_set_text(s_confirm_body_lbl, "");
    lv_label_set_long_mode(s_confirm_body_lbl, LV_LABEL_LONG_WRAP);
    SetLabelFont(s_confirm_body_lbl, &font_puhui_20_4, Tok::Faint);

    lv_obj_t* btn_row = lv_obj_create(sheet);
    screen_strip_obj_chrome(btn_row);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn_row, LV_PCT(100), 96 + 10);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 20, LV_PART_MAIN);

    lv_obj_t* cancel_lbl = nullptr;
    lv_obj_t* cancel_btn = MakeSheetButton(btn_row, Tok::Line2, &cancel_lbl);
    lv_label_set_text(cancel_lbl, "\xe5\x8f\x96\xe6\xb6\x88");  // "取消"
    SetLabelFont(cancel_lbl, &font_puhui_24_4, Tok::Dim);
    lv_obj_add_event_cb(
        cancel_btn, [](lv_event_t*) { CloseConfirmSheet(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* confirm_btn = MakeSheetButton(btn_row, Tok::Accent, &s_confirm_confirm_lbl);
    SetLabelFont(s_confirm_confirm_lbl, &font_puhui_24_4, Tok::Accent);
    lv_obj_add_event_cb(
        confirm_btn,
        [](lv_event_t*) {
            std::function<void()> fn = s_confirm_on_confirm;
            CloseConfirmSheet();
            if (fn) fn();
        },
        LV_EVENT_CLICKED, nullptr);
}

// 供 CommandRegistry::SetConfirmHook / pin ✕ 手势调用（均在 LVGL 线程）。
void ShowConfirmSheet(const std::string& title, const std::string& body,
                      const std::string& confirm_label, std::function<void()> on_confirm) {
    if (s_confirm_sheet_root == nullptr) return;
    if (s_state == ViewState::Listen) CancelListen();
    if (pi_quick_panel::IsOpen()) pi_quick_panel::Close();
    lv_label_set_text(s_confirm_title_lbl, title.c_str());
    lv_label_set_text(s_confirm_body_lbl, body.c_str());
    lv_label_set_text(s_confirm_confirm_lbl, confirm_label.empty() ? "\xe7\xa1\xae\xe8\xae\xa4"
                                                                   : confirm_label.c_str());  // "确认"
    s_confirm_on_confirm = std::move(on_confirm);
    lv_obj_remove_flag(s_confirm_sheet_root, LV_OBJ_FLAG_HIDDEN);
    s_confirm_sheet_open = true;
}

// ---------------------------------------------------------------------------
// P0 -- 快捷面板呼出（PWR_KEY 长按 / 状态栏下拉）与 Chat 态右滑回待机。
// ---------------------------------------------------------------------------
void OpenQuickPanel() {
    if (pi_quick_panel::IsOpen() || s_sheet_open || s_confirm_sheet_open)
        return;
    if (s_state == ViewState::Listen)
        CancelListen();  // 聆听中先取消
    pi_quick_panel::Open();
}

// 状态栏（y < kSbarH）按下向下拖 > kSbarPullThreshold px -> 呼出快捷面板。
// 靠 EVENT_BUBBLE（sbar 子树 + screen 直达按压）把 PRESSED/PRESSING 送到
// screen 对象；命中后 wait_release 抑制 IdBox/mode/TTS 等的 CLICKED。
void OnScrPressed(lv_event_t* e) {
    if (pi_quick_panel::IsOpen() || s_sheet_open || s_confirm_sheet_open || pi_settings::IsOpen())
        return;
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (p.y >= kSbarH)
        return;
    s_sbar_pull_start_y = p.y;
    s_sbar_pull_tracking = true;
}

void OnScrPressing(lv_event_t* e) {
    if (!s_sbar_pull_tracking)
        return;
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (p.y - s_sbar_pull_start_y > kSbarPullThreshold) {
        s_sbar_pull_tracking = false;
        lv_indev_wait_release(indev);
        OpenQuickPanel();
    }
}

void OnScrReleased(lv_event_t*) { s_sbar_pull_tracking = false; }

// 边缘滑动导航的策略路由（机制在 screen_util 的 indev 级 edge-nav 层：只有起点
// 落在屏幕左/右边缘带的横滑才会派发到这里，屏幕内部横滑全归控件所有，不再需要
// 旧的逐控件 screen_swipe_back_ignore 打标）。两向都只切视图、绝不销毁会话，与
// Go() 的隐藏/显示模型一致：
//   左缘右滑：设置栈打开 -> 逐级返回；Chat 态 -> 回待机（生成中忽略）。
//   右缘左滑：Idle 态 -> 回对话（要求本会话已有轮次，否则无历史可回、忽略）。
//   快捷面板 / sheet / overlay 卡片打开时一律忽略（模态语义；overlay 此前靠
//   scrim 打标豁免，现收进这里统一守卫）。
void OnEdgeNav(screen_edge_nav_dir_t dir) {
    if (pi_media::IsOpen()) {  // 全屏媒体页在最上层：左缘右滑 = 收抽屉/返回聊天（不停播）
        if (dir == SCREEN_EDGE_NAV_FROM_LEFT) pi_media::Back();
        return;
    }
    if (pi_settings::IsOpen()) {
        if (dir == SCREEN_EDGE_NAV_FROM_LEFT) pi_settings::Back();
        return;
    }
    if (pi_quick_panel::IsOpen() || s_sheet_open || s_confirm_sheet_open ||
        pi_card::HasOpenOverlay()) {
        return;
    }
    if (dir == SCREEN_EDGE_NAV_FROM_LEFT) {
        if (s_state != ViewState::Chat || IsGenerating()) return;
        Go(ViewState::Idle);
    } else {
        // Idle 右缘左滑：回到隐藏着的对话。气泡与 agent 上下文从未销毁，Go(Chat)
        // 直接复用且保留原滚动位置＝"最小化/还原"语义。刚开机/刚新建时
        // s_session_turns==0，无历史可回，忽略。
        if (s_state != ViewState::Idle || s_session_turns <= 0) return;
        Go(ViewState::Chat);
    }
}

void EnableEventBubbleRecursive(lv_obj_t* obj) {
    if (obj == nullptr)
        return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        EnableEventBubbleRecursive(lv_obj_get_child(obj, i));
    }
}

// PWR_KEY "按住说话"（IOExpander 监视任务线程 -> lv_async_call 回 LVGL 线程）：
//   onPress   -> OnKeyPressAsync：处理息屏唤醒 / 覆盖层惰性 / 生成中打断；
//                普通按下不进聆听（快速单击 = 无反应）。
//   onLongPress(kKeyHoldToTalkMs) -> OnKeyHoldAsync：按住够久才真正进聆听。
//   onRelease -> OnKeyReleaseAsync：松开即收音发送。
// 快捷面板不再由实体键呼出（只留状态栏下拉）。护栏见 s_key_ignore_until_release /
// s_key_finish_pending 的声明处。

// 覆盖层（设置栈/快捷面板/新对话 sheet）打开时，实体键 PTT 一律惰性。
bool KeyOverlayOpen() {
    return pi_media::IsOpen() || pi_settings::IsOpen() || pi_quick_panel::IsOpen() ||
           s_sheet_open || s_confirm_sheet_open;
}

void OnKeyPressAsync(void*) {
    // 每次物理按下先清两个闩（本次按压重新判定）。
    s_key_ignore_until_release = false;
    s_key_finish_pending = false;
    // P1 息屏：重置无操作计时；OFF 下按下只唤醒（返回 true），并作废本次按压
    // 后续的 hold/release，避免刚唤醒就误进聆听/误发送。DIM 唤醒后照常透传。
    if (pi_sleep::ConsumeKeyWake()) {
        s_key_ignore_until_release = true;
        return;
    }
    if (KeyOverlayOpen()) {  // 覆盖层打开：键惰性
        s_key_ignore_until_release = true;
        return;
    }
    // 生成中按下即打断（轻触，无需按住）；随后的 hold/release 作废。
    if (s_state == ViewState::Chat && s_stop_btn != nullptr &&
        !lv_obj_has_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN)) {
        pi_agent_task_abort();
        s_key_ignore_until_release = true;
        return;
    }
    // 其余情况：不在按下时进聆听——等 OnKeyHoldAsync 的按住判定，快速单击无反应。
}

void OnKeyHoldAsync(void*) {
    if (s_key_ignore_until_release || KeyOverlayOpen()) return;
    if (s_state == ViewState::Listen) return;  // 触屏已在聆听：防重入
    StartListen(s_state == ViewState::Chat ? ViewState::Chat : ViewState::Idle, ListenOwner::Key);
    if (s_key_finish_pending) {  // release 已先到（极端调度兜底）：立即收音发送
        s_key_finish_pending = false;
        FinishListenSend();
    }
}

void OnKeyReleaseAsync(void*) {
    if (s_state == ViewState::Listen && s_listen_owner == ListenOwner::Key) {
        FinishListenSend();  // 松开发送（本次聆听是键触发的）
    } else if (s_listen_owner != ListenOwner::Key && !s_key_ignore_until_release) {
        // 聆听尚未起来（hold async 还没执行）：标记待收，OnKeyHoldAsync 消费。
        s_key_finish_pending = true;
    }
    s_key_ignore_until_release = false;
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
    pi_sleep::OnScreenUnloaded();  // 恢复亮度/删 tick（在删 burnin timer 之前跑钩子无碍）
    // pi_sleep 卸载时直接回 Awake、不回调 on_off(false)——这里显式复位息屏门控，否则
    // Off 态卸载会让 DataHub/stock 的暂停态泄漏进下一次 Create（进程级单例，不随屏销毁）。
    pi_card::SetScreenOff(false);
    if (s_net_listener_id >= 0) {
        pi_net_events::RemoveListener(s_net_listener_id);
        s_net_listener_id = -1;
    }
    if (s_theme_listener_id >= 0) {
        pi_theme::RemoveListener(s_theme_listener_id);
        s_theme_listener_id = -1;
    }
    if (s_net_timer != nullptr) {
        lv_timer_delete(s_net_timer);
        s_net_timer = nullptr;
    }
    if (s_burnin_timer != nullptr) {
        lv_timer_delete(s_burnin_timer);
        s_burnin_timer = nullptr;
    }
    if (s_clock_timer != nullptr) { lv_timer_delete(s_clock_timer); s_clock_timer = nullptr; }
    if (s_asr_timer != nullptr) { lv_timer_delete(s_asr_timer); s_asr_timer = nullptr; }
    if (s_rec_timer != nullptr) { lv_timer_delete(s_rec_timer); s_rec_timer = nullptr; }
    if (s_wave_timer != nullptr) { lv_timer_delete(s_wave_timer); s_wave_timer = nullptr; }
    if (s_ptt_hold_timer != nullptr) { lv_timer_delete(s_ptt_hold_timer); s_ptt_hold_timer = nullptr; }
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
    if (s_pickup_timer != nullptr) { lv_timer_delete(s_pickup_timer); s_pickup_timer = nullptr; }

    // Drain and free anything still in flight so agent-thread mallocs never
    // leak just because the screen went away mid-turn.
    pi_ui_evt_t evt;
    while (xQueueReceive(pi_ui_queue(), &evt, 0) == pdTRUE) {
        if (evt.s1 != nullptr) free(evt.s1);
        if (evt.s2 != nullptr) free(evt.s2);
        if (evt.s3 != nullptr) free(evt.s3);  // 卡片事件的 data/props JSON，漏了就泄
    }

    s_scr = s_idle_view = s_listen_view = s_chat_view = s_ptt_layer = nullptr;
    s_feed = s_act_line = s_act_dot = s_act_text = s_act_peek = s_peek_container = nullptr;
    s_feed_stick = true;  // 别把脱离态泄漏给下一次 Create（屏卸载/重载）
    s_sbar_clock_lbl = nullptr;
    s_clock_lbl = s_date_lbl = s_idle_mid = s_idle_breath = s_idle_ctx_fill = s_idle_ctx_lbl =
        nullptr;
    s_wifi_widgets.clear();
    s_net_last_render = -1;
    s_bat_last_render = -1;
    s_idle_model_lbl = s_chat_model_lbl = nullptr;
    s_wave_row = s_asr_lbl = s_rec_lbl = nullptr;
    s_listen_pill = s_listen_pill_lbl = s_listen_cancel_hint = nullptr;
    s_ptt_cancel_armed = false;
    s_chat_ctx_fill = s_chat_ctx_lbl = nullptr;
    s_mode_icon_flow = s_mode_icon_zen = s_mode_lbl = s_tts_dot = nullptr;
    s_dock_stat_lbl = s_dock_action_box = s_stop_btn = s_talk_btn = nullptr;
    s_cur_think_row = s_cur_think_dot = s_cur_think_lbl = nullptr;
    s_cur_tool_card = s_cur_tool_dot = s_cur_tool_fn_lbl = nullptr;
    s_cur_tool_ret_lbl = s_cur_tool_body_args_lbl = s_cur_tool_body_partial_row = nullptr;
    s_cur_md = nullptr;  // the MdView frees itself when the LVGL tree is deleted
    s_md_views.clear();
    s_wave_bars.clear();
    s_tool_cache.clear();

    // P0/P1 浮层/手势状态
    pi_quick_panel::OnScreenUnloaded();
    pi_settings::OnScreenUnloaded();
    pi_media::OnScreenUnloaded();
    s_sheet_root = s_sheet_meta_lbl = s_sheet_confirm_lbl = nullptr;
    s_sheet_open = false;
    s_confirm_sheet_root = s_confirm_title_lbl = s_confirm_body_lbl = s_confirm_confirm_lbl = nullptr;
    s_confirm_sheet_open = false;
    s_confirm_on_confirm = nullptr;
    s_pin_host = nullptr;
    s_has_pin = false;
    s_idle_hint = s_idle_hrule = nullptr;
    s_sbars.clear();
    s_sbar_pull_tracking = false;
    // PTT 所有权 / 实体键护栏：防跨屏残留
    s_ptt_tracking = false;
    s_ptt_listening = false;
    s_listen_owner = ListenOwner::None;
    s_key_ignore_until_release = false;
    s_key_finish_pending = false;
    s_agent_busy = false;
}

}  // namespace

lv_obj_t* PiScreen::Create() {
    // P2：初始主题必须在构建任何控件之前定下来（NVS "ui"/"theme"），开机
    // 即目标主题、无先深后浅的闪切。
    pi_theme::Init();

    Settings settings("pi_screen", false);
    s_zen = settings.GetBool("zen_mode", false);
    s_tts_on = settings.GetBool("tts_on", true);
    pi_agent_tts_set_enabled(s_tts_on);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kW, kH);
    pi_theme::ApplyBg(scr, Tok::Bg);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // LVGL 对 screen（无 parent 的对象）默认不给 PRESS_LOCK：裸 sbar 区起手的
    // 按压一旦拖过 y=56 就会被重新命中到 ptt 层，PRESSING 断流，状态栏下拉
    // 手势永远到不了阈值。补上 PRESS_LOCK，press 锁在 screen 上直到松开。
    lv_obj_add_flag(scr, LV_OBJ_FLAG_PRESS_LOCK);

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
    // 与边缘导航天然共存：edge-nav 是 indev 级、不依赖对象冒泡，PTT 层收走按压
    // 不影响它；纯按住 dx~0 永不越导航阈值，两手势不冲突。
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

    // Phase3：常驻小组件宿主。建在 s_ptt_layer 之后（z 序压过它，pin 卡内可点击控件才能
    // 收到触摸）；非 clickable + 非 scrollable，让空白区按压穿透回 s_ptt_layer 触发 PTT（D3）。
    // 仅 Idle 且 s_has_pin 时可见（Go()/ApplyPinLayout 管理 HIDDEN），默认隐藏。
    s_pin_host = lv_obj_create(scr);
    screen_strip_obj_chrome(s_pin_host);
    lv_obj_remove_flag(s_pin_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_pin_host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_pin_host, kW, kH - kSbarH);
    lv_obj_set_pos(s_pin_host, 0, kSbarH);
    // 卡片 wrapper（宽 100%、高自适应）在整片区域内纵向居中——大时钟已让位，居中展示。
    lv_obj_set_flex_flow(s_pin_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pin_host, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_pin_host, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_pin_host, LV_OBJ_FLAG_HIDDEN);

    // Stage C：常驻 mini 播放条。建在 pin host 之后（z 序压过 ptt 层，条上按钮可收到
    // 触摸）、快捷面板/设置栈/媒体全屏页之前（那些不透明浮层自然遮住它）。
    pi_media::CreateMiniBar(scr);

    // P0 浮层：快捷面板在 ptt 层之上，新对话 sheet 再压一层（面板里的
    // 「新对话」要能弹出 sheet）。都常驻构建、HIDDEN 切换。
    pi_quick_panel::Hooks qp_hooks;
    qp_hooks.on_new_session = []() { OpenNewSessionSheet(); };
    // P1：「⚙ 设置」接线——面板已自行收起，这里直接推入设置 Hub（设置栈
    // 懒创建为 screen 的最后一个子对象，z 序压过 ptt 层/面板/sheet）。
    qp_hooks.on_settings = []() { pi_settings::Open(s_scr); };
    // 「文件管理」快捷面板一步直达（跳过设置 Hub，直接推 Files 页；WiFi 门
    // 控在 pi_quick_panel 侧已做，这里回调时必已连通）。
    qp_hooks.on_files = []() { pi_settings::OpenFiles(s_scr); };
    pi_quick_panel::Create(scr, qp_hooks);
    BuildNewSessionSheet(scr);
    BuildConfirmSheet(scr);  // Phase3：通用确认 sheet（invoke-confirm 与 pin ✕ 共用）

    // 设置栈与 pi_screen 的同源开关联动（TTS/ZEN 都走 pi_screen 的
    // SetTtsOn/SetZen，NVS 持久化与状态栏视觉一并生效）。
    pi_settings::Hooks st_hooks;
    st_hooks.get_tts = []() { return s_tts_on; };
    st_hooks.set_tts = [](bool on) { SetTtsOn(on); };
    st_hooks.get_zen = []() { return s_zen; };
    st_hooks.set_zen = [](bool zen) { SetZen(zen); };
    pi_settings::SetHooks(st_hooks);

    // 状态栏下拉（呼出快捷面板）：追踪器挂在 screen 上；三个 sbar 子树打开
    // EVENT_BUBBLE，让 IdBox 等可点击子件上的按压也可追踪。
    lv_obj_add_event_cb(scr, OnScrPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, OnScrPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(scr, OnScrReleased, LV_EVENT_RELEASED, nullptr);
    for (lv_obj_t* sbar : s_sbars)
        EnableEventBubbleRecursive(sbar);
    // 边缘滑动导航：indev 级手势层（机制），路由策略在 OnEdgeNav。须在 indev
    // 已创建后初始化（mhal::Init 的 LVGL 适配器早于 pi_screen Create，满足）。
    // 不变量：screen 保持默认 CLICKABLE——空白背景按压靠它兜底命中，indev 事件
    // 才会派发（见 screen_util.h 说明）。
    screen_edge_nav_init(OnEdgeNav);

    // P1：状态栏网络真状态。分发层一处订阅 mhal::network::OnEvent、多处
    // 监听（pi_settings 网络页共用），这里必须在 main.cc 的 StartAsync()
    // 之前注册（Create 先于起网，满足）。回调在网络栈线程：只写原子快照。
    s_net_link = static_cast<int>(mhal::network::IsConnected() ? NetLinkState::Connected
                                                               : NetLinkState::Connecting);
    s_net_listener_id = pi_net_events::AddListener([](mhal::network::Event e, const std::string&) {
        using E = mhal::network::Event;
        NetLinkState st;
        switch (e) {
            case E::WifiScanning:
            case E::WifiConnecting:
            case E::ModemDetecting:
            case E::CellularConnecting:
                st = NetLinkState::Connecting;
                break;
            case E::WifiConnected:
            case E::CellularConnected:
                st = NetLinkState::Connected;
                break;
            case E::WifiNoCredentials:
            case E::WifiConfigPortal:
                st = NetLinkState::NoCred;
                break;
            default:  // WifiConnectFailed / CellularDisconnected / CellularError*
                st = NetLinkState::Down;
                break;
        }
        s_net_link = static_cast<int>(st);
        s_net_dirty = true;
    });
    s_net_last_render = -1;
    s_bat_last_render = -1;
    RefreshWifiWidgets(true);
    RefreshBatteryWidgets();  // 建屏立即落一次电量，不等首个 tick
    s_net_timer = lv_timer_create(NetTick, 1000, nullptr);

    // P2：主题切换重涂钩子。静态配色全在 pi_theme 共享样式里自动翻转；这里
    // 补 label recolor 内嵌 hex 两处（时钟冒号、ASR 尾部高亮）+ 全部 MdView
    // （颜色烘焙在控件与 recolor 标记里，整块按新快照重建）。
    s_theme_listener_id = pi_theme::AddListener([]() {
        UpdateIdleClock(nullptr);  // 重生成 "#RRGGBB :#" 标记
        s_asr_rendered.clear();    // 下个 AsrTick 用新 accent 重涂高亮
        const lvmd::MdTheme md = PiMdTheme();
        for (lvmd::MdView* v : s_md_views) v->Retheme(md);
    });

    // P1：息屏链路。状态机在 pi_sleep（tick 与钩子都在 LVGL 线程）；拦截层
    // 挂在 screen 最后，OFF 进入时再 move_foreground 保证压过一切浮层。
    pi_sleep::Hooks sl_hooks;
    sl_hooks.is_gated = []() {
        // 播放队列非空（!IsPlaybackIdle）有两种成因：媒体（音乐/电台）在播，或 TTS 在
        // 播报——二者共用同一条 FeedPlayback 队列，仅凭队列忙无法区分。只应对后者门控
        // （TTS 短播报挡熄屏合理；音乐/电台长播不应该常亮整晚）。用 MediaController
        // 状态快照（media::MediaController::Instance().state()，只读，不触发任何
        // Suspend/Resume）区分：媒体正在 Playing ⇒ 声音来自媒体，不门控；媒体非
        // Playing（Stopped/Paused/Loading/Error）⇒ 声音只能来自 TTS，门控。
        // 安全性：pi_media_focus.cc 保证 TTS 出声前必先 SuspendForSpeech() 把 media
        // 拨到 Paused，因此"队列忙且 media 正在 Playing"与"队列忙且是 TTS"两种情形
        // 互斥，不会漏判 TTS 也不会误判媒体。
        const bool queue_busy = !mhal::audio_pipeline::IsPlaybackIdle();
        const bool media_playing = media::MediaController::Instance().state() == media::MediaState::Playing;
        return s_agent_busy ||                          // 生成中
               (queue_busy && !media_playing) ||         // TTS 播报中（队列忙但不是媒体在放）
               pi_quick_panel::IsOpen() || pi_settings::IsOpen() || pi_media::IsOpen() ||
               s_sheet_open || s_confirm_sheet_open ||  // 浮层
               pi_card::HasOpenOverlay() ||                                          // 卡片浮层
               s_state == ViewState::Listen;                                         // 聆听中
    };
    sl_hooks.on_dim = [](bool dim) { ApplySleepDimVisual(dim); };
    sl_hooks.on_off = [](bool off) {
        ApplySleepOffVisual(off);
        // 屏全黑：停 DataHub 活性刷新与 stock 行情拉取（历史采样照旧）；亮屏立即补种/补拉。
        pi_card::SetScreenOff(off);
        // 真正休眠了才回待机：屏已全黑，此时切回 Idle 不打断任何阅读，醒来是
        // 干净的时钟主界面（会话仍保留，右滑/继续对话可回到 Chat）。亮屏期间
        // 绝不强制回主界面——这正是"啥也没干却跳回主界面"的根治。
        if (off && s_state != ViewState::Idle)
            Go(ViewState::Idle);
    };
    pi_sleep::Start(scr, sl_hooks);

    s_session_turns = 0;
    s_session_start_ms = lv_tick_get();

    Go(ViewState::Idle);

    // pi_card 声明式 UI：注册 DataHub 内置路径 + 消息流接入钩子。DataHub 须在
    // agent 首次跑工具（校验器查 bind 路径）之前就绪；这里在 Create 早于任何
    // prompt，满足。
    pi_card::Init();
    pi_card::DataHub::Instance().StartLiveRefresh();
    // UI 侧可控路径注册进 DataHub（它不反向依赖 pi_screen，故由此处注入 getter/setter）：
    // TTS 播报开关 + 息屏时长档位——都无重启、纯 NVS/内存开关，可安全交给 LLM。
    {
        auto& hub = pi_card::DataHub::Instance();
        // worker 可读：只读一个全局 bool。
        hub.Register("speech.tts", pi_card::HubType::Bool,
                     []() -> pi_card::HubValue { return s_tts_on; },
                     [](const pi_card::HubValue& v) { SetTtsOn(std::get<bool>(v)); },
                     pi_card::WorkerRead::Safe);
        // worker 可读：NVS 读在 IDF 内部有锁；会阻塞几 ms，但阻塞在 agent worker 上无妨
        //（真正要避免的是阻塞 LVGL 线程）。
        hub.Register("display.sleep_s", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         Settings ui("ui", false);
                         return ui.GetInt("sleep_s", 0);
                     },
                     [](const pi_card::HubValue& v) {
                         Settings ui("ui", true);
                         ui.SetInt("sleep_s", std::get<int>(v));
                         pi_sleep::ReloadConfig();  // 立即重读，别等 ~5s tick
                     },
                     pi_card::WorkerRead::Safe, 0, 3600);

        // ---- P4-a 数据面扩容：只读遥测路径 ----
        // 全部只读（setter=nullptr），getter 一律走非阻塞快照 / 缓存读，绝不在
        // getter 里做阻塞 I2C/UART——因为 1Hz PublishLive 在 LVGL 线程也会调它。
        // WorkerRead::Safe：agent 渲染卡片时同步读快照，无 I2C 副作用。

        // 电池扩展遥测（BQ27220 standard commands，sysmon 1Hz 采样发布快照）。
        // 电压/电流带历史 → 直接喂 chart 做功耗曲线。
        hub.Register("battery.voltage_mv", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);  // 未采样时 e 为默认值(占位)
                         return static_cast<int>(e.voltage_mv);
                     },
                     nullptr, pi_card::WorkerRead::Safe, 3000, 4300, /*keep_history=*/true);
        hub.Register("battery.current_ma", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);
                         return static_cast<int>(e.current_ma);  // + 充 / - 放，无量程保留符号
                     },
                     nullptr, pi_card::WorkerRead::Safe, 0, -1, /*keep_history=*/true);
        hub.Register("battery.temp_c10", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);
                         return static_cast<int>(e.temp_c10);  // 0.1 ℃
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("battery.tte_min", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);
                         return static_cast<int>(e.tte_min);  // 剩余续航分钟，-1=未知
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("battery.soh_pct", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);
                         return static_cast<int>(e.soh_pct);
                     },
                     nullptr, pi_card::WorkerRead::Safe, 0, 100);
        hub.Register("battery.fcc_mah", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);
                         return static_cast<int>(e.fcc_mah);
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("battery.cycles", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::power::BatteryExt e;
                         mhal::power::GetBatteryExt(e);
                         return static_cast<int>(e.cycles);
                     },
                     nullptr, pi_card::WorkerRead::Safe);

        // 网络：IP / 运营商 / 4G 注册态（CEREG TAC/CI/AcT，JSON 原样透出供 LLM 解析）。
        // 三者门面内部均为缓存读+异步刷新，非阻塞。
        hub.Register("net.ip", pi_card::HubType::String,
                     []() -> pi_card::HubValue { return mhal::network::GetIpAddress(); },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("net.operator", pi_card::HubType::String,
                     []() -> pi_card::HubValue { return mhal::network::GetOperator(); },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("net.cell", pi_card::HubType::String,
                     []() -> pi_card::HubValue { return mhal::network::GetRegistrationStateJson(); },
                     nullptr, pi_card::WorkerRead::Safe);

        // 存储：SD 是否挂载 + 剩余空间（MB）。free_mb 走 statvfs，未挂载返回 0。
        hub.Register("storage.sd", pi_card::HubType::Bool,
                     []() -> pi_card::HubValue {
                         return static_cast<bool>(mhal::storage::IsSdMounted());
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("storage.free_mb", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         uint64_t total = 0, freeb = 0;
                         if (!mhal::storage::GetSdFreeBytes(total, freeb)) return static_cast<int>(0);
                         return static_cast<int>(freeb / (1024ULL * 1024ULL));
                     },
                     nullptr, pi_card::WorkerRead::Safe);

        // 系统：CPU 平均占用 % + 内部 RAM 剩余 KB（sysmon 1Hz 快照）。均带历史 → 性能面板 chart。
        hub.Register("sys.cpu", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         int c0 = 0, c1 = 0, avg = 0;
                         mhal::sysmon::GetCpuUsage(c0, c1, avg);
                         return static_cast<int>(avg);
                     },
                     nullptr, pi_card::WorkerRead::Safe, 0, 100, /*keep_history=*/true);
        hub.Register("sys.heap_kb", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         unsigned free_kb = 0, min_kb = 0;
                         mhal::sysmon::GetHeapKb(free_kb, min_kb);
                         return static_cast<int>(free_kb);
                     },
                     nullptr, pi_card::WorkerRead::Safe, 0, -1, /*keep_history=*/true);

        // 蓝牙：连接态 + 当前模式字符串——让 LLM 不再是盲盒式盲连。
        hub.Register("bt.connected", pi_card::HubType::Bool,
                     []() -> pi_card::HubValue {
                         return static_cast<bool>(mhal::bt::GetConnState() ==
                                                  mhal::bt::ConnState::Connected);
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("bt.mode", pi_card::HubType::String,
                     []() -> pi_card::HubValue {
                         switch (mhal::bt::GetMode()) {
                             case mhal::bt::Mode::Rx: return std::string("rx");
                             case mhal::bt::Mode::Tx: return std::string("tx");
                             case mhal::bt::Mode::MusicRx: return std::string("music");
                             default: return std::string("none");
                         }
                     },
                     nullptr, pi_card::WorkerRead::Safe);

        // ---- P4-b 事件与触觉：传感器只读路径 ----
        // 姿态：加速度计算出的俯仰/横滚（整数度，imu 5Hz 采样快照，非阻塞读）。
        hub.Register("imu.pitch", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         int x = 0, y = 0, z = 0, pitch = 0, roll = 0;
                         mhal::imu::GetSnapshot(x, y, z, pitch, roll);
                         return static_cast<int>(pitch);
                     },
                     nullptr, pi_card::WorkerRead::Safe, -90, 90);
        hub.Register("imu.roll", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         int x = 0, y = 0, z = 0, pitch = 0, roll = 0;
                         mhal::imu::GetSnapshot(x, y, z, pitch, roll);
                         return static_cast<int>(roll);
                     },
                     nullptr, pi_card::WorkerRead::Safe, -180, 180);
        // 电源在场检测：USB 插入 / 无线充电（TCA9555 P10/P11 输入脚，极性待真机确认）。
        // 主动性卡片的理想触发源（插上充电器弹充电面板）；本轮先透出只读状态。
        hub.Register("power.usb_in", pi_card::HubType::Bool,
                     []() -> pi_card::HubValue {
                         return static_cast<bool>(mhal::power::IsUsbInserted());
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("power.wireless_charging", pi_card::HubType::Bool,
                     []() -> pi_card::HubValue {
                         return static_cast<bool>(mhal::power::IsWirelessCharging());
                     },
                     nullptr, pi_card::WorkerRead::Safe);

        // ---- P4-c GPS：只读定位路径（门控默认关，未启用/未定位一律优雅回落）----
        // GPS 默认沉睡（见 gps.h：UART0 占用/贴料未定，需真机确认），故这些路径在启用前
        // 恒为 no-fix：fix=false、sats/alt/speed=0、lat/lon="--"。启用后由 gps.enable invoke。
        hub.Register("gps.fix", pi_card::HubType::Bool,
                     []() -> pi_card::HubValue {
                         mhal::gps::Fix f;
                         return static_cast<bool>(mhal::gps::GetFix(f) && f.valid);
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("gps.sats", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::gps::Fix f;
                         mhal::gps::GetFix(f);
                         return static_cast<int>(f.sats);
                     },
                     nullptr, pi_card::WorkerRead::Safe, 0, 40);
        hub.Register("gps.alt_m", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::gps::Fix f;
                         mhal::gps::GetFix(f);
                         return static_cast<int>(f.alt_m);
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("gps.speed_kmh", pi_card::HubType::Int,
                     []() -> pi_card::HubValue {
                         mhal::gps::Fix f;
                         mhal::gps::GetFix(f);
                         return static_cast<int>(f.speed_kmh);
                     },
                     nullptr, pi_card::WorkerRead::Safe, 0, 300);
        hub.Register("gps.lat", pi_card::HubType::String,
                     []() -> pi_card::HubValue {
                         mhal::gps::Fix f;
                         if (!mhal::gps::GetFix(f) || !f.valid) return std::string("--");
                         return FormatDegE5(f.lat);
                     },
                     nullptr, pi_card::WorkerRead::Safe);
        hub.Register("gps.lon", pi_card::HubType::String,
                     []() -> pi_card::HubValue {
                         mhal::gps::Fix f;
                         if (!mhal::gps::GetFix(f) || !f.valid) return std::string("--");
                         return FormatDegE5(f.lon);
                     },
                     nullptr, pi_card::WorkerRead::Safe);
    }
    pi_card::FeedHooks card_hooks;
    card_hooks.begin_row = CardBeginRow;
    card_hooks.end_row = CardEndRow;
    card_hooks.pin_host = []() -> lv_obj_t* { return s_pin_host; };
    card_hooks.on_pin_changed = [](bool has_pin) { ApplyPinLayout(has_pin); };
    pi_card::SetFeedHooks(card_hooks);

    // Phase3：invoke 命令注册表——net.reconnect/bt.reconnect/net.switch_type 在
    // CommandRegistry::RegisterBuiltins（纯 mhal/Settings，pi_card::Init 已调过）；
    // session.new 需要 DoNewSession()（pi_screen 独有），故在此注册。固件确认 sheet
    // 注入通道复用通用 confirm sheet（invoke-confirm 与 pin ✕ 手势共用同一张 UI）。
    pi_card::CommandRegistry::Instance().Register(
        "session.new", "new chat, clears history", pi_card::CmdLevel::Confirm,
        []() { DoNewSession(); }, "开始新对话？", "将清空当前对话历史", "新建");
    pi_card::CommandRegistry::Instance().SetConfirmHook(ShowConfirmSheet);

    // rehydrate 常驻组件：Go(Idle) 之后（视图/pin host 都已就绪）、drain timer 建立前——
    // 坏 JSON/版本不符/Validate 失败一律静默丢弃（RehydratePin 内部处理），绝不卡开机。
    pi_card::RehydratePin();

    s_drain_timer = lv_timer_create(DrainQueueTick, 80, nullptr);
    s_cursor_blink_timer = lv_timer_create(CursorBlinkTick, 500, nullptr);
    s_pickup_timer = lv_timer_create(PickupWatchTick, 150, nullptr);  // P4-b 拿起唤醒轮询

    // 单 App 固件：无 home 菜单可返回；视图切换全部经边缘导航路由 OnEdgeNav，
    // 只切视图不卸载 screen。
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void PiScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: pi_screen");
        pi_agent_task_start();
        UpdateModelLabels();
        // "按住说话"：按下/按住达阈值/松开三段，见 OnKeyPressAsync 上方注释。
        IOExpander::getInstance().onPress(IOExpander::Pin::PWR_KEY, []() {
            lv_async_call(OnKeyPressAsync, nullptr);
        });
        IOExpander::getInstance().onLongPress(IOExpander::Pin::PWR_KEY, kKeyHoldToTalkMs, []() {
            lv_async_call(OnKeyHoldAsync, nullptr);
        });
        IOExpander::getInstance().onRelease(IOExpander::Pin::PWR_KEY, []() {
            lv_async_call(OnKeyReleaseAsync, nullptr);
        });
    } else {
        ESP_LOGI(TAG, "unload: pi_screen");
        IOExpander::getInstance().offPress(IOExpander::Pin::PWR_KEY);
        IOExpander::getInstance().offLongPress(IOExpander::Pin::PWR_KEY);
        IOExpander::getInstance().offRelease(IOExpander::Pin::PWR_KEY);
    }
}
