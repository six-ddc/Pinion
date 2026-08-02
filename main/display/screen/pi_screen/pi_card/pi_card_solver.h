// pi_card_solver.h —— CARD V2 纯函数布局求解器（grid-only 重构核心）。
//
// 见 docs/CARD_V2.md §2。solver 把「意图树（cJSON）+ 视口宽 + 文本测量回调」求解成每个 cell 的
// 确定像素几何（列数 / 折行 / 每列像素宽 / 每 cell 的 x/w/对齐 / 是否截断），渲染器只做哑翻译。
//
// 硬约束（§11）：**零 LVGL、零 ESP 头文件依赖**，几何 = 纯整数运算 + 注入的 measure 回调，
// 可在 macOS 宿主单独编译单测。只准标准库 + cJSON.h。
#pragma once

#include "cJSON.h"

namespace pi_card {
namespace solver {

// label 的 role 字号阶梯（§1.4）。作为 int code 透传给 MeasureFn，宿主 stub / 真机包装各自
// 按 code 映射到字号。kRoleNone = 无 role 的正文/按钮文本（默认 20 号）。
enum Role {
    kRoleNone = 0,
    kRoleEyebrow,   // mono_14
    kRoleKicker,    // mono_14 accent
    kRoleSection,   // mono_14
    kRoleTitle,     // puhui_30
    kRoleHeading,   // puhui_24
    kRoleLabel,     // puhui_20 dim
    kRoleValue,     // mono_20 tx
    kRoleCaption,   // mono_14
};

// 文本测量回调（注入）：返回 utf8 串在指定 role 字体下的像素前进宽度。
// 宿主单测提供等宽近似 stub（ASCII=8px、CJK=16px，按 role 字号比例缩放）；真机提供
// lv_txt_get_size 包装。ctx 透传。
using MeasureFn = int (*)(const char* utf8, int role, bool mono, void* ctx);

// 设计常量（§2.1；solver 内部，非 schema）。视口宽由调用方按 display 传入，solver 不硬编码。
constexpr int kCardWChat = 600;      // chat 内联卡内容宽
constexpr int kCardWOverlay = 532;   // overlay 卡内容宽
constexpr int kStackGap = 12;        // grid 块竖排间距 / cell 间距 / 行间距（统一一个值）
constexpr int kTouchMinH = 44;       // 交互控件所在行最小触控高度
constexpr int kFillInset = 12;       // 带 fill/bg 底色的 grid 块的内容内边距（四周）

// bind 路径类型提示（注入，可空）：返回 0=未知 1=数值(Int/Bool) 2=字符串。字符串 bind 的
// 活值宽度不可预测（没有 fmt 代表串可言），必须按文本列处理（§2.2）——纯函数拿不到
// DataHub，由调用方注入（render/preview 用 DataHub::TypeOf 包装，宿主单测用 stub）；
// 不注入时退回 fmt/mono 启发式（"%s" 仍视为字符串证据）。
using BindKindFn = int (*)(const char* path, void* ctx);

struct Input {
    const cJSON* root = nullptr;   // 信封里的 root 数组（grid 块列表）
    const cJSON* data = nullptr;   // 卡级 data（bind_rows 展开用），可为 nullptr
    int viewport_w = 0;
    int gap = kStackGap;
    MeasureFn measure = nullptr;
    void* measure_ctx = nullptr;
    BindKindFn bind_kind = nullptr;
    void* bind_kind_ctx = nullptr;
};

// 纯函数，确定性。返回新分配的 layout cJSON（调用方 cJSON_Delete）。结构见 §2.6：
// {"grids":[ {"ncol":N,"track_w":[..],"h_hint":H,"inset":I,
//             "cells":[ {"gi":..,"ci":..,"row":r,"col":c,"span":s,
//                        "x":X,"w":W,"align":"start|center|end",
//                        "truncate":bool,"wrap":"wrap|ellipsis|nowrap"} ] } ]}
// inset：该 grid 声明了 fill/bg 底色时为 kFillInset，否则 0。solver 已按 viewport_w-2*inset
// 求解该 grid 的所有几何（cell x/w 相对内容区原点）；渲染器负责给容器补同宽的 pad。
// wrap 三态是渲染器实际消费的文本排布语义（truncate 仅兼容保留，"是否真的超宽会截断"的
// 精确判定）：
//   "wrap"     —— 独占一整行的正文，允许多行折行撑高，渲染器不钳单行高。
//   "ellipsis" —— 与别的 cell/行共享有限宽度的文本（cells 里跟其它 cell 挤同一行 / rows 表格
//                  文本列），单行 + 省略号，渲染器钳单行高（LV_SIZE_CONTENT 下 DOT/CLIP 必须
//                  钳高才真正生效，否则形同虚设按自然多行撑高）。
//   "nowrap"   —— 数值列/非文本控件，单行不截断，solver 保证轨道宽 ≥ 真实测量宽。
cJSON* Solve(const Input& in);

}  // namespace solver
}  // namespace pi_card
