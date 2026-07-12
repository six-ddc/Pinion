#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// Quick Panel -- pi_screen 屏内的顶部下拉快捷面板（P0）。
//
// 半透明 scrim 盖住底下内容，面板从顶部滑出（圆角 24 底边、卡片色 0x16130E、
// 1px 边线）。内容：状态行（网络类型 + 电量）、VOL / BRT 滑条（拖动即时生效，
// 松手持久化）、一行四个动作按钮（新对话 / 设置 / 主题 / 关机-长按 2s）。
//
// 呼出/收起策略（长按 PWR_KEY、状态栏下拉、scrim 点按、面板内上滑）由
// pi_screen 侧的手势/按键代码调用 Open()/Close()/Toggle() 完成；本模块只
// 负责面板自身的构建与交互。平台无关：仅依赖 lvgl + mhal 门面头 +
// settings.h（sim 侧均有 shim）。
// ---------------------------------------------------------------------------
namespace pi_quick_panel {

struct Hooks {
    // 「新对话」被点按（面板已自行收起后回调）。pi_screen 侧接确认 sheet。
    void (*on_new_session)() = nullptr;
    // 「设置」被点按（面板已自行收起后回调）。pi_screen 侧推入设置栈（P1）。
    void (*on_settings)() = nullptr;
};

// 构建面板（默认隐藏）。parent 是 pi_screen 的 screen 对象；只调一次，
// 必须在所有会被面板遮盖的兄弟对象之后创建（z 序靠前）。
void Create(lv_obj_t* parent, const Hooks& hooks);

void Open();  // 聆听取消等前置动作由调用方负责
void Close();
void Toggle();
bool IsOpen();

// 屏卸载时清理定时器与静态指针（widget 树由 LVGL 随 screen 删除）。
void OnScreenUnloaded();

}  // namespace pi_quick_panel
