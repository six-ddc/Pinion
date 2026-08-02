#pragma once

// ---------------------------------------------------------------------------
// pi_card XML 线格式编译器（docs/CARD_XML.md）——纯函数、零 LVGL/零 ESP 依赖，双端编译。
//
// LLM 产出 HTML 式 XML 串（<card>…</card>），本模块把它编译成与 JSON 通道**完全同构**
// 的 cJSON 信封 {"display","ttl_ms","card","data","root":[grid,…]}，之后走既有的
// Repair/Validate/Lint/solver/渲染器同一漏斗（pi_card_host.cc 不分叉）。
//
// 解析器是自研宽容 SAX：IDF v5.5.4 已无 expat 组件（v5 移除），且 §2.6 的宽容语义
// （未闭合自动闭合/裸 & 当字面量/截断照编译）本就不是严格解析器的行为——闭词表 + 深度
// 恒 2 的场景自研反而更小更贴。宽进严出：词表外的 HTML 标签按 §2.5 映射或剥壳，未知
// 属性剥除，一切降级都写 note，绝不因词表外溢整卡失败。
//
// 流式预览复用同一函数：前缀截断的 XML（含 tag 中间截断）喂进来输出「当前已收内容」的
// 合法信封——尾部残缺 token 丢弃、未闭合元素全部自动闭合，编译确定性保证已冻结前缀的
// grid JSON 逐帧一致（preview_sig 整块签名不抖）。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "cJSON.h"

namespace pi_card {

// xml[0..len) → 信封 cJSON（*out_args，调用方 cJSON_Delete）。notes（可 nullptr）追加
// 人类可读的降级/容错摘要（与 host Repair 的 notes 同口径，随 hints 回给 LLM）。
// 返回 false 仅当整段输入连一个元素都没有（err 给可修复的引导话术）；其余一切输入都
// 尽力编译成功。out_args 至少含 "root"（可能为空数组——空卡交给 Validate 给友好错误）。
bool XmlCompile(const char* xml, size_t len, cJSON** out_args, std::vector<std::string>* notes,
                std::string& err);

}  // namespace pi_card
