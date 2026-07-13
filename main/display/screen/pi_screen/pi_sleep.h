#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// 息屏链路（P1）——pi_screen 的 idle/sleep 管理器。状态机：
//
//   Awake（Idle 或 Chat 视图）--无操作达 NVS "ui"/"sleep_s" 秒（0=永不）--> Dim（亮度 20%，不持久化）
//   Dim --再 60s--> Off（亮度 0；时钟/动画深度降耗、真正休眠时回 Idle 由 hooks.on_off 负责）
//   Dim/Off --任意触摸 或 PWR_KEY--> Awake（恢复进入 Dim 前缓存的用户亮度）
//
// 闸门（hooks.is_gated：生成中 / TTS 播报中 / 快捷面板·设置栈·新对话 sheet
// 打开 / 聆听中；以及 mhal::network::IsConfigPortalActive()：WiFi 配网
// 热点跑着）期间不进 Dim/Off，且无操作计时持续重置。
//
// 唤醒语义：
//   - Off：置顶全屏拦截层吞掉第一次触摸（只唤醒，不落到任何控件）；PWR_KEY
//     短按/长按经 ConsumeKeyWake() 也只唤醒（不进聆听/不呼面板）。
//   - Dim：不拦截，触摸正常透传给控件，亮度在下个 tick（300ms 内）恢复；
//     PWR_KEY 经 NoteKeyActivity()/ConsumeKeyWake() 立即恢复亮度后照常透传。
//
// PWR_KEY 不经过 LVGL indev，lv_display_get_inactive_time() 感知不到——
// 按键处理入口必须调 ConsumeKeyWake()（内部兼做计时重置），这同时补上了
// P1 设置栈遗留的"按键不重置无操作计时"问题。
//
// 平台无关：仅依赖 lvgl + settings.h + mhal::backlight/mhal::network（sim 均有 shim）。
// ---------------------------------------------------------------------------
namespace pi_sleep {

struct Hooks {
    // 任一闸门生效返回 true。tick 在 LVGL 线程跑，可安全读 UI 状态。
    bool (*is_gated)() = nullptr;
    void (*on_dim)(bool dim) = nullptr;  // 进/出 Dim（防烧屏移位、呼吸点暂停）
    void (*on_off)(bool off) = nullptr;  // 进/出 Off（停时钟等深度降耗；进 Off 时回 Idle）
};

// pi_screen Create() 末尾调用一次：screen = pi_screen 的 screen 对象。
// 拦截层挂在其下（Off 进入时 move_foreground 保证 z 序最高）。
void Start(lv_obj_t* screen, const Hooks& hooks);

// PWR_KEY 短按/长按入口调用：重置无操作计时；Dim 下顺带唤醒（返回 false，
// 按键照常透传）；Off 下只唤醒并返回 true（调用方直接 return，吞掉本次按键）。
bool ConsumeKeyWake();

// 显示页息屏档位变更后调用，立即重读 NVS "ui"/"sleep_s"（否则由 tick 每
// ~5s 自动重读）。
void ReloadConfig();

bool IsAwake();

// 屏卸载清理（timer/静态指针；拦截层随 screen 树由 LVGL 删除）。
void OnScreenUnloaded();

}  // namespace pi_sleep
