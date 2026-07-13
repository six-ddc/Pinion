#pragma once

// ---------------------------------------------------------------------------
// pi_card::Host —— 声明式 UI 卡片的宿主 / 注册表 / 生命周期
//
// 一张 LLM 下发的 UI = 一个 UiCard。宿主负责：
//   * 在正确父容器渲染：chat = 塞进 pi_screen 的消息流（经 FeedHooks 回调拿到
//     一行容器）；overlay = 挂 lv_layer_top() 的居中浮层，跨视图存活。
//   * 维护 id -> UiCard 注册表，供 update / close 定位。
//   * 唯一清理通道：每张卡的 root 挂 LV_EVENT_DELETE。无论因何被删（ClearFeed /
//     新会话 / 屏卸载 / close 动作 / ttl），都走 OnRootDeleted → Release 掉所有
//     DataHub 路径 + 出注册表。
//
// 与 pi-c 的接线（见 pi_card_tools.h）：LLM 调 ui.render/update/close 工具，工具
// execute 在 agent worker 线程**同步校验**（不碰 LVGL，错误同步回给 LLM 重试），
// 通过再把 spec 入 pi_ui_queue()；由 pi_screen 的 DrainQueueTick（LVGL 线程）按
// 流式顺序调下面的 On*Event 真正建控件——与既有 tool card 同一条渲染通路。
// ---------------------------------------------------------------------------

#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
#include "lvgl.h"

namespace pi_card {

enum class Display { Chat, Overlay };

struct UiCard {
    std::string id;
    lv_obj_t* root = nullptr;                 // 挂 DELETE 清理的对象（chat=行 / overlay=scrim）
    Display display = Display::Chat;
    std::map<std::string, lv_obj_t*> nodes;   // node id -> widget，供 update
    std::vector<std::string> hub_paths;       // 已 Acquire 的 DataHub 路径，供 Release
    lv_timer_t* ttl_timer = nullptr;          // overlay 自动关闭（一次性）
};

// pi_screen 注入的消息流接入点（都在 LVGL 线程调用）。
struct FeedHooks {
    lv_obj_t* (*begin_row)();  // 在 feed 末尾建一个全宽行返回（act_line 保持在最后）
    void (*end_row)();         // 渲染完做滚动到底等收尾
};

// ---- 启动期一次性（PiScreen::Create，LVGL 线程）----
void Init();  // 注册 DataHub 内置路径（幂等）
void SetFeedHooks(const FeedHooks& hooks);

// ---- drain-tick 入口（LVGL 线程，无需显示锁）----
void OnRenderEvent(const char* spec_json, const char* card_id, int display_mode, int ttl_ms);
void OnUpdateEvent(const char* card_id, const char* node_id, const char* props_json);
void OnCloseEvent(const char* card_id);

// 息屏门控：有 overlay 卡片开着时不进息屏（同 quick_panel 语义）。
bool HasOpenOverlay();

}  // namespace pi_card
