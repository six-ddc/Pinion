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
//   2) 紧凑媒体行（多实例，嵌入 pi_screen 的底部区域：Chat dock 左列 + Idle 提示带
//      上方的屏幕级条）：state != stopped 时浮现，tap 行体打开全屏页、tap 图元切播放。
//
// 数据源：直接读 media::MediaController 快照（任意线程安全）+ 一条 1Hz lv_timer
// 刷新，不经 DataHub。控制走 MediaController 方法（Toggle/Next/Prev/PlayIndex）。
//
// 平台无关：仅依赖 lvgl + pi_theme + media_player 组件（sim 侧均在两端构建）。
// 主题即时切换：Tok 令牌控件走共享样式自动翻转，canvas 图元 / arc 一次性取色
// 经 pi_theme::AddListener 注册的回调重绘。
// ---------------------------------------------------------------------------
namespace pi_media {

// 记住 screen 对象供 Open() 使用 + 起 1Hz 刷新。pi_screen Create 调一次。
void Init(lv_obj_t* screen);

// 构建一个紧凑媒体行实例（默认 HIDDEN；state != stopped 时浮现），返回根对象由调用方
// 定位/定宽。gate_by_context=true 的实例额外受 SetMiniBarContext 门控（Idle 屏幕级条
// 用——它建在 ptt 层之上、不随视图容器显隐）；嵌在视图容器内的实例传 false。
// 可先于 Init 调用（内部懒起刷新定时器/主题监听）。
lv_obj_t* CreateInlineBar(lv_obj_t* parent, bool gate_by_context);

// pi_screen 的 Go() 通知：屏幕级媒体行（gate_by_context=true 实例）是否允许显示
//（仅 Idle = true）。实际可见 = 允许 && media.state != stopped。
void SetMiniBarContext(bool allowed);

// 打开全屏 Now-Playing 页（LVGL 线程；重复调用 no-op）。media.open 命令与媒体行
// tap 都走这里。用 Init 记住的 parent。
void Open();

// 整页关闭并删除（未打开时 no-op）。不影响播放。
void Close();

bool IsOpen();

// 边缘导航左缘右滑 / 返回箭头：抽屉打开时先收抽屉，否则整页关闭。
void Back();

// 屏卸载清理（定时器/静态指针；widget 树随 screen 删除）。
void OnScreenUnloaded();

// ---- 断点续播（体验优化）------------------------------------------------
// ResumeLast 的结果：调用方（快捷面板「音乐」）据此决定关面板 or 吐 toast。
enum class ResumeResult {
    Opened,     // 已恢复并打开播放页（正在播/暂停直接开；否则从 NVS 记录重建）
    NoRecord,   // 无正在播放，也无持久化记录 —— 无可续
    FilesGone,  // 文件类记录里的曲目已全部被删
    NoNetwork,  // 电台类记录但当前无网络
};

// 是否有可继续的播放：MediaController 非 Stopped（正在播/暂停）或存在 NVS 记录。
// 快捷面板据此在点击时决定是否吐「没有可继续的播放」。
bool HasResumable();

// 继续上次播放。非 Stopped 直接 Open()；否则读 NVS "media"/"last" 重建播放列表
// 后 Open()。因 MediaController 公有 API 无 seek，文件类从曲目开头起播（保存的
// pos_s 仅记录、不用于定位，日志注明）。见 ResumeResult。
ResumeResult ResumeLast();

}  // namespace pi_media
