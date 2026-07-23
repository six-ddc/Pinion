// pi_card_preview_sig.h —— CARD V2 流式预览的扁平签名机制（docs/CARD_V2.md §4.2）。
//
// 零 LVGL / 零 ESP 依赖：只用 cJSON + 标准库计算签名，可在宿主单测（sim/tests/
// preview_prefix_test.cc）里直接复用，不发明第二份哈希逻辑。pi_card_preview.cc 用它决定
// "root 数组第 i 个 grid 这一帧该不该整块重建"（§4.2 的 s_grid_sig[] 生长边判定）。
//
// 签名 = grid 块自身 JSON 的紧凑序列化哈希，XOR 上它引用到的 data 切片哈希——data 迟到/变化
// 时签名跟着变，触发整块重渲（§4.3 "无需专门的迟到属性补偿代码"）。"引用到的 data"覆盖两类
// 绑定：bind_rows（动态行遍历的数组）与 bind_data（单个 label 的标量插值）——只窄地覆盖
// bind_rows 会漏掉纯 cells/rows 里裸 bind_data 标签的数据迟到场景（v1 曾专门为它写
// RefreshPreviewDataLabels 全树回刷通道，v2 靠这里的签名把它一并收敛掉，不必再开小灶）。
#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"

namespace pi_card {
namespace preview_sig {

// FNV-1a over raw bytes。
uint32_t Fnv1a(const char* data, size_t len);

// node 的紧凑 JSON 序列化（cJSON_PrintUnformatted）后取 FNV-1a；node 为 nullptr 时返回 0。
// 不修改 node（cJSON_PrintUnformatted 只读遍历），入参可以是 const。
uint32_t HashCompactJson(const cJSON* node);

// 一个 grid 块的整块签名：HashCompactJson(grid_json) XOR grid 引用到的每个 data 切片的哈希
// （多个切片按 key 排序去重后逐个 XOR 进去，与遍历顺序无关，满足 §7.1 P2 幂等：同一输入两次
// 求出的签名必然相等）。data 为 nullptr，或 grid 不含 bind_rows/bind_data，退化为纯结构签名。
uint32_t GridSignature(const cJSON* grid_json, const cJSON* data);

}  // namespace preview_sig
}  // namespace pi_card
