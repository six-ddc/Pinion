#pragma once

// ---------------------------------------------------------------------------
// pi_card::Action —— 声明式动作模型
//
// widget 的 on_click / on_change / on_release 携带一个「动作数组」。渲染时解析成
// std::vector<Action>（脱离 cJSON 生命周期），连同所属 UiCard 打包进堆上的
// EventBinding 挂到控件事件；控件删除时随之释放。分发在 LVGL 线程（事件回调）：
//
//   close   —— 异步删卡片 root（从子控件回调里删祖先，须 async）
//   set     —— 取显式 value 或控件当前值 → DataHub::Write 写硬件
//   report  —— 文本 {v}/{value} 替换控件当前值 → 节流注入回 LLM（pi_agent_steer）
//
// 设计约定（比 Claw4 更克制）：绑了硬件路径的滑块靠双向 bind + 回写即可，不必
// report；report 专供「离散选择」——按钮/开关/列表项，让 LLM 知道用户选了什么。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "cJSON.h"
#include "lvgl.h"

namespace pi_card {

struct UiCard;  // pi_card_host.h

enum class ActionKind { Close, Set, Report, Unknown };

struct Action {
    ActionKind kind = ActionKind::Unknown;
    bool has_value = false;  // set：是否显式给了 value
    int value = 0;
    std::string path;  // set：目标 DataHub 路径
    std::string text;  // report：模板，{v}/{value} 替换为控件当前值
};

std::vector<Action> ParseActions(const cJSON* arr);

// 校验器用（agent worker 线程，不碰 LVGL）：动作数组是否合法。unknown 动作报错。
bool ValidateActions(const cJSON* arr, std::string& err);

// 给控件挂事件 + 对应 DELETE 清理；actions 为空则不挂。
void AttachEvent(lv_obj_t* widget, lv_event_code_t code, UiCard* card, const cJSON* actions_json);

// 读 slider/bar/arc/switch 的当前值（switch 返回 0/1）。
int WidgetValue(lv_obj_t* widget);

}  // namespace pi_card
