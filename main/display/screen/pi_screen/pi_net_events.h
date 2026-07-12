#pragma once

#include <functional>
#include <string>

#include "metalio_hal/network.h"

// ---------------------------------------------------------------------------
// 网络事件分发层（P1）。mhal::network::OnEvent 是覆盖式单回调：后注册者
// 独占，先注册者被顶掉——而 pi_screen（状态栏真状态）与 pi_settings（网络
// 页/配网卡片）都需要订阅。本模块进程内只向 mhal::network::OnEvent 注册
// 一次转发器，向下多播给任意多个监听者。
//
// 线程模型与 mhal::network::OnEvent 完全一致：监听者回调在网络栈自身任务
// 线程触发，想动 LVGL 请自行封送（写快照 + LVGL 定时器轮询，见 pi_settings
// 的既有惯例）。AddListener/RemoveListener 本身线程安全，且允许在监听者
// 回调内部调用（转发时持锁只拷贝列表，回调在锁外执行）。
// ---------------------------------------------------------------------------
namespace pi_net_events {

using Listener = std::function<void(mhal::network::Event event, const std::string& data)>;

// 幂等；首次调用时向 mhal::network::OnEvent 注册转发器。必须先于
// mhal::network::StartAsync() 调用才能收到起网过程事件——pi_screen 的
// Create() 满足（main.cc 先建屏后起网）。
void Init();

// 注册监听者，返回 id（>0）供 RemoveListener 用。内部自动 Init()。
int AddListener(Listener cb);

// 注销监听者（id 无效时是 no-op）。
void RemoveListener(int id);

}  // namespace pi_net_events
