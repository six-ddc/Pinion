#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// Settings stack -- pi_screen 屏上懒创建的全屏设置页栈（P1）。
//
// 不进 pi_screen 的 ViewState 枚举：整个栈是压在四状态 UI 之上的一个全屏
// 容器（Hub + 网络/蓝牙/声音/显示/对话/关于 六页），Open() 时才构建，
// Close()（栈清空 / 30s 无操作 / 长按 PWR_KEY）时整棵删除，露出进入前的
// 视图。导航：右滑 或 左上「<」逐级返回。
//
// 平台无关：仅依赖 lvgl + mhal 门面头 + settings.h + pi_sys_info.h（sim 侧
// 均有 shim）。与 pi_screen 的联动（TTS/ZEN 同源开关）通过 Hooks 注入，
// 本模块不反向 include pi_screen 内部。
// ---------------------------------------------------------------------------
namespace pi_settings {

struct Hooks {
    // TTS 播报开关（与 pi_screen 状态栏 TTS 钮同源；set 内部负责 NVS
    // 持久化与状态栏点刷新）。
    bool (*get_tts)() = nullptr;
    void (*set_tts)(bool on) = nullptr;
    // FLOW/ZEN 模式（与 pi_screen 状态栏 mode 钮同源）。
    bool (*get_zen)() = nullptr;
    void (*set_zen)(bool zen) = nullptr;
};

// 在 pi_screen Create() 时注入一次；必须先于 Open()。
void SetHooks(const Hooks& hooks);

// 打开设置栈（懒创建，parent = pi_screen 的 screen 对象，追加为最后一个
// 子对象故 z 序最高）。重复调用是 no-op。
void Open(lv_obj_t* parent);

// 快捷面板「后台」入口专用：跳过 Hub，直接把「设备后台」页推成栈里唯一
// 一页（同一个懒创建 root/tick 机制，Back() 时空栈即整体收起，与 Open() 后
// 逐级返回体感一致）。重复调用（栈已开）是 no-op。
void OpenFiles(lv_obj_t* parent);

// 整栈关闭并删除（Open 前 / 已关闭时是 no-op）。
void Close();

bool IsOpen();

// 逐级返回一层（栈空则整栈关闭）；确认 sheet 打开时先收 sheet。未打开时 no-op。
// 供 pi_screen 的边缘导航路由（左缘右滑）调用，与页头返回按钮同一语义。
void Back();

// 屏卸载时清理定时器/回调注册与静态指针（widget 树由 LVGL 随 screen 删除）。
void OnScreenUnloaded();

}  // namespace pi_settings
