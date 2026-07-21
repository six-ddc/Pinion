#pragma once

// ---------------------------------------------------------------------------
// pi_card::Render —— JSON 节点树 → LVGL 控件树（遵循 pi_theme 双主题 + 自适应）
//
// type 白名单：column / row / grid / label / button / slider / arc / switch /
// bar / icon / divider / spacer / qrcode / choice / list / chart / stock_chart。
// 配色一律走 pi_theme 令牌（tone/role），字体 puhui(中文)+pi_mono(数值)，图标经
// pi_card_icons 形状拼合。
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
// parent_flow: 父容器主轴（0=column/root, 1=row, 2=grid），用于自适应尺寸默认值。
// in_list_row: 本节点是否位于 list 的 item 模板重渲实例内——为真时不注册 id 进 card->nodes
// （N 行实例 id 不唯一）。
lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow = 0,
                     bool in_list_row = false);

// update：把 props（cJSON 对象）套到已有控件上。未知字段忽略。
bool ApplyProps(lv_obj_t* obj, const cJSON* props, std::string& err);

// 卡片入场动效：opa 0→255（220ms ease_out）+（非 adopted 时）translate_y 12→0（同 220ms，
// LV_STYLE_TRANSLATE_Y 风格属性，不直接改 y——卡片在 flex 布局里，直改 y 会被布局吃掉）。调用
// 方须在卡片最终布局定型之后调用（chat: s_feed.end_row() 之后；overlay: ReflowOverlay +
// AddOverlayCloseButton 之后），绝不能挂在 data 增量刷新路径（RefreshDataConsumers/
// OnUpdateEvent）上，否则每次数值更新都会重播入场。adopted=true（预留给未来"接手复用旧节点"
// 路径，如改造4 list 行级复用；pi_card 目前永远整卡重渲，调用点一律传 false）时只做 120ms
// opa 淡入、不带位移，避免复用内容还跟着跳一截。tree 是 RenderNode 建出来的 root 子树，不是
// wrapper/行容器本身。息屏时（IsScreenOff，pi_card_host.h）直接跳过，不起任何动画。
void PlayCardEntrance(lv_obj_t* tree, bool adopted);

// ---------------------------------------------------------------------------
// 流式生长卡片（改造1）专用的"预览渲染"——只画外观，零 bind/零事件/零 id 注册进
// card->nodes/零 DataConsumer 登记，因为流式阶段既没有 UiCard 也没有校验过的合法 bind 路径。
// 与 RenderNode 共用同一套 type 分派/样式助手，但 list/chart/stock_chart 三种数据驱动或自管
// 生命周期的类型直接跳过不建。限额沿用 64 节点/8 层，用独立计数（不占用正式渲染的
// RenderLimits/node_count），超限只是静默停止生长，不报错——预览允许"半棵树"。

// 一次性整棵渲染 node（及其全部子树，若是 column/row）：用于 (a) 流式会话第一次出现可渲染
// 内容时的初始建树，(b) 位置游标推进时把"已确定不再变"的兄弟节点渲染成最终形。会给自己建出
// 的每个 column/row 打 LV_OBJ_FLAG_USER_2（内部惯用记号，标记"这是一个预览容器，可以被
// PreviewSyncContainer 继续增量同步"）+ user_data 记一个 committed 游标（= 已建子节点数-1，
// 即认定最后一个子节点仍是"生长边"，留给下一次调用去继续对齐）。
lv_obj_t* RenderPreviewNode(lv_obj_t* parent, const cJSON* node, int depth, int& node_count);

// 位置游标增量对齐：lv_container 是先前调用建出的、打了 USER_2 标记的容器；json_container 是
// 它这次收到的最新 partial 节点（读它的 "children" 数组，N 个孩子）。[0, N-2]（如果还没被上
// 一轮标成 committed）逐个渲成最终形；第 N-1 个（生长边）走 SyncPreviewNode 原地更新/递归/
// 删旧重建。已定稿区间的 lv_obj 指针跨调用不变——只有生长边那一个位置会被反复替换或原地更新。
void PreviewSyncContainer(lv_obj_t* lv_container, const cJSON* json_container, int depth,
                          int& node_count);

// 单个位置的"生长边"同步：existing 是当前挂在这个位置的 lv 对象（可为 nullptr，表示这个位置
// 之前还没能渲出东西）；node_spec 是这个位置最新的 partial 节点。返回这个位置最终对应的 lv
// 对象（可能与 existing 相同——原地更新；也可能是新建的——删旧重建）。column/row 类型且
// existing 已带 USER_2 标记时递归调 PreviewSyncContainer 继续往深处生长（保住已定稿的孙子）；
// 类型不符/首次出现则整棵重建。label 类型原地 lv_label_set_text（不重建，长文本不闪烁）；
// 其它叶子类型无状态可原地更新，统一删旧重渲（成本低，也是团队定稿点名的做法）。这是
// PreviewSyncContainer 的生长边分支与流式会话顶层 tree 同步共用的唯一入口。
lv_obj_t* SyncPreviewNode(lv_obj_t* parent, lv_obj_t* existing, const cJSON* node_spec, int depth,
                          int& node_count);

// 预算重算：删除节点时不精确回补 node_count（子树带走几个节点不值得追踪），调用方
// （PreviewOnArgs）在每帧处理完 SyncPreviewNode 之后应该用这个函数对整棵预览树重新计数，
// 作为下一帧的准确起点。不计入占位对象（RenderPreviewNode 内部的 USER_3 标记）。
int CountPreviewNodes(lv_obj_t* tree);

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
