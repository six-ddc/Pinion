#pragma once

// ---------------------------------------------------------------------------
// pi_card::Action —— 声明式动作模型
//
// widget 的 on_click / on_change / on_release 携带一个「动作数组」。渲染时解析成
// std::vector<Action>（脱离 cJSON 生命周期），连同所属 UiCard 打包进堆上的
// EventBinding 挂到控件事件；控件删除时随之释放。分发在 LVGL 线程（事件回调）：
//
//   close             —— 异步删卡片 root（从子控件回调里删祖先，须 async）
//   set               —— 取显式 value 或控件当前值 → DataHub::Write 写硬件
//   toggle/show/hide  —— 切同卡某节点的显隐（target = 该节点 id）。纯本地，零 LLM 往返
//   patch             —— 就地改同卡某节点的 props（text/value/checked/hidden/tone/color）。纯本地，
//                        零 LLM 往返；text 支持 {v}/{value} 替换成触发控件的当前值
//   report            —— 文本 {v}/{value} 替换控件当前值 + 同卡状态快照 → 节流注入回 LLM
//
// 设计约定 —— 判据是**这次点击设备自己能做完吗**：能 → close/set/toggle/show/hide，一帧
// 搞定；只有必须让模型生成新内容 / 替用户做新决策时才 report。一次 report 的代价是三重的：
// 一轮完整 LLM 往返（steering 队列非空正是 pi_loop 内层不退出的条件，入队即保证多一轮）、
// 聊天流里一个假的用户气泡（它以 user message 身份进 transcript）、以及 token。绑了硬件
// 路径的滑块靠双向 bind + 回写即可，更不必 report。
// ---------------------------------------------------------------------------

#include <set>
#include <string>
#include <vector>

#include "cJSON.h"
#include "lvgl.h"

namespace pi_card {

struct UiCard;  // pi_card_host.h

enum class ActionKind { Close, Set, Report, Toggle, Show, Hide, Patch, Invoke, Unknown };

struct Action {
    ActionKind kind = ActionKind::Unknown;
    bool has_value = false;  // set：是否显式给了 value
    int value = 0;
    std::string path;        // set：目标 DataHub 路径
    std::string text;        // report：模板，{v}/{value} 替换为控件当前值
    std::string target;      // toggle/show/hide/patch：同卡目标节点的 id
    std::string props_json;  // patch：props 对象序列化串（脱离 cJSON 生命周期，dispatch 时重解析）
    std::string cmd;          // invoke：CommandRegistry 里的命令名
};

std::vector<Action> ParseActions(const cJSON* arr);

// 校验器用（agent worker 线程，不碰 LVGL）：动作数组是否合法。unknown 动作报错。
// node_ids = 同一份 spec 里已声明的全部节点 id——此刻卡片尚未渲染、UiCard::nodes 还不存在，
// 但 target 指向的节点就写在同一棵 JSON 树上，故能在干跑期同步校验、错误同步回给 LLM 重试。
// in_list_row：本动作数组是否位于 list 的 item 行模板内——为真时拒绝 toggle/show/hide/patch
// （N 行实例 id 不唯一，per-row 作用域是过度工程；行内只许 report/set/close）。
bool ValidateActions(const cJSON* arr, const std::set<std::string>& node_ids, std::string& err,
                     bool in_list_row = false);

// 给控件挂事件 + 对应 DELETE 清理；actions 为空则不挂。
void AttachEvent(lv_obj_t* widget, lv_event_code_t code, UiCard* card, const cJSON* actions_json);

// 读 slider/bar/arc/switch 的当前值（switch 返回 0/1）。
int WidgetValue(lv_obj_t* widget);

}  // namespace pi_card
