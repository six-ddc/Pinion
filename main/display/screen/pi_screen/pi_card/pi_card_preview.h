#pragma once

// ---------------------------------------------------------------------------
// pi_card::Preview —— 流式生长卡片会话状态机 v2（docs/CARD_V2.md §4，改造4）
//
// ui_render 的参数在 SSE 流入期间，pi_agent_task.c 每收到一片 delta 就把当前累积文本重新
// parse 成一棵"尽力而为"的 partial cJSON 树，序列化后经 UI_TOOL_ARGS 事件传到这里（drain
// 侧，LVGL 线程）。v2 schema 下 "root" 是 grid 块的竖排数组（§1.1），本模块按
// pi_card_preview_sig::GridSignature 给每个 grid 下标算一个扁平签名（s_grid_sig[]，
// §4.2）：下标第一次出现时整块建一次；此后只有"当前最后一个" grid 的签名每帧还会比对——
// 前面的 grid 一旦其后出现了新 grid 就永久冻结，不再触碰（§4.1 两条生长边：root 数组追加新
// grid / 末尾 grid 内容生长）。data 折进签名（XOR 数据切片哈希），迟到/变化会让引用它的 grid
// 签名跟着变，自然触发整块重渲——不需要 v1 那种全树回刷通道。
//
// 零订阅：零 bind observer、零事件、零 id 注册、零 DataConsumer——这时既没有 UiCard，也没有
// 校验过的合法路径；bind 控件的值走 DataHub::ReadForWorker 一次性快照直读，安全路径流式期即
// 显真实值，读不到的维持 "--"/缺省占位（见 pi_card_render.cc 的 PreviewPeekInt/
// PreviewSeedBindLabel）。流吐完、真正的 pi_card_tool_render 校验通过、UI_CARD_RENDER 到达
// 时，由正式渲染在同一个 row 容器里"adopt"（接管）这个预览、原地换装成带真实 bind/事件的正式
// 卡，观感上是一帧换装、不跳行。
//
// 生命周期由 pi_screen.cc 的 DrainQueueTick 驱动（详见各函数头注）：
//   UI_TOOL_START → PreviewOnToolStart（记住这次是不是 ui_render 在吐字）
//   UI_TOOL_ARGS  → PreviewOnArgs（每 tick 只处理一条，见函数头注）
//   UI_CARD_RENDER 处理前 → PreviewAdopt（正式渲染接管预览行，若有）
//   UI_TOOL_END   → PreviewOnToolEnd（校验失败等场景兜底撤除）
//   UI_AGENT_START/UI_ERROR/UI_DONE，以及每 tick 顶部发现 session gen 变化 → PreviewTeardown
//
// 只在 chat 模式生长；overlay/standby（非默认 display）一律不预览（v2 已删 preset，不再需要
// 那一条判据）。预览行是 feed 的普通子节点，ClearFeed/屏卸载会连同带走——不会悬垂（挂了
// DELETE 回调自清）。
// ---------------------------------------------------------------------------

#include <cstdint>

#include "lvgl.h"

namespace pi_card {

// UI_TOOL_START 消费：tool_name 是这次开始流式吐字的工具名（可能为 nullptr）。若上一轮预览
// 还没被 PreviewOnToolEnd/PreviewAdopt 处理（防御性兜底，理论不会发生），先撤掉，不跨工具
// 调用残留；复位"本次是否已放弃预览"（disqualified）状态。
void PreviewOnToolStart(const char* tool_name);

// UI_TOOL_ARGS 消费：partial_json 是这次 delta 累积出的 partial 树快照字符串（drain 侧重新
// cJSON_Parse，不能指望复用 worker 侧的树）。gen 是本次 drain tick 的 session gen（用于预览
// 首次建行时记下诞生代次，供 PreviewTeardown 判断是否该因新会话/打断被撤）。仅当当前正在流式
// 的工具是 ui_render 时才处理；一旦顶层 display 值确定不是 "chat"（即不再是 "chat" 的前缀——
// partial parser 会把半吐的值补全成完整字符串，"ch" 这类前缀要继续等而不是误杀），本次工具
// 调用永久放弃预览（若已建了行则立即撤除）。root 还不是数组时安静等待，不算错。
// 调用方须保证：同一个 DrainQueueTick 里最多调用一次（多条 ARGS 落在同一 tick 时，其余的
// evt.s1 直接 free，不重复调用——见 pi_screen.cc 的接线注释），避免一个 tick 内被同一批
// 挤压的 delta 反复重渲。
void PreviewOnArgs(const char* partial_json, uint32_t gen);

// UI_TOOL_END 消费：这次工具调用的 execute 已经跑完（含同步校验 + 若通过则已入队
// UI_CARD_RENDER）。若预览此时仍然存活（没有被 PreviewAdopt 取走），说明校验失败/未生成合法
// root/其它原因导致没有真卡片跟上——撤除预览，不留孤儿行挂在聊天流里。
void PreviewOnToolEnd();

// 强制撤除当前预览（若有）：UI_AGENT_START、UI_ERROR、UI_DONE 等场景下调用，防止预览跨轮次
// 残留。删 row（若存在）触发其 DELETE 回调自清内部状态；幂等——重复调用/预览本就不存在时是
// 空操作。
void PreviewTeardown();

// 会话代次校验：DrainQueueTick 顶部（算出本 tick 的 cur_gen 之后）调用一次。预览存活且诞生
// 代次与 cur_gen 不符（新会话/barge-in 在预览生长期间发生）→ 撤除。幂等，预览不存在或代次
// 未变时是空操作。
void PreviewCheckGen(uint32_t cur_gen);

// UI_CARD_RENDER 处理前调用：若存在一个仍存活、合格（未被 disqualify）的预览，返回其 row 并
// 放弃所有权（调用方——CardBeginRow——接管后须 lv_obj_clean(row) 清掉预览子节点再继续渲染
// 真卡片）；否则返回 nullptr（调用方走老路径新建一行）。
lv_obj_t* PreviewAdopt();

// 测试/调试用：返回当前预览会话的卡片外观容器（MakePreviewCardRoot 建出的那个，子节点是各
// grid 块——对应 root 数组），无预览或还没长出内容时为 nullptr。仅供 sim 验收脚手架窥探节点
// 指针跨帧稳定性用（见 sim/main.cc 的 previewfeed 命令），产品代码不应依赖它。
lv_obj_t* PreviewDebugTree();

}  // namespace pi_card
