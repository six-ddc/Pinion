#include "pi_sleep.h"

#include "esp_log.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/network.h"
#include "screen_util.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "PiSleep";

constexpr int32_t kW = 720;
constexpr int32_t kH = 720;

constexpr uint32_t kTickMs = 300;            // 唤醒响应粒度（Dim 下触摸恢复亮度的延迟上限）
constexpr uint32_t kDimToOffMs = 60 * 1000;  // Dim 再 60s 熄灭
constexpr uint8_t kDimBrightness = 20;

enum class State { Awake, Dim, Off };

pi_sleep::Hooks s_hooks;
lv_obj_t* s_shield = nullptr;  // Off 态置顶全屏拦截层（吞掉唤醒的第一次触摸）
lv_timer_t* s_timer = nullptr;
State s_state = State::Awake;
int32_t s_sleep_s = 0;           // NVS "ui"/"sleep_s"（0=永不）
uint32_t s_key_activity_ms = 0;  // 最近一次 PWR_KEY（lv_tick 时基；indev 感知不到按键）
uint32_t s_anchor_ms = 0;        // 闸门/状态切换的计时重置锚点
uint8_t s_user_brightness = 75;  // 进 Dim 前缓存的用户亮度（唤醒恢复用）

void LoadConfig() {
    Settings ui("ui", false);
    s_sleep_s = ui.GetInt("sleep_s", 0);
}

// 有效无操作时长 = 触摸（lv inactive）、PWR_KEY、闸门锚点三者取最近。
uint32_t IdleMs() {
    uint32_t now = lv_tick_get();
    uint32_t idle = lv_display_get_inactive_time(nullptr);
    uint32_t since_key = now - s_key_activity_ms;
    if (since_key < idle)
        idle = since_key;
    uint32_t since_anchor = now - s_anchor_ms;
    if (since_anchor < idle)
        idle = since_anchor;
    return idle;
}

void ResetIdleAnchor() { s_anchor_ms = lv_tick_get(); }

void EnterDim() {
    s_user_brightness = mhal::backlight::GetBrightness();
    if (s_user_brightness < 5)
        s_user_brightness = 5;  // 与 backlight::Restore 同款下限，唤醒不至于仍近黑
    mhal::backlight::SetBrightness(kDimBrightness, false);  // 不持久化
    s_state = State::Dim;
    ESP_LOGI(TAG, "dim (sleep_s=%d, cached user brightness %u)", static_cast<int>(s_sleep_s),
             static_cast<unsigned>(s_user_brightness));
    if (s_hooks.on_dim != nullptr)
        s_hooks.on_dim(true);
}

void EnterOff() {
    mhal::backlight::SetBrightness(0, false);
    s_state = State::Off;
    ESP_LOGI(TAG, "off");
    if (s_shield != nullptr) {
        lv_obj_move_foreground(s_shield);  // 压过 ptt 层/面板/sheet（设置栈闸门期不可能在开）
        lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_hooks.on_off != nullptr)
        s_hooks.on_off(true);
}

// keep_shield：来自拦截层按压的唤醒——层留到 RELEASED 再收，吞掉整次手势。
void WakeUp(bool keep_shield) {
    if (s_state == State::Awake)
        return;
    State was = s_state;
    s_state = State::Awake;
    ResetIdleAnchor();
    mhal::backlight::SetBrightness(s_user_brightness, false);
    ESP_LOGI(TAG, "wake (from %s, brightness -> %u)", was == State::Off ? "off" : "dim",
             static_cast<unsigned>(s_user_brightness));
    if (!keep_shield && s_shield != nullptr)
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_HIDDEN);
    if (was == State::Off && s_hooks.on_off != nullptr)
        s_hooks.on_off(false);
    if (s_hooks.on_dim != nullptr)
        s_hooks.on_dim(false);
}

void OnShieldPressed(lv_event_t*) { WakeUp(true); }

void OnShieldReleased(lv_event_t*) {
    if (s_shield != nullptr)
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_HIDDEN);
}

void Tick(lv_timer_t*) {
    // 配网热点跑着时不进 Dim/Off、不回待机——与快捷面板/设置栈/sheet 打开
    // 同级闸门，避免配网中途息屏或被自动回主页打断。
    bool gated = (s_hooks.is_gated != nullptr && s_hooks.is_gated()) ||
                 mhal::network::IsConfigPortalActive();
    if (gated) {
        ResetIdleAnchor();  // 闸门期间计时持续重置
        if (s_state != State::Awake)
            WakeUp(false);  // 保险：闸门中不应停留在 Dim/Off（如息屏中来了 TTS）
        return;
    }

    uint32_t idle = IdleMs();

    if (s_state != State::Awake) {
        // Dim/Off 下的唤醒检测：任何触摸都会把 lv inactive 清零（Off 的第一
        // 击落在拦截层上，同样计入且已被 OnShieldPressed 即时处理）。
        if (idle < kTickMs * 2) {
            WakeUp(false);
        } else if (s_state == State::Dim && s_sleep_s > 0 &&
                   idle >= static_cast<uint32_t>(s_sleep_s) * 1000 + kDimToOffMs) {
            EnterOff();
        }
        return;
    }

    // Dim/Off 由用户设置的 sleep_s 统一驱动，Idle 与 Chat 同等对待——不再有
    // "Chat 无操作 120s 强制回待机"那条独立计时（亮屏读长回复时会莫名跳回主
    // 界面，2026-07 按用户反馈移除）。真正休眠（进 Off）时才由 on_off 顺带回
    // Idle，亮屏期间绝不打断阅读。Listen/生成中/TTS/浮层等已被 is_gated 挡在
    // 上面，不会误进 Dim。sleep_s=0（永不）直接不进。
    if (s_sleep_s > 0 && idle >= static_cast<uint32_t>(s_sleep_s) * 1000) {
        EnterDim();
    }
}

}  // namespace

namespace pi_sleep {

void Start(lv_obj_t* screen, const Hooks& hooks) {
    s_hooks = hooks;
    s_state = State::Awake;
    s_key_activity_ms = lv_tick_get();
    ResetIdleAnchor();
    LoadConfig();

    s_shield = lv_obj_create(screen);
    screen_strip_obj_chrome(s_shield);
    lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_shield, kW, kH);
    lv_obj_set_pos(s_shield, 0, 0);
    lv_obj_set_style_bg_opa(s_shield, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_PRESS_LOCK);  // 手势全程锁在层上
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_shield, OnShieldPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_shield, OnShieldReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_shield, OnShieldReleased, LV_EVENT_PRESS_LOST, nullptr);

    s_timer = lv_timer_create(Tick, kTickMs, nullptr);
}

bool ConsumeKeyWake() {
    s_key_activity_ms = lv_tick_get();
    if (s_state == State::Off) {
        WakeUp(false);
        return true;  // 只唤醒，不透传（不进聆听/不呼面板）
    }
    if (s_state == State::Dim)
        WakeUp(false);  // 恢复亮度后照常透传
    return false;
}

// 配置更新的唯一入口（设置页改档走这里）；新增写者必须调用。
void ReloadConfig() { LoadConfig(); }

bool IsAwake() { return s_state == State::Awake; }

void OnScreenUnloaded() {
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_state != State::Awake)  // 卸载兜底：别把黑屏/暗屏状态带进下一张屏
        mhal::backlight::SetBrightness(s_user_brightness, false);
    s_state = State::Awake;
    s_shield = nullptr;  // widget 树由 LVGL 随 screen 删除
    s_hooks = Hooks{};
}

}  // namespace pi_sleep
