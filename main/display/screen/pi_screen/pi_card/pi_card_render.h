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
// v2：root_node 必须是 grid 块数组（见 docs/CARD_V2.md §6.1）；建议先调 Repair() 静默自愈
// 掉常见语法糖错误，再调本函数——Repair 修不了的错误由本函数给出可读 err。
bool Validate(const cJSON* root_node, const cJSON* data, std::string& err);

// v2 自动修复（docs/CARD_V2.md §6.2）：就地修改 envelope（顶层信封对象，含 "root" 键，
// 可能含 "data"），把模型常见的语法糖错误静默修掉（剥除已删属性/root 单对象包数组/cols 长度
// 纠偏/旧 list→bind_rows/旧 spacer→side:end）。notes（可为 nullptr）追加人类可读的修复摘要，
// 一条一行，供日志观察做了哪些静默修复。返回 false 时是「repair 表也判定必须拒绝重试」的
// 情形（如顶层 preset/slots 键），err 写清楚原因——调用方应把它当 Validate 失败处理，不必
// 再跑 Validate；返回 true 时调用方应接着跑 Validate（envelope 里的 "root" 可能已被替换成
// 新分配的 cJSON，调用方须重新 cJSON_GetObjectItem(envelope, "root") 取最新指针）。
bool Repair(cJSON* envelope, std::string& err, std::vector<std::string>* notes);

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
// row_all_growable: 仅在 parent_flow==FLOW_ROW 时有意义——本节点所在这一行的 JSON children
// 是否清一色都是 IsGrowable 类型（RenderNode 自己的 row 子节点遍历处算出并传给每个孩子）；
// 决定 button/choice 是均分整行还是内容自适应宽（见 pi_card_render.cc 的 ApplySizing）。
// 非 row 场景/调用方不关心时传默认 true 即可。
lv_obj_t* RenderNode(lv_obj_t* parent, const cJSON* node, UiCard* card, const RenderLimits& limits,
                     int depth, int& node_count, std::string& err, int parent_flow = 0,
                     bool in_list_row = false, bool row_all_growable = true);

// update：把 props（cJSON 对象）套到已有控件上。未知字段忽略。
bool ApplyProps(lv_obj_t* obj, const cJSON* props, std::string& err);

// bind_rows 数据变化后的整 grid 重渲（F1 修复：UiCard::DataConsumer::List 消费者的落地
// 函数，见 pi_card_host.cc RefreshDataConsumers）。old_gobj 是首次渲染或上一轮重渲建出的
// grid 容器——本函数整体删除它、用当前 card->data 重新走一遍 solver::Solve + 建控件，
// 在原来的兄弟节点位置插回一个新对象并返回；调用方必须用返回值刷新自己持有的指针（旧的
// old_gobj 已失效，即使返回 nullptr 也一样——失败时该 grid 从此空着，不做整卡回滚，同
// v1 rollback 代价过高的判断）。grid_spec 是 card->json_pool 里持久化的完整 grid 块 JSON
// （cells/rows/bind_rows 三形态皆可，实际只会传 bind_rows）；viewport_w/gap 是首次渲染定下
// 的几何（同一张卡运行期不会变宽，不必每次重算）。
lv_obj_t* RebuildBindRowsGrid(lv_obj_t* old_gobj, const cJSON* grid_spec, UiCard* card,
                              int viewport_w, int gap, const RenderLimits& limits, std::string& err);

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
// 流式生长卡片 v2（docs/CARD_V2.md §4，改造4）专用的"预览渲染"——只画外观，零 bind/零事件/
// 零 id 注册进 card->nodes/零 DataConsumer 登记，因为流式阶段既没有 UiCard 也没有校验过的
// 合法 bind 路径。v1 的"生长边状态机"（RenderPreviewNode/SyncPreviewNode/
// PreviewSyncContainer/USER_2 标记/s_leaf_sig/RefreshPreviewDataLabels，靠叶子级/容器级增量
// 对齐维持前缀不变量）已整体删除：v2 树深恒为 2（card→grid→leaf），grid 是原子渲染单位，
// 每帧靠 pi_card_preview_sig::GridSignature 判定"这个 grid 变没变"，变了就整块删旧重建、
// 没变就原样不动（见 pi_card_preview.cc 的 s_grid_sig[]）——不再需要按位置对齐的增量同步。

// 预览卡片外观容器：镜像 RenderNode 顶层 card_root 的建法（圆角/边框/pad/竖排间距，见
// pi_card_render.cc 的 RenderNode），供 preview.cc 首次建会话时调用一次；此后各 grid 块作为
// 它的子节点整块进/整块出。
lv_obj_t* MakePreviewCardRoot(lv_obj_t* parent, int viewport_w);

// v2 预览：单个 grid 块整块渲染——复用正式 RenderNode 路径同一套"solver::Solve + 按 layout
// 落位"哑翻译逻辑（RenderGridBlock 的表头/divider 合成行、bind_rows 展开与本函数共享，见
// pi_card_render.cc 的 RenderColsHeaderRow/RenderColsDividerRow/BuildBindRowsForRender），
// 但叶子零 bind/零事件/零 DataConsumer——数值控件/label 的当前值用 DataHub 快照直读
// （PreviewPeekInt/PreviewSeedBindLabel），不订阅、不随后续更新联动，安全路径流式期即显真实
// 值，读不到的维持 "--"/JSON 静态值/控件默认。chart/stock_chart（数据驱动或有网络副作用）
// 预览期跳过不建，只占位不占用真实资源。返回新建的 grid 容器（挂在 card_root 下）；调用方
// （preview.cc 的 s_grid_sig[]）负责在签名变化时先 lv_obj_delete 旧容器再调用本函数重建。
lv_obj_t* RenderGridBlockPreview(lv_obj_t* card_root, const cJSON* grid_json, const cJSON* data,
                                 int viewport_w, int gap);

// 预算重算：调用方（PreviewOnArgs）按**当前快照的 JSON 树**统计节点数，用于夹住预览的
// grid/节点上限（§6.1 的 kMaxGrids/64 节点，预览期超限静默不再新建，不报错——预览允许
// "半棵树"）。口径必须是 JSON 节点，不能数 lv 对象——复合控件（choice 的分段按钮等）内部
// 对象会把预算数爆。
int CountSpecNodes(const cJSON* node);

// 流式预览的 partial data 上下文：PreviewOnArgs 每帧把快照顶层的 "data" 对象借给
// RenderGridBlockPreview（非 object/缺失传 nullptr 等效清空）；GridSignature 已经把 data 折进
// 每个 grid 的签名，data 迟到/变化会让相关 grid 的签名变化、整块重渲，不再需要额外的全树回刷
// 通道。帧处理完必须再调一次传 nullptr 清空——指针指向即将被 cJSON_Delete 的快照树，绝不允许
// 跨帧存活。
void PreviewSetData(const cJSON* data);

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
