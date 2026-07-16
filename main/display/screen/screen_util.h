#pragma once

#include "lvgl.h"

// Make `obj` and all of its descendants ignore touch input, so that
// PRESSED / RELEASED events fall through to the screen. This is required
// for screen-level swipe gestures to receive coordinates regardless of
// where on the page the user touches.
void screen_make_input_passive(lv_obj_t* obj);

// Strip default LVGL chrome (padding / margin / border / radius / scrollbar)
// from a generic container that we are using purely for layout.
void screen_strip_obj_chrome(lv_obj_t* obj);

// ---------------------------------------------------------------------------
// Edge-swipe navigation (进程唯一的 indev 级手势层)
//
// 手机式边缘导航：只有**起始点落在屏幕左/右边缘带**的横滑才算导航手势，屏幕
// 内部的横向拖拽（滑条、图表平移、横向滚动……）几何上自动归控件所有——这替代
// 了旧的 screen_swipe_back_ignore 逐控件打标机制（已删除）。
//
// 实现挂在 **indev 级**（lv_indev_add_event_cb）：LVGL 的 send_event() 会把
// PRESSED/RELEASED（转发白名单）与单指 GESTURE 先发给 indev 本身，完全不依赖
// 对象树的事件冒泡/GESTURE_BUBBLE——按压落在任意控件上都全局可见。前提不变量：
// **根 screen 必须保持 CLICKABLE**（LVGL base 对象默认即是），这样空白背景的
// 按压才有兜底命中对象（indev_obj_act 非空，indev 事件才会发）。
//
// 三段式状态机（indev 回调收不到 PRESSING，这是 LVGL 转发白名单决定的）：
//   PRESSED  记起点 + 判 arm（边缘带内且 y 不在状态栏区）；
//   GESTURE  快甩路径（LVGL 手势：≥3px/帧速度累计 50px），命中即派发并
//            wait_release（底下控件收 PRESS_LOST，不会误 CLICK）；
//   RELEASED 慢拖兜底（不满足手势速度阈值时按位移判定），按 arm 的边分正负号。
// 守卫（免打标的最后两道）：indev 正在滚动某对象 → 弃（服务兜底路径；GESTURE
// 与 scroll 在 LVGL 内部本就互斥）；按压对象是 slider/arc/roller → 弃（滑条可能
// 横跨边缘带）。
//
// 派发只报告"哪条边起手"；把它翻译成哪个视图切换/返回是 pi_screen 路由回调的
// 职责（settings 返回 / Chat↔Idle / 模态打开时忽略），机制与策略分层。
// ---------------------------------------------------------------------------
typedef enum {
    SCREEN_EDGE_NAV_FROM_LEFT,   // 左缘起手右滑（返回/回主页语义）
    SCREEN_EDGE_NAV_FROM_RIGHT,  // 右缘起手左滑（前进/回对话语义）
} screen_edge_nav_dir_t;

typedef void (*screen_edge_nav_cb_t)(screen_edge_nav_dir_t dir);

// 初始化：对当前存在的所有 LV_INDEV_TYPE_POINTER indev 挂回调（sim 有 mouse +
// vtouch 两个，真机一个）。幂等；须在 indev 创建之后调用（pi_screen Create 末尾）。
void screen_edge_nav_init(screen_edge_nav_cb_t cb);

// ---------------------------------------------------------------------------
// Screen lifecycle hooks
//
// A single callback receives both LOAD and UNLOAD notifications.  Use this
// to do per-screen logging, telemetry, or to start / stop background work
// that should run only while the screen is on stage.
//
//   void weather_lifecycle_cb(screen_lifecycle_event_t event) {
//       if (event == SCREEN_LIFECYCLE_LOAD) { ... }
//       else { ... }
//   }
//
// The hook is wired with `screen_attach_lifecycle(scr, cb)` -- typically
// called by the home / menu screen right after building each child screen,
// so the wiring lives in one central place rather than scattered through
// every screen implementation.
//
// LOAD fires when the screen becomes active (LV_EVENT_SCREEN_LOADED).
// UNLOAD fires when LVGL has finished switching away from the screen
// (LV_EVENT_SCREEN_UNLOADED).  The screen object remains valid throughout
// the UNLOAD callback; callers are expected to free their own state there.
// ---------------------------------------------------------------------------
typedef enum {
    SCREEN_LIFECYCLE_LOAD,    // matches LV_EVENT_SCREEN_LOADED
    SCREEN_LIFECYCLE_UNLOAD,  // matches LV_EVENT_SCREEN_UNLOADED
} screen_lifecycle_event_t;

typedef void (*screen_lifecycle_cb_t)(screen_lifecycle_event_t event);

void screen_attach_lifecycle(lv_obj_t* scr, screen_lifecycle_cb_t cb);
