#pragma once

// ---------------------------------------------------------------------------
// pi_card::Render —— JSON 节点树 → LVGL 控件树（遵循 pi_theme 双主题 + 自适应）
//
// type 白名单：column / row / label / button / slider / arc / switch / bar /
// icon / divider / spacer / qrcode / choice。配色一律走 pi_theme 令牌
// （tone/role），字体 puhui(中文)+pi_mono(数值)，图标经 pi_card_icons 形状拼合。
// 未知 type → 整卡失败（回滚已建部分）+ 可读错误串；未知字段静默忽略（前向
// 兼容）。限额：≤64 节点、≤8 层。
//
// Validate() 是「不建控件」的干跑校验（agent worker 线程调用，同步把错误回给
// LLM）；RenderNode() 真正建控件（LVGL 线程，drain tick）。二者共用同一套
// type/bind/action 约束，保证「校验过 → 一定能建」。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "cJSON.h"
#include "lvgl.h"

#include "pi_card_host.h"  // RenderLimits 定义在这里（UiCard::DataConsumer 需要按值持有）

namespace pi_card {

// 干跑校验整棵树（含 bind 路径存在性、action 合法性、节点/深度限额）。data：卡级 data
// 模型（object|nullptr），list 节点的 bind_data 用它算实际长度、预留节点数。
bool Validate(const cJSON* root_node, const cJSON* data, std::string& err);

// 非阻断设计建议（worker 线程、纯 cJSON、不碰 LVGL）：primary 按钮超一个、无文本 label、
// on_change 挂 report、死控件、choice 无回流出口、节点/深度逼近上限等。Validate 通过后跑，
// 结果搭在 render 返回值的 "hints" 数组里回给 LLM，不阻断渲染。
std::vector<std::string> Lint(const cJSON* root_node, const cJSON* data);

// choice 复合控件（flex-row 容器 + N 个内部按钮）取值 / 置值。USER_1 标记判定是否为
// choice 容器；非 choice 容器 ChoiceValue 返回 false。
bool ChoiceValue(lv_obj_t* obj, int& out);
void ChoiceSetValue(lv_obj_t* obj, int idx);

// 取 choice 当前选中段的按钮文本（{label} token / CollectState 的 idx(label) 用）。
// 非 choice 容器或取不到文本 → false。
bool ChoiceLabel(lv_obj_t* obj, std::string& out);

// 递归渲染 node 到 parent 下。成功返回顶层控件；失败返回 nullptr 并写 err。
// parent_flow: 父容器主轴（0=column/root, 1=row），用于自适应尺寸默认值。
// in_list_row: 本节点是否位于 list 的 item 模板重渲实例内——为真时不注册 id 进 card->nodes
// （N 行实例 id 不唯一）。
lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow = 0,
                     bool in_list_row = false);

// update：把 props（cJSON 对象）套到已有控件上。未知字段忽略。
bool ApplyProps(lv_obj_t* obj, const cJSON* props, std::string& err);

// ---- data 值格式化 / list 行模板替换（RenderNode 的 list/data-label 分支与
// pi_card_host.cc 的 RefreshDataConsumers 共用，故跨 TU 可见而非 render.cc 内部静态）----

// number→%g（去尾）；string→原样；null/其他→空串。
std::string Stringify(const cJSON* v);
// 模板里的 {value}/{v} → Stringify(v)。
std::string SubstDataValue(const std::string& tpl, const cJSON* v);
// list 的 eff_max：max 声明优先，否则=数组实际长度（>0）或缺省 8；夹在 [1,20]（硬顶 20）。
int EffMax(const cJSON* node, int arr_len);
// list 行模板递归替换：字符串值里的 {i}(0基)/{n}(1基)/{item.KEY} 替换成本行记录内容；
// 缺失字段替空串；只替字符串**值**，不碰 key。
void SubstRecord(cJSON* node, const cJSON* rec, int i);

// 稳定性护栏：mono 字体只有 ASCII，一旦文本含中文会渲成豆腐块，此时无论角色如何都回退到
// puhui（返回是否发生了回退）。list 的 empty 兜底文案（render.cc 与 pi_card_host.cc 的
// RefreshDataConsumers 各一处）都要走它，故导出为跨 TU 可见。
bool SafeFont(const lv_font_t*& font, const char* text);

}  // namespace pi_card
