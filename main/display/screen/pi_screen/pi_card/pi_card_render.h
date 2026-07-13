#pragma once

// ---------------------------------------------------------------------------
// pi_card::Render —— JSON 节点树 → LVGL 控件树（遵循 pi_theme 双主题 + 自适应）
//
// type 白名单：column / row / label / button / slider / switch / bar / icon /
// divider / spacer。配色一律走 pi_theme 令牌（tone/role），字体 puhui(中文)+
// pi_mono(数值)，图标经 pi_card_icons 形状拼合。未知 type → 整卡失败（回滚已
// 建部分）+ 可读错误串；未知字段静默忽略（前向兼容）。限额：≤64 节点、≤8 层。
//
// Validate() 是「不建控件」的干跑校验（agent worker 线程调用，同步把错误回给
// LLM）；RenderNode() 真正建控件（LVGL 线程，drain tick）。二者共用同一套
// type/bind/action 约束，保证「校验过 → 一定能建」。
// ---------------------------------------------------------------------------

#include <string>

#include "cJSON.h"
#include "lvgl.h"

#include "pi_card_host.h"

namespace pi_card {

struct RenderLimits {
    int max_nodes = 64;
    int max_depth = 8;
};

// 干跑校验整棵树（含 bind 路径存在性、action 合法性、节点/深度限额）。
bool Validate(const cJSON* root_node, std::string& err);

// 递归渲染 node 到 parent 下。成功返回顶层控件；失败返回 nullptr 并写 err。
// parent_flow: 父容器主轴（0=column/root, 1=row），用于自适应尺寸默认值。
lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow = 0);

// update：把 props（cJSON 对象）套到已有控件上。未知字段忽略。
bool ApplyProps(lv_obj_t* obj, const cJSON* props, std::string& err);

}  // namespace pi_card
