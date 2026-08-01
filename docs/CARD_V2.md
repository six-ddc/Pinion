# pi_card v2 —— grid-only 声明式 UI 重构规格书

> 状态：**v2.3：终版，三轮独立核验通过**（实现与文档全量对齐；沿革：v2.1 修订 solver 暴露的 6 处
> 规格矛盾 §2.2/§2.3，v2.2 步骤 5 收官回写实施期偏离，v2.3 补文本折行三态 `wrap` 与测量字体一致性
> 约束）。本文档是 pi_card 从「自由布局 schema（17 节点/~55 属性）」减法重构到
> 「grid-only + 纯函数 solver」的权威实施规格。方向已在产品讨论中拍板（见本仓 CLAUDE.md /
> MEMORY），本文只做细化和补洞，不推翻既定方向。
>
> 红线：**旧实现能删则删，无兼容层、无过渡期**（refactor-no-baggage）。v1 的 column/row/spacer/
> 旧 grid、justify/align/grow/gap/pad/cols 权重/span/w/h、preset/slots/ExpandPreset、
> `SyncPreviewNode`/`PreviewSyncContainer` 生长边机器，全部删除。

---

## 0. 为什么重构（一句话）

v1 把布局决策（几列、怎么折行、谁对齐、谁拉伸）交给了 LLM 用 justify/align/grow/cols-fr/span
拼，模型经常拼错 → grid 列塌缩、row 挤行、列不对齐、动态 label 塌 0 宽。近半年的 fix 高度集中
在「flex/grid 自适应推断」和「流式预览生长边状态机」两处（见 render.cc 1010-1142 的六层 FLOW_*
推断注释、preview.cc 的 s_leaf_sig 缓存）。v2 的赌注：**布局决策从 schema 里彻底拿走，交给一个
可宿主单测的纯函数 solver**，模型只声明「有哪些控件、语义是什么」，几何由固件按尺寸契约算死。

---

## 1. v2 JSON schema

### 1.1 顶层信封（保留，键不变）

```jsonc
{
  "display": "chat" | "overlay" | "standby",   // 缺省 chat
  "ttl_ms": <int>,                             // overlay 自动关闭；缺省 0
  "card":   "<id>",                            // 复用/定位；standby 恒为 "pin"
  "data":   { "<key>": scalar | array },       // 卡级数据模型（bind_data/bind_rows 用）
  "root":   [ <grid>, <grid>, ... ]            // ★ v2：root 是 grid 块的竖排数组
}
```

- 与 v1 唯一的结构差异：`root` 从「一个 node 对象」变成「grid 块的数组」。卡 = 若干 grid 竖排，
  块之间由固件用固定间距（`GRID_STACK_GAP`，见 §2.1）拼接，模型不写块间距。
- `preset`/`slots` 键**删除**（§3 决策 A）。
- 树深固定 = 2：卡（root 数组）→ grid → 叶子 cell。**禁止任何嵌套容器**。

### 1.2 grid 块：三种形态（互斥，靠出现的键判定）

一个 grid 对象必须恰好含 `cells` / `rows` / `bind_rows` 三者之一：

**形态 A —— `cells`（一维流式折行）**
```jsonc
{ "cells": [ <leaf>, <leaf>, ... ], "fill"?: <token> }
```
- 一维 cell 列表。**一行放几个由 solver 按尺寸契约决定，模型不决定折行**。
- 语义 = flex-wrap：从左往右贪心塞，塞不下换行，每行独立分宽（跨行不对列）。
- 适用：控制排、按钮组、图标+滑块+数值行、标题头部。

**形态 B —— `rows`（二维对齐表格）**
```jsonc
{
  "cols"?: [ {"title"?: "项目", "num"?: false}, {"title"?:"今日","num":true}, ... ],
  "rows":  [ [<leaf>, <leaf>, ...], [<leaf>, ...], ... ],
  "fill"?: <token>
}
```
- 二维。**所有行共享同一套列宽轨道**（表格/键值/表单/仪表盘）。
- `cols` 可选表头：数组长度 = 列数，每项 `{title?, num?}`。`title` 渲染为 section 表头行；
  `num:true` 标记该列为**数值列**（右对齐 + mono + 不截断，见 §2.4）。缺 `cols` 时列数 =
  各行 cell 数的最大值，数值列由 solver 按数值列门槛推断（mono / 数值 bind / fmt 数值转换 /
  静态数值文本，见 §2.2；裸 role:value 纯文本不算）。
- 每行一个 cell = 竖排列表（菜单）。这是 v1 一列全宽按钮 / list 的替代。

**形态 C —— `bind_rows`（数据驱动的动态行，替代 v1 list）**
```jsonc
{
  "item":     [ <leaf>, ... ] | <leaf>,   // 行模板：一行的 cell 数组，或单 cell
  "bind_rows":"<data-key>",               // 遍历 data[key] 数组，每元素克隆一份 item
  "cols"?:    [ {...} ],                   // 同形态 B
  "max"?:     <int>,                       // 行数上限（同 v1 list max，夹 [1,20]）
  "empty"?:   "<空数组兜底文案>"
}
```
- 行模板字符串里 `{i}`(0基)/`{n}`(1基)/`{item.FIELD}` 占位符，语义与 v1 list 完全一致
  （SubstRecord 复用）。
- 行内 action 仍受限：只允许 report/set/close，不允许 toggle/patch/show/hide（下标不稳定，
  同 v1 in_list_row 约束）。

> `bind_rows` 与 `rows` 是同一套对齐轨道求解器的静态/动态两个入口——静态行来自 schema，动态行
> 来自 data。solver 不关心行来自哪里，渲染器在建树前把 data 展开成行数组即可复用同一 solve。

### 1.3 叶子 cell 属性表

保留的叶子类型（12 种，一个不加不减）：
`label · button · slider · arc · switch · bar · icon · divider · qrcode · choice · chart · stock_chart`

| 属性 | 适用类型 | 说明 | v1 变化 |
|---|---|---|---|
| `type` | 全部 | 叶子类型枚举 | 不变 |
| `text` | label/button | 文本；label 支持 `{value}` 内联 | 不变 |
| `role` | label | eyebrow\|kicker\|section\|title\|heading\|label\|value\|caption | 不变 |
| `variant` | button | primary\|ghost\|plain\|default | 不变 |
| `mono` | label | 强制等宽（数值） | 不变 |
| `fmt` | label | 单占位符，须与 bind 类型相容 | 不变 |
| `icon` | button/icon | Lucide 名 | 不变 |
| `bind` | label/slider/arc/switch/bar/choice | DataHub 路径 | 不变 |
| `bind_data` | label | 显示 data[key] 标量 | 不变 |
| `bind_history` | chart | 历史路径 | 不变 |
| `points` | chart | 采样点数 8-120 | 不变 |
| `value/min/max` | slider/arc/bar/choice | 初值/量程/选中索引 | 不变 |
| `checked` | switch | 初值 | 不变 |
| `options` | choice | 2-6 字符串 | 不变 |
| `symbol/name/mode` | stock_chart | 行情标的 | 不变 |
| `tone` | 全部 | 语义前景 token（见 §1.4） | 不变，**裸 color 删** |
| `id` | 全部 | 供 update/action target | 不变 |
| `hidden` | 全部 | 初始隐藏（配 show/hide/toggle 做展开详情） | 不变 |
| `on_click/on_change/on_release` | 交互控件 | action 数组（8 种 do） | 不变 |
| **`side`** | 任意 cell | **`"end"`**：推到行尾 / 右对齐（新增，§3 决策 F） | 新增 |

**删除的属性（从 schema 消失，出现即静默忽略，Validate 不报错以保前向兼容）**：
`justify · align · grow · gap · pad · cols(旧 fr 权重语义) · span · w · h · size · color · bg · children`。

> `fill` 从「叶子属性」提升为「grid 块属性」（§3 决策 F），值仍是语义 token。
> `size` 删除：字号由 role 阶梯给（label），icon/qrcode 用契约默认（§2.4）。见 §9 风险 R5。

### 1.4 语义 token / role / action（引用不变部分）

- **tone/fill token**（12 个，自动适配深浅主题）：
  `accent · accent_dim · ok · err · tx · dim · faint · card · card2 · line · line2 · bg`。
  **裸 `color`/`bg` 十六进制彻底删除**——只准 token。
- **role 字号阶梯**（label，映射见 render.cc:293-300，不变）：
  eyebrow/section/caption→mono_14；kicker→mono_14 accent；title→puhui_30；heading→puhui_24；
  label→puhui_20 dim；value→mono_20 tx。
- **8 种 action `do`**：`close/set/report/toggle/show/hide/patch/invoke`，事件 `on_click/on_change/
  on_release`——**语义与 schema 完全不变**，见 `pi_card_tools.h` 的 `$defs.action`。
- 工具名 `ui_render/ui_update`（`ui_close` 合并进语义未变）不变；DataHub 绑定、数值 label 插值
  动效——**机制与语义不变**，只是承载它们的节点从 row/list 变成 grid/bind_rows。
- **行级 fast path 取消**：v1 的 `DataConsumer` 行级增量更新（append/remove/replace 就地补/删/换一行）
  **在 v2 不再保留**。`bind_rows` 的任何数据更新（`ui_update` 的 data.set/append/remove/replace）运行时
  统一走「整 grid 删除 + 重新过 solver + 整块重建」的**单一路径**。理由：v2 的行是**共享轨道**的
  网格行，列宽轨道依赖**全部行内容**（某行新增一个长字段会改变整列轨道宽），行级增量绕过 solver
  无法保证「列宽和=容器宽」「同列对齐」等几何不变量；而设备规模（行数 ≤20）下整块重建成本可忽略。
  这与流式预览 grid 整块重渲（§4）是同一套「grid 是原子渲染单位」的口径。

### 1.5 四个典型卡（完整 JSON）

**① 设备控制（cells 头部 + cells 控制排 + divider + cells 底部按钮）** —— 迁移自 v1 kCard0
```json
{"display":"overlay","root":[
  {"cells":[
    {"type":"label","role":"eyebrow","text":"PI CONTROL"},
    {"type":"label","role":"title","text":"设备控制"}]},
  {"cells":[
    {"type":"icon","icon":"volume"},
    {"type":"slider","bind":"audio.volume"},
    {"type":"label","role":"value","bind":"audio.volume","fmt":"%d%%"}]},
  {"cells":[
    {"type":"icon","icon":"sun"},
    {"type":"slider","bind":"display.brightness"},
    {"type":"label","role":"value","bind":"display.brightness","fmt":"%d%%"}]},
  {"cells":[
    {"type":"icon","icon":"battery"},
    {"type":"bar","bind":"battery.level"},
    {"type":"label","role":"value","tone":"dim","bind":"battery.level","fmt":"%d%%"}]},
  {"cells":[{"type":"divider"}]},
  {"cells":[
    {"type":"icon","icon":"wifi","tone":"ok"},
    {"type":"label","role":"label","text":"网络"},
    {"type":"switch","checked":true,"side":"end"}]},
  {"cells":[
    {"type":"button","variant":"ghost","text":"取消","on_click":[{"do":"close"}]},
    {"type":"button","variant":"primary","text":"确认",
     "on_click":[{"do":"report","text":"确认"},{"do":"close"}]}]}
]}
```
- 音量排：solver 见 [icon(固定22), slider(FILL,min160), value(数值,右对齐)] → icon 靠左自然宽、
  slider 吃掉剩余、value 右对齐 mono。三排 icon 等宽（同 grid 内 icon 列对齐由 cells 各行独立、
  但 icon 契约宽相同天然对齐）。
- 网络排：`switch` 带 `side:"end"` → 推到行尾右对齐（替代 v1 的 `{type:spacer}`）。
- 底部两 button 是纯控件行（全 growable）→ solver 均分整行。

**② 表格（rows + cols 表头 + divider 行 + 数值列右对齐）** —— 迁移自 v1 kCardGridBasic
```json
{"display":"overlay","root":[
  {"cells":[{"type":"label","role":"title","text":"网格表格"}]},
  {"cols":[{"title":"项目"},{"title":"今日","num":true},{"title":"昨日","num":true}],
   "rows":[
    [{"type":"label","text":"温度"},{"type":"label","text":"24"},{"type":"label","text":"22"}],
    [{"type":"label","text":"湿度"},{"type":"label","text":"60"},{"type":"label","text":"55"}],
    [{"type":"label","text":"气压"},{"type":"label","text":"1013"},{"type":"label","text":"1009"}]
  ]}
]}
```
- `cols` 声明 3 列，后两列 `num:true` → 右对齐 + mono + 不截断；首列文本列左对齐可截断。
- 表头 title 行由 `cols` 自动生成为 section 表头 + 其下一条 divider（solver 内建，见 §2.5），
  模型不再手写 `{divider span:3}`。

**③ 表单（rows：每行 label + 控件，共享轨道对齐）** —— 迁移自 v1 preset form
```json
{"display":"overlay","root":[
  {"cells":[
    {"type":"label","role":"eyebrow","text":"FORM"},
    {"type":"label","role":"title","text":"偏好设置"}]},
  {"rows":[
    [{"type":"label","role":"label","text":"亮度"},
     {"type":"slider","id":"bri","min":0,"max":100,"value":60}],
    [{"type":"label","role":"label","text":"静音"},
     {"type":"switch","id":"mute","checked":false}],
    [{"type":"label","role":"label","text":"模式"},
     {"type":"choice","id":"mode","options":["省电","均衡","性能"],"value":1}]
  ]},
  {"cells":[{"type":"button","variant":"primary","text":"保存",
    "on_click":[{"do":"report","text":"已保存"},{"do":"close"}]}]}
]}
```
- 无 `cols`：表单无需表头，ncol 由各行 cell 数自动推断（=2）。第 2 列混排 slider/switch/choice：
  solver 按列取各行 cell 的 max 契约宽定轨；label 列内容宽、控件列吃剩余；`switch` 契约 stretch==false
  保持 52px 右锚不随轨拉伸（§2.4 步骤 7），`choice` 在 rows 里占满该行剩余轨道（不 span-all，因为它
  在多列行内，见 §2.4 例外）。

**④ 菜单（rows：每行单 button；或 bind_rows 动态）** —— 迁移自 v1 kCard2 / preset menu
```json
{"display":"overlay","root":[
  {"cells":[
    {"type":"label","role":"eyebrow","text":"SELECT"},
    {"type":"label","role":"title","text":"选择操作"}]},
  {"rows":[
    [{"type":"button","text":"新建对话","on_click":[{"do":"report","text":"新建对话"},{"do":"close"}]}],
    [{"type":"button","text":"导出记录","on_click":[{"do":"report","text":"导出记录"},{"do":"close"}]}],
    [{"type":"button","text":"清空历史","on_click":[{"do":"report","text":"清空历史"},{"do":"close"}]}],
    [{"type":"button","variant":"ghost","text":"关闭","on_click":[{"do":"close"}]}]
  ]}
]}
```
- 每行一个 button cell → 竖排全宽按钮列表（button 在单列 rows 里 SPAN 该唯一列 = 全宽）。
- 动态版：`{"item":[{"type":"button","text":"{item.title}","on_click":[{"do":"set",
  "path":"media.play_index","value":"{i}"}]}],"bind_rows":"tracks","max":8}`。

---

## 2. solver 规格（纯函数布局求解器）

### 2.0 定位与不变式

solver 是**不依赖 LVGL、不依赖任何 ESP 头文件**的纯 C++ 函数，可在 macOS 宿主编译单测。
输入 = 意图树（cJSON）+ 视口宽 + 文本测量回调；输出 = 每个 cell 的**确定像素几何**（列数 /
折行 / 每列像素宽 / 每 cell 的 x/w/对齐 / 是否截断）。渲染器是「哑翻译器」：拿 solver 的像素
几何直接摆位，LVGL 只负责 cell **内部**的文本换行。

> **实现落位（与代码对齐）**：solver 输出的确定几何有两条落位通路，语义（确定性 / solver 算全部
> 几何）完全一致，只是 LVGL 承载不同：
> - **`cells` 形态用绝对定位**（`lv_obj_set_pos` + `lv_obj_set_size`，父容器不设 flex/grid）。原因：
>   cells 每行列数不同（flex-wrap 语义），逐行建 grid 轨道 dsc 不划算；solver 已算出每 cell 的
>   `(x, w, row高)`，直接绝对摆位最省、最贴 solver 输出。
> - **`rows`/`bind_rows` 形态用共享轨道**（`lv_obj_set_grid_dsc_array` 定像素轨道 + `set_grid_cell`
>   落位），保住「所有行共享同一列宽轨道」的对齐语义。
> 两条路的轨道宽/坐标都来自 solver，不存在 LVGL 运行时再推断几何。

> **决策（§3 决策 G）：solver 直接算像素，不输出 fr/CONTENT 轨道让 LVGL 自适应。**
> 理由：v1 的全部 grid bug（列塌 0、CONTENT 轨道量尺反馈环、动态 label 塌陷）根因都是把
> 「几何决策」留给 LVGL 的 fr/CONTENT 在运行时解。定像素轨道把这些决策前移到可单测的纯函数里，
> 逐像素可断言（TDD 不变量「列宽和=容器宽」要求已知像素）。fr 分配数学很便宜（剩余空间按权重
> 比例分，~15 行），在 solver 内复刻不构成实现负担；换来的是 sim 与真机**像素一致**、不再有
> 「LVGL 运行时才暴露」的布局崩坏。唯一留给 LVGL 的是 cell 内中文逐字折行（宽度已定，行高确定）。

### 2.1 设计常量（solver 内部，非 schema）

```cpp
constexpr int kCardWChat    = 600;   // chat 内联卡内容宽（feed 宽 - 边距）
constexpr int kCardWOverlay = 532;   // overlay 卡内容宽（wrap 80%×720=576 - 卡 pad 44）
constexpr int kStackGap     = 12;    // grid 块竖排间距 / cell 间距 / 行间距（统一一个值）
constexpr int kTouchMinH    = 44;    // 交互控件所在行最小触控高度
```
视口宽由**调用方**（host）按 display 传入（chat→600 / overlay→532 / standby→pin host 实测宽），
solver 不硬编码 display 判断。

### 2.2 逐控件尺寸契约表（solver 权威，给具体数值）

每个叶子登记 5 元组 `{min_w, pref_w, occupancy, align_default, stretch}`。`occupancy`：
`INLINE`（行内参与折行/分列）/ `FILL`（吃满所在轨道剩余宽）/ `SPAN_ALL`（独占一行、跨所有列）/
`SQUARE`（固定方形、独占行居中——**仅 qrcode 用**）。`min_w` 单位 px。

**数值列门槛（精确化）**：一个 label cell 获得「数值列特权」（右对齐 end + mono + **不截断**）当且
仅当满足以下**任一**：① `mono:true`；② `bind` 到数值路径（Int/Bool）；③ `fmt` 含数值转换符
（`%d`/`%u`/`%i`/`%.Nf` 等）；④ 无 bind 但**静态 text 整体是数值样式**（纯数字 / 带单位如 `"24"`、
`"1013"`、`"32C"`）。**裸 `role:value` 不构成数值列**——`role:value` 只决定字体阶梯（mono_20），若其
文本是普通字符串（未命中上述任一条件），按**文本列**处理（左对齐、可截断单行）。这条把「role 语义」
与「数值列几何特权」解耦：数值列由内容/绑定/fmt 判定，不由 role 名判定。命中数值列门槛的 cell 走
**NOWRAP**（单行不截断），其轨道宽以 **未 clamp 的 natural_w**（fmt 代表串/内容完整测量宽，不夹 400
上限）兜底保证装得下（wrap 三态见 §2.5）。

| 类型（细分） | min_w | pref_w | occupancy | align 默认 | 拉伸 | 备注 |
|---|---|---|---|---|---|---|
| label 文本样式（含裸 role=value 纯文本） | 0 | 测量值 clamp≤400 | INLINE | start(左) | 可拉 | 超宽截断 DOT（单行） |
| label **数值样式**（见下方门槛） | 由 **fmt 代表串**测量 | 同 min | INLINE | end(右) | 否 | **不截断**，数值列(mono) |
| label role=eyebrow/kicker/section/caption/title/heading | = 视口宽 | = 视口宽 | SPAN_ALL | start | — | 结构性文本各占整行；title/heading WRAP，眉标/小节单行 |
| button | max(72, text_w+32) | text_w+32 | INLINE | center | 纯控件行可均分 | 行高≥44 |
| slider | **160** | FILL | FILL | — | 是 | 硬保证≥可拖宽度 |
| arc | **120**（pref 132） | 132 | INLINE | center | 等径可放大 | 方形；参与折行，与兄弟同行；同 grid 内等径 |
| bar | 120 | FILL | FILL | — | 是 | 恒满轨道 |
| switch | 52 | 52 | INLINE | end(右) | 否 | 固定小尺寸 |
| icon | =字号(默认22) | =字号 | INLINE | start | 否 | 固定 |
| divider | 视口宽 | 视口宽 | SPAN_ALL | — | — | 高 1，跨所有列 |
| qrcode | 160(clamp 96-320) | 160 | SQUARE | center | 否 | 固定方形，独占行居中 |
| choice | seg数×44 | 视口宽 | SPAN_ALL(cells) / FILL(rows多列) | — | 是 | 分段满宽，例外见 §2.4 |
| chart | 视口宽 | 视口宽 | SPAN_ALL | — | — | 高**固定 120**（§3 决策 C） |
| stock_chart | 视口宽 | 视口宽 | SPAN_ALL | — | — | 内部高 260 自管 |

> role 文本 SPAN_ALL 的取舍（消决策 B 与 S7 的冲突）：eyebrow/kicker/section/caption/title/heading
> 是结构性文本（头部/小节标），一律各占整行左对齐，**不参与折行也不触发 S7 居中**；决策 B 的
> 「eyebrow+title 两个 label 各占一行」由此天然成立。只有 role=label/value 及无 role 的正文 label
> 才是 INLINE、参与折行与列分配。arc 不再独占行——它是方形 INLINE cell，可与 value label 同行
> （见 §5.2 idx7），SQUARE 独占居中现在只剩 qrcode。

**fmt 代表串规则**（数值列宽预测的核心）：数值 bind 的 value label 渲染时文本还空，不能按空文本
量 0 宽（v1 塌陷根因）。solver 用 fmt 生成**代表串**测量。整型转换规则：**当整个 fmt 恰为单个
`%d`/`%u`/`%i`**（除转换符外无其它字面/百分号）时用 5 个 `8`；否则（fmt 含后缀/前缀/多占位）每个
`%d`/`%u`/`%i` 用 3 个 `8`。浮点 `%.Nf` → `88` + `.` + N 个 `8`。示例：`%d`→`"88888"`、`%d%%`→`"888%"`、
`%d dBm`→`"888 dBm"`、`%.1f`→`"88.8"`。其余字面照抄。string bind（`%s` 或 bind_data 字符串）**不算
数值列**，按文本列处理（左对齐可截断，min_w 给一个 64px 兜底）。

### 2.3 `cells` 折行算法（形态 A，flex-wrap）

```
输入: cells[], viewport_w, gap
1. 预处理: 每个 cell 查契约得 (min_w, pref_w, occupancy, align, stretch)。
2. 顺序扫描, 累积当前行:
   - 遇 SPAN_ALL/SQUARE cell: 结束当前行(若非空), 该 cell 独占一行 → 换行。
   - 遇 INLINE/FILL cell: 若 (当前行已用宽 + gap + cell.min_w) > viewport_w → 换行开新行;
     否则加入当前行, 已用宽 += gap + cell.min_w(FILL 按 min_w 占位)。
3. 每行分宽与落位（各 cell 起于 min_w, 平摊剩余, 保证不越界）:
   - 行剩余 rem = viewport_w - Σmin_w - Σgap(=gap×(cell数-1))。每 cell 起始 w = min_w。
   - **右锚定分组**: 一个 cell 右锚定当且仅当 (a) 显式 side:"end", 或 (b) 其契约 align 默认为
     end(value 数值 label / switch)。右锚定 cell 靠行右边; 其余 cell 靠行左边; 二者之间的空隙
     由 stretch cell 吸收(见下)。一行内可有多个右锚定 cell(依声明序在右侧相邻排列)。
   - **剩余分配**:
     * 有 stretch cell(FILL 的 slider/bar, 或可拉 label): rem 平摊给各 stretch cell(按 pref_w
       权重, 无权重差异时等分), 吃在左右分组之间。保证 Σw + Σgap == viewport_w, 不越界。
     * 无 stretch cell: rem 留作左组与右组之间的空隙(左组左靠、右组右靠); 若全部 cell 同属一组
       (无右锚定)则:
         - 单个 cell 且 stretch==false → 居中(x = (viewport_w - w)/2)。 [S7: 仅 stretch==false]
         - 多个 cell → 左对齐, rem 留在行尾。
   - button/choice 均分整行仅当**本行全是 growable 控件**(button/slider/bar/arc/choice):
     此时视全部为 stretch, rem 平摊 → 等宽(min_w 相等时退化为均分整行); 混入 label/icon/switch
     则 button 退回内容宽(继承 v1 IsRowGrowConditional 语义)。
   - 越界兜底: rem<0(min_w 之和已超行宽)时按比例压缩非固定 cell, 固定契约 cell(icon/switch/
     数值 label 的 min_w)不压。
4. 输出每 cell 的 (row, x, w, align, truncate)。x 由左组累加 / 右组从右回填得出。
```
`cells` 跨行**不对列**——这是它与 `rows` 的本质分工。需要列对齐用 `rows`。

### 2.4 `rows` 轨道分宽算法（形态 B/C，共享列）

```
输入: rows[][], cols[](可选), viewport_w, gap
1. ncol = cols.length 或 max(每行 cell 数)。
2. 逐列求轨道宽 track_w[c]:
   - 收集第 c 列所有行的 cell (按行内下标, 缺列的行跳过)。
   - is_num[c] = cols[c].num || 该列所有 cell 都命中数值列门槛(mono/数值 bind/fmt 数值转换/静态
     数值文本, 见 §2.2; 裸 role:value 纯文本不算)。
   - fixed 列: 该列所有 cell 契约都是固定宽(icon/switch/数值 label) → track_w = max(min_w)。
   - text/stretch 列: 先记 min_w = max(该列各 cell min_w)。
3. 分配剩余: rem = viewport_w - Σtrack_w(fixed) - Σmin_w(stretch) - gap×(ncol-1)。
   rem 按 stretch 列均分(或按 pref_w 权重); rem<0 时按比例压缩 stretch 列(不压 fixed/数值列)。
4. 每 cell 对齐:
   - 数值列 → 右对齐(end) + 不截断 + mono。
   - 文本列 → 左对齐(start) + 超轨道宽截断 DOT。
   - side:"end" cell → 在其轨道内右对齐(覆盖列默认)。
5. SPAN_ALL cell(divider/chart/stock_chart, 或单列行里的 choice) → 该行跨所有列, 不参与列宽。
   FILL cell(slider/bar, 及多列行内的 choice) → 填满自己那一列的 track_w。
6. 等径: 同一 grid 内所有 arc 取统一直径 = min(各自 min_w 满足下的可用宽)。
7. 契约 stretch==false 的 cell(switch/icon/数值 label)即便落在被拉伸的轨道里, **保持契约宽、
   不随轨道拉伸**, 在轨道内按其契约默认对齐落位(switch 52px 右锚、icon 22px 左靠、数值 label
   按测量宽右锚)。轨道拉伸只作用于该轨道里 stretch==true 的 cell。
```
**choice 满宽 vs 折行的冲突消解**（§9 风险 R4）：choice 在 `cells` 里恒 SPAN_ALL（独占一行）；在
`rows` 多列行里退化为 FILL（填自己那一列）。规则由「所在 grid 形态 + 是否单列行」唯一确定，模型
无需关心。

### 2.5 对齐默认 & 内建装饰

- **文本折行三态 `wrap`**（solver 为每个 label cell 输出，渲染器直接照用，见 §2.6）：
  - **WRAP**（多行、不钳高）：`cells` 里 label 折行后独占一行（SPAN_ALL 化的正文），以及所有
    SPAN_ALL 正文（role=title/heading 等结构性文本）**恒 WRAP**——整段文本多行铺满、行高随内容长。
  - **ELLIPSIS**（单行 + DOT + 钳单行高）：`cells` 行内被兄弟挤窄的文本 label，以及 `rows` 的
    **文本列** cell——超轨道/槽位宽即单行截断省略号，钳死单行高不换行（防挤兄弟 / 防表格行错位）。
  - **NOWRAP**（单行、不截断）：**数值列 cell 与 FIXED 契约列**（数值 label / switch / icon 等）。
    轨道宽以该 cell **未 clamp 的 natural_w**（fmt 代表串/内容的完整测量宽，不夹 400 上限）兜底，
    **保证一定装得下、绝不省略数值**（数值被截断=读数错误，比换行更不可接受）。
- `cols` 表头：solver 自动在 rows 顶部插一行 section 表头（`cols[c].title` → role=section label，
  数值列右对齐）+ 其下一条 divider(SPAN_ALL)。模型不再手写表头行/divider span。
  **例外：`cols` 全部 title 为空（或 cols 缺省）时，不渲染表头行也不插 divider**——此时 cols 仅用于
  声明列数/标记 num 列，不产生可见表头。
- 卡=grid 竖排：root 数组每个 grid 块之间插 `kStackGap`；块内 `fill` token 给背景盒
  （ApplyFill 局部 radius，复用 v1 §kCardStyleFam 的 fill 容器样式）。
- 高度：solver 输出**高度提示**（advisory），**不作权威**——overlay 的真实高度仍由 `ReflowOverlay`
  量活体树决定（§3 决策 D）。cell 内中文折行导致的行高增长由 LVGL 决定，solver 不预测行高。

### 2.6 API 签名（C++，cJSON in / cJSON out，测量回调注入）

```cpp
// solver/pi_card_solver.h —— 零 LVGL / 零 ESP 依赖, 可在 macOS 宿主单独编译。
namespace pi_card::solver {

// 文本测量回调(注入): 返回 utf8 串在指定字体下的像素前进宽度。
// role: 见 §1.4 role 枚举(定字体); mono/size 为无 role 时的回退。ctx 透传。
// 宿主单测提供等宽近似 stub(ASCII=8px, CJK=16px×size/20); 真机提供 lv_txt_get_size 包装。
// ★ 字体映射铁律: solver 测量用的字体必须与渲染字体**严格一致**——尤其 role:value 恒走
//   mono_20, 不受 JSON 的 mono 属性影响(role 优先, 见 render.cc:299 的 ApplyLabelStyle)。
//   若 solver 按 JSON mono 选字体、渲染按 role 选字体, 两者宽度不符 → 数值列轨道量偏、装不下
//   或错位(kCardGridTall/25_grid_tall 回归的根因教训)。measure 回调的字体决策必须复刻
//   ApplyLabelStyle 的 role→font 优先级, 不能只看 mono 参数。
using MeasureFn = int (*)(const char* utf8, int role, bool mono, void* ctx);

struct Input {
    const cJSON* root;      // 信封里的 root 数组(grid 块列表)
    const cJSON* data;      // 卡级 data(bind_rows 展开用), 可为 nullptr
    int          viewport_w;
    int          gap;       // = kStackGap
    MeasureFn    measure;
    void*        measure_ctx;
};

// 纯函数, 确定性。返回新分配的 layout cJSON(调用方 cJSON_Delete)。结构:
// {"grids":[ {"ncol":N,"track_w":[..],"h_hint":H,
//             "cells":[ {"gi":..,"ci":..,"row":r,"col":c,"span":s,
//                        "x":X,"w":W,"align":"start|center|end",
//                        "wrap":"wrap"|"ellipsis"|"nowrap","truncate":bool} ] } ]}
// "gi"/"ci" = grid 下标 / grid 内 cell(或展开后行内 cell)下标, 供渲染器回指原 JSON 叶子。
// "wrap" 文本三态(见 §2.5): 渲染器据此设 LONG_WRAP / LONG_DOT+单行钳高 / LONG_CLIP 单行不截。
//   ("truncate" 保留为 wrap==ellipsis 的等价布尔, 冗余兼容旧断言, 新代码读 wrap。)
cJSON* Solve(const Input& in);

}  // namespace pi_card::solver
```
- 输入输出全 cJSON → 天然支持 golden 文件单测（喂意图 JSON + stub measure，比对输出 layout JSON）。
- 渲染器按形态取 solver 输出落位（`cells`→绝对定位 `set_pos/set_size`；`rows`/`bind_rows`→定像素
  grid dsc + `set_grid_cell`），按 `.align` 设对齐、按 `.wrap` 三态设长文本模式（`wrap`→LONG_WRAP
  多行不钳高 / `ellipsis`→LONG_DOT 单行+钳单行高 / `nowrap`→LONG_CLIP 单行不截），**不再自己
  推断任何几何**。`track_w` 仅 rows/bind_rows 通路使用；cells 通路用每 cell 的 `x/w`。

---

## 3. 六个开放决策（结论 + 理由）

| # | 决策点 | 结论 | 理由（一句） |
|---|---|---|---|
| A | preset(confirm/form/dashboard/menu) 去留 | **整删**。删 `ExpandPreset` + `BuildConfirm/Form/Dashboard/Menu` 四个 + slots 校验 + schema 里 preset/slots 键 | v2 schema 已足够简单，prompt 里给 4 个完整示例卡即可替代；preset 是 v1「schema 太难写」的补丁，根因已被 grid-only 消除 |
| B | 标题区 eyebrow+title | **无需新构造**：一个 `cells` grid 放两个 label（role:eyebrow + role:title），二者都是 SPAN_ALL 文本 → solver 天然各占一行竖排 | 复用 role 阶梯 + SPAN_ALL 折行，不加旋钮 |
| C | chart 高度（v2 删 h） | **固定档 120px**（契约常量），不按内容不给档位 | 删 h 后需确定值；120 是 v1 既有默认且视觉验证过；stock_chart 保留内部 260 自管 |
| D | overlay 超限滚动 | `ReflowOverlay` **原样保留**：量活体树自然高度，≤86% 屏高跟手收缩、超出钉高度开竖滚 | grid 竖排本质仍是垂直子对象堆叠，Reflow 逻辑与布局形态无关；solver 高度仅 advisory，权威高度用实测 |
| E | 流式预览生长语义 | 深度固定 2 → **两条生长边**：①root 数组追加新 grid ②末尾 grid 内容生长。签名机制简化为**按 grid 下标的整块签名**（见 §4） | 树深固定消灭了 v1 的按深度 s_leaf_sig + 生长边路径机器，grid 是原子块整建整签 |
| F | 新增旋钮 | 仅三个，各有必要性：`side:"end"`（替代已删的 spacer 推挤，右对齐语义无替代）、grid 级 `fill`（背景盒分组，替代已删的 fill 容器）、`bind_rows`（替代 list+bind_data，深度 2 下 list 无处嵌套）。**不再新增其它** | 每个都是「删除某物后语义缺口的最小补偿」，非新功能 |
| G | solver 承载形式 | **直接算像素**（定像素轨道），不输出 fr/CONTENT 让 LVGL 自适应 | 确定性 > 少写 15 行 fr 数学；v1 所有 grid bug 根因是把几何决策留给 LVGL 运行时；定像素让决策可单测、sim/真机逐像素一致 |

---

## 4. 流式预览 v2 语义

### 4.1 生长边定义（深度固定 2 → 只有两条）

1. **卡级生长边**：`root` 数组末尾追加新 grid。已出现的前序 grid 一旦其后出现了新 grid，即冻结
   （它已完整，签名不再变）。
2. **块级生长边**：`root` 数组**最后一个** grid 的内容（cells/rows/item + 迟到属性）还在长。

不存在第三层——没有嵌套容器，就没有 v1 的「容器里递归找最后一个孩子」的多层生长路径。

### 4.2 签名简化方案

删除 v1 的 `s_leaf_sig[depth]`（按深度）+ `PreviewSyncContainer`/`SyncPreviewNode`/USER_2 容器标记
+ 生长边路径机器。替换为一个**扁平的按 grid 下标的整块签名向量**：

```cpp
std::vector<uint32_t> s_grid_sig;   // 索引 = root 数组里的 grid 下标(容量 ≤ 卡内 grid 上限)
```
每帧对 partial 快照：
```
对 root[i] (i = 0..N-1):
  sig_i = FNV1a(grid_i 的紧凑 JSON) XOR FNV1a(该 grid 引用到的 data 切片)   // data 迟到自然触发重渲
  若 i >= s_grid_sig.size() 或 s_grid_sig[i] != sig_i:
     删除已建的 lv grid_i(若有) → 用 solver 整块重解 + 整块重建 → s_grid_sig[i] = sig_i
  否则: grid_i 一动不动(签名未变)
```
- **grid 是原子渲染单位**：整块解、整块建、整块签。规模受预览预算约束（≤64 节点、grid 数上限），
  整块重建成本可忽略——这正是 v1 grid 分支已验证可行的做法（render.cc:1995-2007），v2 把**整棵卡**
  都收敛到这个模式，不再有叶子级/容器级的增量生长特例。
- data 折进签名（XOR data 切片哈希）→ 迟到 data 自然触发相关 grid 重渲，删除 v1 的
  `RefreshPreviewDataLabels`/USER_4 全树回刷特殊通道。
  **实现注**：预览签名的 data 依赖覆盖两类消费者——`bind_rows`（行数据数组）与 `bind_data`
  （单标量标签）；二者引用的 data key 切片都参与该 grid 的签名哈希，任一类迟到/变更都触发其
  所在 grid 整块重渲。

### 4.3 迟到属性处理

v1「迟到属性只在 adopt 那刻生效」的整类问题（role/tone/justify/align/fill 晚到）在 v2 自动消失：
既然 grid 整块按签名重建，任何属性（含容器级）变化都改变该 grid 的 JSON → 签名变 → 整块重解重建，
迟到属性下一帧即生效。**无需**任何「每帧幂等重打容器属性」的补偿代码（v1 `PreviewSyncContainer`
的 ApplyPreviewContainerGeom/ApplyDefaultStyle 每帧重打，全删）。

### 4.4 前缀不变量（v2 天然成立）

因为每个 grid 每帧都从**当前快照的完整 JSON** 整块重解重建（从不做叶子级原地增量 patch），任意切分
点喂完的最终树 = 一次性渲染树。增量粒度是「整 grid」，而整 grid 重建是幂等的 → 前缀不变量近乎显然
成立（v1 靠叶子级生长边对齐勉强维持、fix 频发；v2 结构上保证）。

---

## 5. 全量迁移映射表

### 5.1 四个 preset → v2 写法

| preset | v2 结构要点 |
|---|---|
| confirm | `cells`头部(eyebrow+title) + 可选`cells`(body label) + `cells`(ghost/primary 两 button 均分)。完整见 §1.5① 底部两 button 段 |
| form | `cells`头部 + `rows`(cols:[{title},{}]，每行[label,控件]) + `cells`(submit button)。完整见 §1.5③ |
| dashboard | `cells`头部 + `rows`(cols 末列 num:true，每行[icon?,label,value])；bar 型指标行用 [label,bar,value] 三列 rows，bar=FILL 列 |
| menu | `cells`头部 + `rows`(每行单 button)；choice 型菜单 = `cells`([choice] + [button])。完整见 §1.5④ |

### 5.2 sim 26 张 kCards → v2 结构要点

| idx | 卡 | v2 迁移要点 |
|---|---|---|
| 0 | kCard0 设备控制 | §1.5① 完整给出；spacer→`side:"end"` |
| 1 | kCard1 确认框 | 头部 cells + 长正文 label（SPAN_ALL 自动 WRAP）+ 两 button cells |
| 2 | kCard2 菜单 | §1.5④ 完整；spacer 删（块间距自动） |
| 3 | kCard3 状态卡 | `rows`(cols:[{title:"项"},{num:true}])，每行[icon?,label,value]；spacer→数值列右对齐天然分离 |
| 4 | kCard4 换行压力 | 头部 + 长正文 label(SPAN_ALL WRAP) + `cells`(6 button → solver 折行成 2-3 行均分) |
| 5 | kCard5 退化兜底 | `cells`：min>max slider(渲染器兜底)、空 text button、未知 icon→dot、caption；空容器不存在(无嵌套) |
| 6 | kCardBadFmt | 不变：Validate 仍拒绝数值路径 `%s`(§6) |
| 7 | kCardArc | `cells`([arc, value label])；arc=INLINE 方形与 value 同行→arc 居左方形、value 右锚定右对齐 |
| 8 | kCardQr | `cells`([qrcode])；qrcode=SQUARE 自动居中，**删两侧 spacer** |
| 9 | kCardChoice | `cells`([title]) + `cells`([choice]) + `cells`([button])；choice SPAN_ALL |
| 10 | kCardPatch | `cells`([slider(id,on_change patch), value label])；grow 删，slider=FILL 天然吃剩余 |
| 11 | kCardP4aInfo | `rows` 键值表(cols:[{title},{num}])；divider 用单独 `cells`([divider]) 或分成两个 grid 块 |
| 12 | kCardP4aChart | 头部 + `cells`([section,value]) + `cells`([chart])×2；chart 高固定 120 |
| 13 | kCardP4bSensors | `rows` 键值表 + `cells`([divider]) + `cells`([invoke button]) |
| 14 | kCardP4cGps | `rows` 键值表 + `cells`([divider]) + `cells`(两 button 均分) |
| 15 | kCardMultiCol | **重点迁移**：A/B 段合并——直接用一个 `rows`(cols:[{title:城市},{title:温度,num},{title:天气}])，列天然对齐；v1 的「grow 才对齐/不 grow 不对齐」二选一问题在 v2 消失(rows 恒对齐) |
| 16 | kCardStockBind | `rows`(cols 标 num 列) 承载 stock.* 多路 bind；分隔用块拆分 |
| 17 | kCardMediaCtl | 头部 + `cells`(state/position) + `cells`([bar]) + `cells`(3 button 均分) + `bind_rows`(tracks 曲目)；list→bind_rows |
| 18 | kCardStockChart | `cells`([stock_chart])；w 删(SPAN_ALL 自动满宽)，overlay wrapper 宽由 viewport_w 传入 |
| 19 | kCardStyleFam | `cells`(4 variant button) + `cells`([slider]) + `cells`([bar]) + `cells`([label,switch side:end]) + `cells`([choice]) + `cells`([divider]) + grid `fill:"card2"`(块级 fill) |
| 20 | kCardJustify | **废弃删除**（justify 已删，无迁移对象）——同 refactor 红线 |
| 21 | kCardAlign | **废弃删除**（align 已删）——同上 |
| 22 | kCardGridBasic | §1.5② 完整；`divider span:3` → cols 表头自动 divider |
| 23 | kCardGridAuto | `rows`(cols:[{title:名称},{title:值}])；"auto"轨道概念删除，solver 按内容/剩余定轨 |
| 24 | kCardGridCtl | `rows`(cols 2 列，每行[icon/label, slider/switch])；控件混排由 §2.4 FILL/固定列规则处理 |
| 25 | kCardGridTall | `rows` 22 行键值表 → 超 overlay 86% 高触发 ReflowOverlay 滚动(§3 决策 D，不变) |

新增验收卡（建议补 2 张）：`kCardCellsWrap`（cells 折行：8 个不等宽 button 验贪心换行 + 每行均分）、
`kCardRowsAlign`（rows 数值列右对齐 + 文本列截断 + side:end + 等径 arc）。

---

## 6. 校验（Validate v2）与自动修复

### 6.1 Validate v2 规则清单

结构（新增/改动）：
- `root` 必须是**数组**，元素 ≥1、≤ `kMaxGrids`（建议 8）；每元素是 grid 对象。
- 每个 grid 恰含 `cells` / `rows` / `bind_rows` 之一（多于一个 → 拒绝）。
- `cells`：非空数组，元素都是**叶子**（type ∈ 12 类；出现容器类型 column/row/grid → 拒绝「no nested
  containers」）。
- `rows`：非空二维数组，每行元素都是叶子；`cols`(若给) 长度须 = 各行最大 cell 数（不匹配 → 提示修）。
- `bind_rows`：需 `item`(叶子或叶子数组) + `bind_rows`(data key)；预留节点 = `EffMax × 行模板 cell 数`
  计入 64 上限（同 v1 list）；行内 action 仅 report/set/close。
- 树深恒 2：任何叶子的属性里出现 `children` → 拒绝。

叶子（保留 v1 校验，逐条不变）：
- `bind` 路径必须已注册（`DataHub::Has`），附 `HintFor` 提示；`media.*` 前缀单独给「播控 UI 内置、
  勿画播放器卡」话术（invoke 的 `media.*` 命令同口径），防弱模型换名连环重试。
- label `fmt` 与 bind 类型相容（`FmtSafeForType`，数值路径 `%s` → 拒绝，防真机 newlib 崩溃）。
- 数值控件 slider/arc/bar/switch/choice 不得 bind String 路径。
- qrcode text 非空 ≤256 字节；choice options 2-6 字符串；chart bind_history 须有历史；
  stock_chart 走 `pi_card_stock::ValidateNode`。
- action 合法性（in-list-row 拒 toggle/patch/show/hide）。

限额：≤64 节点（bind_rows 按 max×行 cell 数预留）、grid 数 ≤8。深度检查退化为「root 是数组、
grid 无 children」两条即可。超节点上限时 Validate 先整卡预统计（与逐叶校验同口径），错误里带
「实际声明 N 节点、至少砍 N−64」的具体数字——弱模型拿不到数字只会盲目微调连环重试。

### 6.2 自动 repair 规则（模型常见错误，能修则修不重试）

| 模型错误 | 自动修 | 还是拒绝重试 |
|---|---|---|
| grid 里写了 `justify/align/grow/gap/pad/w/h/span/cols(fr数字)/size/color/bg` | **静默剥除**（前向兼容，Lint 提示） | 修 |
| `root` 写成单 grid 对象（非数组） | **包成 `[单grid]`** | 修 |
| cells 里塞了 `{type:"column"/"row"}` 容器 | 尝试**扁平化**：把其 children 提到父 grid 的 cells（仅一层） | 修（提示）；两层以上 → 拒绝 |
| `cols` 长度 ≠ 行 cell 数 | 若 cols 更短，**按行最大 cell 数补空列**；更长则截断 | 修（提示） |
| 数值列忘标 `num` | solver 按数值列门槛（mono/数值 bind/fmt 数值转换/静态数值文本，§2.2）**自动推断** | 修（无需模型标） |
| 旧 `list` 节点 | **改写为 `bind_rows`**（list.bind_data→bind_rows，list.item 原样） | 修 |
| 旧 `spacer` 节点 | **删除**，若其后有 cell 则给该 cell 补 `side:"end"`（仅当 spacer 在两 cell 之间） | 修（启发式，提示） |
| grid 块级挂 `on_click/on_change/on_release` | **剥除 + note**（渲染器只认叶子事件，静默忽略=交互悄悄丢失） | 修（提示挂到叶子上） |
| `rows` 写成一维叶子数组（忘二维） | **裸叶子包成单格行 + note**；schema 侧放开 rows 内层约束（pi-c 只查内联约束不查 `$defs` 内部，内联 `type:"array"` 会硬拒） | 修（提示 rows 是 2-D） |
| `preset`/`slots` 键 | 顶层出现 → **拒绝**，回错误串「preset 已移除，请直接给 root grid 数组，示例见 system prompt」 | 拒绝 |
| bind 路径不存在 / fmt 类型不符 / 数值控件绑 string / 嵌套 grid / choice<2 / qr>256 | **拒绝重试**（正确性/安全底线，不能猜） | 拒绝 |

> repair 的哲学：**布局/语法糖类错误一律吞掉自愈**（这些正是 v1 让模型头疼、fix 频发的地方，v2 干脆
> 不让它们成为错误）；**正确性/安全类错误（崩溃、读垃圾、越界、语义歧义）一律同步拒绝回给 LLM 重试**
> （沿用 v1 已验证的防线）。

---

## 7. 测试计划

### 7.1 TDD 不变量清单

**前缀不变量（流式）**
- P1 任意切分点等价：对语料每张卡的 args JSON 串，在**每个字节边界**切分，依次喂 partial → 最终
  DOM ≡ 一次性 `Solve` 输出。（v2 grid 整块重建使这条近乎结构保证，仍须测）。
- P2 幂等重渲：同一意图 `Solve` 两次，输出 layout cJSON 逐字段相等。
- P3 迟到 data：先喂无 data 的前缀、后喂带 data 帧 → 与一次性带 data 等价。

**solver 不变量（纯单测，宿主）**
- S1 不重叠：同一行内 cell 的 `[x, x+w)` 两两不交。
- S2 不越界：每行 `Σ(w) + Σgap ≤ viewport_w`；SPAN_ALL cell 的 `w == viewport_w`。
- S3 列宽和：`rows` 形态 `Σtrack_w + gap×(ncol-1) == 已用宽 ≤ viewport_w`。
- S4 触控最小：slider.w ≥ 160；交互 cell 所在行高 ≥ 44；choice seg 宽 ≥ 44。
- S5 同组等径：同一 grid 内所有 arc 的 w 相等。
- S6 数值列：`num` 列 cell align==end && truncate==false && mono；文本列可 truncate。
- S7 单不可拉居中：**仅当 cell 的 stretch==false** 且单个独占行 → `x == (viewport_w - w)/2`；
  单个可拉伸 label 独占行时吸满整行、左对齐（`w==viewport_w, x==0`），不居中。
- S8 折行确定性：给定 measure stub，`cells` 折行结果与 golden 逐 cell 相等。

**负例语料（必须拒绝，不得渲染半张）**
min>max slider(渲染兜底放行但 Lint)、choice<2/>6、qrcode>256、unknown bind、数值控件绑 string、
嵌套 grid/容器、fmt `%s` 绑数值、root 非数组且不可修、节点>64、grid>8。

### 7.2 测试分层

1. **solver 纯单测**（宿主，无 LVGL）：`sim/solver_test`（新增 CMake target）。喂意图 JSON + 等宽
   measure stub → 断言 S1-S8 + golden layout 文件比对。这是主战场，覆盖所有几何决策。
2. **流式前缀测试**（宿主）：`sim/preview_prefix_test`——语料每卡逐字节切分喂 `Solve`，断言 P1-P3。
3. **headless 截图冒烟**（sim）：迁移后的 26→~24 张卡 + 4 archetype，`PI_SIM_CARD_MS`/previewscene
   命令跑真 LVGL 渲染 + F12 截图，人工/像素基线核验最终观感（sim 与真机因定像素轨道应逐像素一致）。

---

## 8. 实施切分建议（依赖顺序 + 每步验收）

> 严格 TDD：先写测试基建和 solver 单测，再实现，最后换渲染器/prompt。每步可独立 build + 验收。

**步骤 1 —— 测试基建**〔已完成〕
- 建 `sim/solver_test` + `sim/preview_prefix_test` CMake target（链 cJSON，不链 LVGL）；写 measure
  stub（ASCII=8px、CJK=16px×scale）；准备 golden 目录 + 4 archetype 意图 JSON。
- 验收：空 solver 桩下测试框架能跑、能比对 golden（先全 xfail）。

**步骤 2 —— solver（纯函数）**〔已完成〕
- 实现契约表(§2.2) + cells 折行(§2.3) + rows 分宽(§2.4) + fmt 代表串 + `Solve` API(§2.6)。
- 验收：S1-S8 全绿、4 archetype golden 通过、负例折行不 panic；宿主 ASan 干净。

**步骤 3 —— 渲染器（哑翻译器）**〔已完成〕
- 删 render.cc 的 column/row/grid/spacer/list 分派 + 六层 FLOW_* 推断 + ApplySizing 几何推断；
  新增翻译层（`cells`→绝对定位 `set_pos/set_size`；`rows`/`bind_rows`→定像素 grid dsc + `set_grid_cell`
  + 设 truncate，见 §2.0）；保留全部叶子 builder
  （label/button/slider/arc/switch/bar/icon/divider/qrcode/choice/chart/stock_chart）+ ApplyBind/
  num-anim（机制不变，挂点从 row/list 改到 grid/bind_rows）；bind_rows 数据更新走整 grid 重建
  单一路径（无行级 fast path，见 §1.4）。
- 验收：4 archetype 在 sim 截图逐像素符合预期；assert-style 回归（v1 kCardStyleFam 等价卡）过。
  〔真机烧录烟测：待验〕

**步骤 4 —— 预览 v2**〔已完成〕
- 删 `SyncPreviewNode`/`PreviewSyncContainer`/`s_leaf_sig`/USER_2/`RefreshPreviewDataLabels`；
  实现 §4 的扁平 `s_grid_sig[]` 整块签名生长。
- 验收：P1-P3 前缀测试全绿；previewscene 命令跑 4 archetype 流式截图与一次性一致。

**步骤 5 —— prompt / preset / 语料**〔已完成〕
- 删 `ExpandPreset` + 4 个 `Build*` + slots 校验 + schema 里 preset/slots/旧布局属性；换 §1 的 v2
  schema（`PI_CARD_RENDER_SCHEMA`）；换 §6 的 Validate/repair；写 §10 新 system prompt + ui_render
  DESC；迁移 sim 26 卡（删 kCardJustify/kCardAlign，补 2 张 v2 验收卡）。
- 验收：system prompt ≤4KB（实测 4076B）；sim 全语料视觉冒烟过。
  〔真机语音端到端「画个控制卡/表格/菜单」观感：待验〕

---

## 9. 风险清单（规格盲点 / 实现雷区）

- **R1（top1）solver 测量保真 + 数值列宽预测** —— sim 的 measure stub 与真机 `lv_txt_get_size`
  的中文宽度/字距若不一致，折行点和列宽会 sim/真机分叉（本仓有先例：sim-vs-device-sprintf、
  中文测量踩过坑）。数值列用 fmt 代表串预测宽度是启发式，`%s`/未知 fmt 兜底不当会重现 v1「值列
  塌陷」或「过宽挤兄弟」。**缓解**：真机 measure 用真字体；数值列宽只信 fmt 代表串不信 live text；
  为代表串规则单独建 golden。**这是最可能返工的一处**（见末尾）。
- **R2 LVGL 定像素轨道行为** —— 改用定像素 grid dsc（不用 LV_GRID_FR/CONTENT）后，需确认 LVGL9 对
  「track 宽 = 固定像素 + cell 内 label LONG_DOT/WRAP」的行高自适应正确（宽定、行高随折行）。cell
  内中文折行行高是唯一留给 LVGL 的自由度，solver 的 h_hint 只能 advisory；overlay 权威高度靠
  ReflowOverlay 实测（已保留）。若某 cell 实际折行行高超预期 → 观感偏差但不崩。
- **R3 中文测量与 SafeFont 回退** —— mono 字体仅 ASCII，含中文回退 puhui 会改变宽度；solver 测量
  时必须用 SafeFont 回退后的**实际字体**测，否则会导致宽度预测错位（v1 有此类护栏）。
- **R4 choice 满宽 vs cells 折行冲突** —— §2.4 已定「cells 里 SPAN_ALL、rows 多列里 FILL」，但要
  测「cells 里 choice 前后有 inline cell」——choice 必须打断折行 run、独占行，前后 cell 各自成行。
- **R5 删 size 的表达力损失** —— 删 `size` 后无法做 hero 大图标 / 特大二维码；若产品需要，需回补
  一个受控档位（如 icon role 或 qrcode 尺寸档），而非放开像素。规格暂删，风险登记。
- **R6 bind_rows 节点预留与 64 上限** —— 动态行按 `max×行cell数` 预留，模型给大 max + 宽行模板易
  撞 64 上限；repair 无法自动降 max（改语义），只能拒绝提示。
- **R7 side:"end" 语义边界** —— cells 里多个 `side:"end"` 或 end 与均分共存时的落位需在 §2.3 明确
  单测（当前定义：end cell 右靠，其余左靠，中间留剩余）。

---

## 10. 新 system prompt 草稿（替换 host.cc:1452-1507 的 DESIGN/COMPACT/LAYOUT 段）

> 目的注解（中文，不进正文）：删掉 v1 的 DESIGN/COMPACT/LAYOUT 三段布局教学（它们教的是 row/grow/
> justify，v2 已无这些）。ACTION ECONOMICS / UPDATE vs RE-RENDER / WRITABLE / HOME WIDGET /
> DEVICE COMMANDS 五段**保留引用**（本草稿不含，拼接时接在其后）。目标 ≤4KB，风格贴近 v1（英文正文 +
> 语义 token）。以下是替换段正文：

```
CARDS — you have a 720×720 SCREEN. Draw real interactive UI with ui_render instead of describing.
A card = "root": an ARRAY of grid blocks stacked top-to-bottom. Only ONE container: grid. No
column/row/nesting. Depth is fixed: card → grid → leaf. You never write x/y, width, gaps, columns,
grow, justify or align — the device lays it out from your intent. Just say WHAT controls exist.

GRID has three forms (put exactly one key):
- "cells":[leaf,…] — a flow of leaves; the device wraps them by size. Use for a header, a control
  row (icon+slider+value), a button group. Full-width leaves (divider/chart/choice/qrcode) take
  their own line automatically.
- "rows":[[leaf,…],…] with optional "cols":[{"title","num"}] — an aligned TABLE: every row shares
  the same columns. Mark number columns "num":true (right-aligned mono). One cell per row = a
  vertical list/menu. Use for key/value status, forms, dashboards, tables.
- "item":[leaf,…],"bind_rows":"key","max"?,"empty"? — repeat the row template once per element of
  data["key"]; inside strings use {i}/{n}/{item.FIELD}. Row taps: report/set/close only.

LEAVES: label{text,role,bind,fmt,mono,tone} · button{text,icon,variant,on_click} · slider/arc/bar/
switch/choice{bind/value/options,on_change} · icon · divider · qrcode{text} · chart{bind_history} ·
stock_chart{symbol}. role ramp: eyebrow|kicker|section|title|heading|label|value|caption. A header
= a cells grid with role:eyebrow then role:title. A big number = a mono label (role:value); number
cells (mono / number-bound / numeric fmt) auto right-align in tables, plain text left-aligns.
Segment/switch/slider carry state — don't restate it in words.

STYLE: lean on pi's look. tone/fill are semantic tokens (accent/ok/err/tx/dim/faint/card2/line…),
never hex. Exactly ONE primary(amber) button per card; the rest ghost/plain/default. Put a cell to
the right edge with "side":"end". Give a grid a background box with "fill":"card2". Keep labels
1-3 words; number columns don't truncate, text columns do.

CHOOSE: SET something → a control grid that binds the path (writes hardware directly). STATUS → a
rows table binding the paths. CHOICE/CONFIRM/FORM/MENU → render it, the tap rides back on report.
Chit-chat → just text. Prefer display:"chat"; "overlay" only for a modal moment (auto-closes).
```

（拼接：以上 + `WRITABLE device paths…` + `HOME WIDGET…` + `DEVICE COMMANDS…` + 一个 §1.5① 精简示例。
`BuildPathsClause(false)` / `BuildCommandsClause` 保持不变复用。）

**实测数据（步骤 5 收官）**：v2 system prompt 教学段实测 **4076B（≤4096 达标）**；ui_render 工具
DESC 实测 **5134B**；两者合计 **9210B（≤9216 软预算达标，余量仅 6 字节）**。余量极薄，标注为
**后续观察项**——路径/命令清单（`BuildPathsClause`/`BuildCommandsClause`，随注册表运行时增长）或
教学段再增一句都可能顶破 9216；后续如需扩写，优先精简 DESC 腾余量，勿动教学段的形态/门槛表述。

---

## 11. 硬约束（写进规格，实现须遵守）

- 真机 newlib-nano：格式串禁 `%zu/%lld/%llu`；`CONFIG_LV_USE_CLIB_SPRINTF=y` 使 `lv_snprintf`/
  `lv_label_set_text_fmt` 同样落 nano vsnprintf。solver/渲染器一律 `%u/%d` + 显式强转。
- 代码中文字符串直接写中文，不用 `\x` 转义（icon_map PUA 码点等既有例外除外）。
- 节点上限维持 64；新增 grid 数上限（建议 8）；bind_rows 按 max×行 cell 预留计入 64。
- solver 必须可在 macOS 宿主单独编译：**零 LVGL、零 ESP 头文件依赖**，几何 = 纯整数运算 + 注入的
  measure 回调。
- 保留 `.clang-format` 手动保格式（本仓 clang-format 配置无法加载，列宽实际 120）。
