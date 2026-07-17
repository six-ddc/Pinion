#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// pi_media —— 媒体播放器的呈现层（Stage C）。
//
// 两件产物，同住本模块（pi_screen.cc 只做最小接线，媒体 UI 的全部细节归这里）：
//   1) 全屏 Now-Playing 页（懒创建，parent = pi_screen 的 screen 对象，追加为
//      最后一个子对象故 z 序最高）：生成式母题 art、传输排、进度、列表抽屉。
//      视觉语言延续 pi_settings（Bg 底 + Card + 1px Line + 唯一琥珀强调 + mono
//      小字 caption + 大圆角）。
//   2) 常驻 mini 播放条（挂 pi_screen 的 screen 对象，浮在底部提示/输入区上方）：
//      state != stopped 时浮现，tap 条体打开全屏页。
//
// 数据源：直接读 media::MediaController 快照（任意线程安全）+ 一条 1Hz lv_timer
// 刷新，不经 DataHub。控制走 MediaController 方法（Toggle/Next/Prev/PlayIndex）。
//
// 平台无关：仅依赖 lvgl + pi_theme + media_player 组件（sim 侧均在两端构建）。
// 主题即时切换：Tok 令牌控件走共享样式自动翻转，canvas 图元 / arc 一次性取色
// 经 pi_theme::AddListener 注册的回调重绘。
// ---------------------------------------------------------------------------
namespace pi_media {

// 构建常驻 mini 条（隐藏态）并记住 parent 供 Open() 使用。pi_screen Create 调一次。
void CreateMiniBar(lv_obj_t* parent);

// pi_screen 的 Go() 通知：当前视图是否允许显示 mini 条（Idle / Chat = true，
// Listen = false）。mini 条实际可见 = 允许 && media.state != stopped。
void SetMiniBarContext(bool allowed);

// 打开全屏 Now-Playing 页（LVGL 线程；重复调用 no-op）。media.open 命令与 mini
// 条 tap 都走这里。用 CreateMiniBar 记住的 parent。
void Open();

// 整页关闭并删除（未打开时 no-op）。不影响播放。
void Close();

bool IsOpen();

// 边缘导航左缘右滑 / 返回箭头：抽屉打开时先收抽屉，否则整页关闭。
void Back();

// 屏卸载清理（定时器/静态指针；widget 树随 screen 删除）。
void OnScreenUnloaded();

}  // namespace pi_media
