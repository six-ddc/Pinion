# AI → UI：pi_card 声明式卡片系统设计文档

> 本文是 Metalio Claw（ESP32-P4，720×720 触摸屏）固件中 "AI to UI" 子系统的完整设计说明：
> LLM 通过工具调用（`ui_render` / `ui_update` / `ui_close`）用一份 JSON spec 在聊天流里渲染
> **真实可交互的 LVGL 卡片**，卡片控件可双向绑定设备硬件数据（DataHub），并带一套零回程的
> 本地动作引擎。代码位于 `main/display/screen/pi_screen/pi_card/`（UI 层）与
> `main/stock/`（股票数据层）。文中 `file:line` 均相对 `main/display/screen/pi_screen/`
> 与 `main/stock/`，行号对应写作时的工作树。

---

## 目录

1. [设计理念](#1-设计理念)
2. [整体架构与数据流](#2-整体架构与数据流)
3. [LLM 工具面：四个工具](#3-llm-工具面四个工具)
4. [提示词设计](#4-提示词设计)
5. [渲染层：控件集与布局](#5-渲染层控件集与布局)
6. [数据面：DataHub 与绑定/刷新](#6-数据面datahub-与绑定刷新)
7. [卡片数据模型（data / list / bind_data）](#7-卡片数据模型data--list--bind_data)
8. [动作引擎与 invoke 命令](#8-动作引擎与-invoke-命令)
9. [Preset：四种预置卡片](#9-preset四种预置卡片)
10. [卡片生命周期：chat / overlay / standby](#10-卡片生命周期chat--overlay--standby)
11. [校验、Lint 与错误回传](#11-校验lint-与错误回传)
12. [线程模型与代次过滤](#12-线程模型与代次过滤)
13. [股票子系统](#13-股票子系统)
14. [限额速查表与健壮性设计汇总](#14-限额速查表与健壮性设计汇总)

---

## 1. 设计理念

整套系统围绕六条原则设计，理解了这六条，其余细节都是推论：

1. **卡片即读取（render-as-read）**。模型没有任何"读设备"工具——TOOLS[] 里只有三个只写的
   `ui_*` 和 `stock`。渲染一张绑定了设备路径的卡片，就是读设备的唯一途径：`ui_render` 的返回值
   附带 `state`（每个绑定路径的当前值快照）。这带来两个好处：读取零额外往返；且"模型读到的"
   永远等于"用户屏上看到的"，不存在模型知道但屏幕不显示的暗数据。

2. **声明式 spec，双跑校验**。LLM 下发的是一棵 JSON 控件树，不是命令序列。同一套约束跑两遍：
   worker 线程先干跑 `Validate()`（不碰 LVGL，错误**同步**返回给 LLM 让它立即修正重试），
   校验通过后才入队，由 LVGL 线程真正建控件。契约是"校验过 → 一定能建"
   （`pi_card_render.h:11-14`）。

3. **动作经济学（action economics）**。每次点击先问：设备自己能完成吗？能 → 本地 action
   （close/set/toggle/show/hide/patch/invoke），瞬时、零往返、聊天流不可见；不能（要生成新内容、
   做新决策）→ `report`，代价是一整轮 LLM 往返 + 一条假用户气泡 + token。系统从 schema、
   提示词到 Lint 都在推着模型选便宜的那条路。

4. **单一真相（single source of truth）**。可绑路径清单、可用命令清单都在运行时由活体注册表
   （`DataHub::ListPaths()`、`CommandRegistry`）生成，动态拼进工具描述和 system prompt——
   全固件不存在第二份硬编码清单，注册即生效、删除即消失，描述永远不会过时。

5. **红线靠"不给能力"而非"劝模型别用"**。会静默重启设备的 `net.type` 干脆不注册 setter；
   `power.off`、`factory.reset` 干脆不进命令表；危险命令（切网络、开 GPS、清历史）注册为
   Confirm 级——弹**固件层**确认 sheet，模型无法绕过。安全性来自能力面裁剪，不依赖提示词服从。

6. **worker 不碰 LVGL，LVGL 不碰网络**。agent worker 线程只做校验+入队+读快照；建控件、
   改控件、删控件全部发生在 LVGL 线程的 80ms drain tick 里；股票抓取又在独立 worker 线程，
   结果经队列回 LVGL。三方之间只有队列和原子代次，没有共享锁地狱。

## 2. 整体架构与数据流

```
┌─────────────┐  tool call (JSON args)   ┌──────────────────────────────┐
│  DeepSeek    │ ────────────────────────→ │ agent worker 线程 (pi-c)     │
│  (pi-c 运行时)│ ←──────────────────────── │  pi_card_tool_render/update/ │
└─────────────┘  同步返回 {card,state,    │  close：ExpandPreset →       │
                  hints} 或错误串（重试）  │  Validate → Enqueue          │
                                          └──────────────┬───────────────┘
                                                         │ pi_ui_queue()（深 32，
                                                         │ 事件带 run 代次）
                                          ┌──────────────▼───────────────┐
                                          │ LVGL 线程 DrainQueueTick(80ms)│
                                          │  代次过滤 → OnRenderEvent /   │
                                          │  OnUpdateEvent / OnCloseEvent │
                                          │  → RenderNode 建控件树        │
                                          └───────┬──────────────┬───────┘
                              双向 bind / observer │              │ ReportAsyncError →
                                          ┌───────▼──────┐       │ pi_agent_task_inject
                                          │   DataHub    │       ▼ （异步错误/report
                                          │ 37 静态路径 + │      回注入 LLM 对话）
                                          │ stock.* 动态  │
                                          └───────┬──────┘
                                     mhal getter/setter（1Hz poll）
                                          ┌───────▼──────┐   ┌───────────────┐
                                          │  硬件 (mhal)  │   │ stock worker   │→ 腾讯行情
                                          └──────────────┘   └───────────────┘
```

分层与文件对应：

| 层 | 文件 | 职责 |
|---|---|---|
| 工具桥 | `pi_card/pi_card_tools.h` + `pi_agent_task.c` | 工具注册、schema/描述常量、C↔C++ 桥 |
| 宿主 | `pi_card/pi_card_host.{cc,h}` | 校验入口、preset 展开、卡片注册表、生命周期、state/hints、动态描述与 system prompt 生成 |
| 渲染 | `pi_card/pi_card_render.{cc,h}` + `pi_card_icons.{cc,h}` | JSON 树 → LVGL 控件树、Validate/Lint、样式 |
| 数据面 | `pi_card/pi_card_data.{cc,h}` + `pi_screen.cc` 注册段 | DataHub：路径注册、subject、poll、history、动态 provider |
| 动作 | `pi_card/pi_card_actions.{cc,h}` + `pi_card_cmd.{cc,h}` | 8 种 action、report 节流注入、invoke 命令注册表 |
| 股票 | `pi_card/pi_card_stock.{cc,h}` + `stock_chart_renderer.{cc,h}` + `main/stock/*` | stock 工具、stock_chart 控件、腾讯数据源、抓取调度 |

一句话核心链路：LLM 调 `ui_*` → worker 线程同步校验（preset 展开 + Validate + 动作校验 +
pin 尺寸）→ 打上 run 代次入队 → 秒回 `{card,state,hints}` → LVGL 线程 80ms drain 按代次
过滤后建控件 → root 挂 `LV_EVENT_DELETE`，`OnRootDeleted` 是唯一清理通道。同步错误走工具
返回值，drain 阶段的异步失败走 `ReportAsyncError` → 注入回 LLM。

---

## 3. LLM 工具面：四个工具

`pi_agent_task.c:416-441` 的静态 `TOOLS[]` 注册四个工具：`ui_render`、`ui_update`、
`ui_close`、`stock`。工具名不能含点号（DeepSeek/OpenAI `function.name` 约束
`^[a-zA-Z0-9_-]+$`），故用下划线。所有工具的 `execute` 都跑在 **agent worker 线程**
（SSE 读循环的栈上），契约：不碰 LVGL、毫秒级返回。

### 3.1 ui_render —— 渲染一张卡

参数**就是**卡片 spec：

```
{ display?: 'chat'|'overlay'|'standby',   // 默认 chat
  ttl_ms?:  int,                          // overlay 自动关闭时限
  card?:    'id',                         // 显式卡片 id（可选）
  data?:    { key: scalar|array },        // 卡级数据（供 list/bind_data）
  preset?:  'confirm'|'form'|'dashboard'|'menu',
  slots?:   {…},                          // 与 preset 配对
  root:     <node> }                      // 给 root，或给 preset+slots
```

节点（`node`）是递归结构：`{type, children?, text?, bind?, on_click?, …}`，完整属性见
§5。JSON-Schema 在 `pi_card_tools.h:99-133`（`PI_CARD_RENDER_SCHEMA`）：`$defs` 里定义
`action` 和自引用的 `node`，类型枚举 16 种，事件是 action 数组。

**返回值**（`pi_card_host.cc:1098`）：`{"card":"<id>","state":{<path>:<value>},"hints":[…]}`

- `card`：分配的卡片 id（自动分配形如 `c.1`，"c." 前缀刻意与 LLM 惯用的 `c1/card1` 隔开防撞车，
  `host.cc:52-54`；`standby` 强制 `pin`）。
- `state`：本卡**所有 bind 路径的当前值快照**（`BindStateJson`，`host.cc:690-720`）——这就是
  "渲染即读取"。worker 侧经 `ReadForWorker` 读，bool 输出 0/1 与 switch 口径一致；快照发生在
  渲染**之前**（卡还没建，读的是数据面）。
- `hints`：非阻塞的设计建议（Lint 产物，见 §11.3），如"两个 primary 按钮，请只留一个"。

校验失败时返回错误串并置 `is_error`，LLM 收到后修正重试——错误文案都写成"可执行的修复指引"
（如列出全部可用路径/命令）。

### 3.2 ui_update —— 改一个节点或改卡片数据

描述见 `pi_card_tools.h:135-143`。两种互斥形态（`host.cc:1114-1122`）：

- **节点 patch**：`{card?, id, props:{text?,value?,checked?,hidden?,tone?,color?}}` ——
  改渲染时声明了 `id` 的节点的六种属性。
- **数据操作**：`{card?, data:{set?:{k:v}, append?:{key,item}, remove?:{key,index|id},
  replace?:{key,index,item}}}` —— 改卡级 data，绑定该 key 的 list / bind_data label 自动重渲。

`card` 省略即"最近一张卡"。语义要点：能用 ui_update 就不要重新 ui_render——system prompt
明确"只有结构不同的卡才重渲"。如果目标卡/节点已消失（被关、TTL 到期、新会话清空），失败
**异步**报回 LLM（此调用本身已返回 ok），见 §11.2。

### 3.3 ui_close —— 关一张卡

`{card?:'' (latest)}`（`pi_card_tools.h:158-160`）。无前置校验；卡不存在时静默忽略
（`host.cc:601-611`）——关一张已经没了的卡不算错。`card:'pin'` 是移除待机屏挂件的正道
（先擦 NVS 再删控件）。

### 3.4 stock —— 行情快照查询

详见 §13.1。给 `query`（中文名/拼音/代码搜索）或 `symbols`（直接批量报价），返回
`{quotes:[{sym,name,price,chg,pct,…}]}` 数字快照。描述里明确引导后续动作：
"To SHOW a live chart card, follow with ui_render {root:{type:'stock_chart',symbol,name}}"
——工具之间用描述互相接力。

### 3.5 工具描述的运行时动态生成

`ui_render` 的 description 不是编译期常量，而是启动时由 `pi_card_render_desc()`
（`host.cc:1193-1202`）拼出，缓存进 function-static `std::string`（指针常驻整个固件运行期，
满足 pi-c 浅拷贝工具结构体、借用 description 指针的契约）：

```
PI_CARD_DESC_HEAD                    // 编译期骨架：spec 形状、16 控件类型、role/variant、
                                     // 通用属性、tone/fill 语义 token（tools.h:51-72）
+ BuildPathsClause(true)             // ← 运行时：DataHub 活体路径清单（见下）
+ PI_CARD_DESC_TAIL                  // 编译期骨架：fmt 安全规则、事件/action 模型、data 操作、
                                     // preset、图标、限额、表格布局技巧、内联示例（tools.h:75-97）
+ " COMMANDS (invoke cmd): " + BuildCommandsClause(true)   // ← 运行时：命令注册表清单
+ STANDBY 段                          // pin 挂件说明
```

骨架拆 HEAD/TAIL 两段，是因为路径清单必须插在"双向 bind 目标"与"事件/图标/示例"两段说明
之间（`tools.h:47-50`）。

**BuildPathsClause**（`host.cc:1156-1188`）遍历 `DataHub::ListPaths()` 分三桶：

- 可写 Int 带量程 → `WRITABLE slider/arc (range overrides your min/max): audio.volume(0-100), …`
- 可写 Bool → `WRITABLE switch (0/1): ui.theme, speech.tts`
- 只读 → `READ-ONLY (dimmed; label+fmt shows it live): battery.level(int 0-100), net.ssid(str), …`

`full=true` 给工具描述用（完整措辞 + 追加 `pi_card_stock::BindPathsDesc()` 动态股票路径说明）；
`full=false` 给 system prompt 用（只列可写路径一句话）。**两处共用同一个生成器，全固件不存在
第三份硬编码路径清单**（`host.cc:1151-1155`）。

启动时有 8KB 预算软告警（`pi_agent_task.c:781-796`）：system prompt + render desc 总长
超 9216 字节打 WARN（不 fail），防止路径/命令清单膨胀悄悄吃掉上下文。

---

## 4. 提示词设计

### 4.1 system prompt（`pi_card_host.cc:1207-1268`）

与工具描述同款 function-static 缓存 + 常驻指针契约（pi-c 在 `create_agent` 里深拷贝
system_prompt，`pi_agent.c:58-60`，故 new_session 重建 agent 再借同一指针安全）。全文英文
（省 token、指令遵循更稳），共 12 段，每段解决一个明确的行为问题：

| 段 | 内容 | 解决的问题 |
|---|---|---|
| 1 身份 | "You are pi, the on-device assistant living in a palm-size 720×720 touch screen… Reply short, warm, in the user's language (usually Chinese). Your text is read aloud by TTS, so avoid markdown symbols, links, and long bullet lists in prose." | 语音场景：回复要能读出来 |
| 2 屏幕能力 | "You have a SCREEN… There is NO read tool: rendering a card that binds a device path IS how you read the device, and ui_render's return value gives you the live values (state)." | 教会"渲染即读取"这个非常规心智模型 |
| 3 何时用卡 | SET → 控制卡（slider 直写硬件零往返）；STATUS → 小 dashboard；CHOICE/CONFIRM/FORM/LIST → 渲染出来让点击经 report 回来；闲聊 → 纯文本。"Prefer 'chat'; 'overlay' only for a modal moment — auto-closes and capped." | 防止过度渲染和 overlay 滥用 |
| 4 DESIGN | 靠 pi 既有观感别手调样式：header = eyebrow+title；**恰好一个** primary（琥珀）按钮——主题已把 slider 填充/开态 switch/选中 choice 涂琥珀，别再把文字也涂琥珀；用语义 tone/fill token 不用裸 hex | 视觉一致性 |
| 5 COMPACT | 窗口小、密胜于高：数据摆成多列表格行（每条记录一行、每字段一个 label、**每个 label 都 grow:1** 才跨行对齐），每行 2-4 列；短事实绝不一行一条；长文不进卡片；label 1-3 词；list 用 max 夹紧 | 720px 宽度下的信息密度 |
| 6 ACTION ECONOMICS | 设备自己能完成 → LOCAL action（瞬时、零往返、聊天不可见）；只有要生成新内容/新决策才 REPORT（一整轮往返 + 显示成用户消息）；绝不为浏览/展开/回显值而 report，用 toggle/patch；report 自动携带每个带 id 控件的值（choice 是 idx(label)），所以给控件 id 而不是把值写进 text | 省 token 省往返 |
| 7 UPDATE vs RE-RENDER | 改一个节点的 text/value/显隐或卡数据 → ui_update；只有结构不同才重新 ui_render | 防重复渲染 |
| 8 可写路径 | `"WRITABLE device paths you can set: " + BuildPathsClause(false)` | 活体清单，运行时拼入 |
| 9 HOME WIDGET | display:'standby' 钉一张卡到待机屏（跨新会话/重启存活）；里面优先本地 action/invoke——从挂件发 report 会让模型在**后台**跑，要罕用；移除用 ui_close card:'pin' | 常驻挂件的特殊语义 |
| 10 DEVICE COMMANDS | `{do:'invoke',cmd}` 做数据路径做不到的真动作（重连、切网、新会话）；confirm 级会弹**固件确认**，模型无法绕过；`"Available: " + BuildCommandsClause(false)` | 命令能力面 + 活体清单 |
| 11 PRESETS | 四种 preset 的 slot 结构一览（confirm/form/dashboard/menu），"they expand to a normal card and validate the same" | 常见形状的省心通道 |
| 12 Examples | 三个内联 JSON 完整示例：音量控制（chat）、确认弹层（overlay+preset，中文文案）、数据驱动列表（data+list+{item.t}） | few-shot 定形 |

### 4.2 提示词工程的几个刻意取舍

- **描述与 prompt 分工**：ui_render 的工具描述（~几 KB）承载"怎么写 spec"的全部语法细节
  （类型、属性、fmt 规则、事件、限额、示例）；system prompt 承载"什么时候画卡、怎么设计得好"
  的策略。语法查描述、策略进人设，各查各的。
- **活体清单三处复用**：`BuildPathsClause`（路径）与 `BuildCommandsClause`（命令）同时喂
  工具描述（full 版）和 system prompt（精简版），保证模型看到的能力面 = 设备真实能力面。
- **错误即文档**：校验错误串都带"下一步怎么改"（列出可用路径/命令/正确用法），把重试轮变成
  在线学习轮。
- **fmt 崩溃防线写进描述**：`PI_CARD_DESC_TAIL` 开头就是 "A bound label's fmt MUST match
  the bound type and hold ONE placeholder… (a %s on a number path is rejected — it would
  crash)"——把最危险的 UB（`%s` 套 int 路径 → newlib `strlen(0)` 崩机）在提示词、校验器、
  渲染器三层都设防。
- **预算意识**：prompt+desc 合计软上限 9216B（`pi_agent_task.c:781-796`），新增路径/命令时
  启动日志会告警。

---

## 5. 渲染层：控件集与布局

入口 `RenderNode(parent, node, card, limits, depth, node_count, err, parent_flow, in_list_row)`
（`pi_card_render.cc:627`）：递归把 JSON 树建成 LVGL 控件树；任何一步失败向上冒泡，宿主删掉
root **整卡回滚**（不留半张卡）。每个节点统一流程：建对象 → 默认样式 → 通用属性 → 尺寸 →
bind → data-label → 事件 → 死控件降级 → 递归 children（仅 column/row）。

限额：**64 节点 / 8 层深**（`render.cc:634,638`；`RenderLimits` 定义在 `pi_card_host.h:35-38`）。
未知 type 整卡失败；未知字段静默忽略（前向兼容）。

### 5.1 十六种控件

**容器**

- `column` / `row {children:[], gap?}`：flex 容器，gap 默认 12。row 用 ROW_WRAP（放不下自动
  换行不裁切）并 flex_align(START,CENTER)。**depth==0 的顶层 column/row 自动获得卡片外观**：
  Card 底色 + radius 18 + Line 边框 + pad_all 24（`render.cc:308-315`）——LLM 不需要也不能
  自己画卡壳。

**文本与展示**

- `label {text?, role?, bind?, fmt?, bind_data?}`：WRAP 长文本。列内默认全宽，行内自然宽。
  role 字号阶梯见 §5.2。`bind` 绑设备路径实时显示（配 `fmt`）；`bind_data` 绑卡级 data key
  （text 里可用 `{value}` 内联）。bind 与 bind_data 同给时 bind 优先。
- `icon {icon:'name', size?}`：无图标字体，全部用 LVGL 图元（条/圆/环/弧）拼出，size 默认 22
  像素，tone 默认 dim。名称清单（含别名，`pi_card_icons.h:11-16`）：check/ok、close/x、
  plus/add、minus、chevron/arrow/next、dot（未知名回落）、gear/settings、info、warning/alert、
  battery、charging/bolt（琥珀闪电）、wifi/signal、cellular、sun/brightness、volume /
  volume_high / volume_low / mute（红斜杠）/ music / mic、clock。
- `divider`：1px 全宽 Line 色横线。
- `spacer`：行内 flex_grow=1 顶开两侧；列内**绝不 grow**（否则撑高卡片），默认高 8。
- `qrcode {text, size?}`：size 默认 160 钳 [96,320]；text 非空且 ≤256 字节。配色**固定**
  浅底深码不随主题反色——保可扫描性（`render.cc:713-715`）。

**输入控件**

- `button {text, variant?, on_click}`：variant 见 §5.3。行内自动 grow。
- `slider {min, max, value, bind?, id?, on_change?, on_release?}`：min/max 默认 0/100
  （max≤min 时自动 +1 兜底）。高 6px 轨道，琥珀填充，**中性色把手**（把手不抢琥珀），
  ext_click_area 20 扩大触区。绑可写路径→直写硬件；绑定路径有量程时**量程覆盖 JSON min/max**
  （`render.cc:491-494`）。
- `arc {…同 slider}`：圆形旋钮，固定 132×132，270° 扇形（rotation 135）。
- `switch {checked, bind?, id?, on_change?}`：开态琥珀。行内不 grow（保持自然宽）。
- `bar {min, max, value, bind?}`：只读进度条，无回写（观察者手动同步值）。
- `choice {options:[2-6], value?, id?, bind?, on_change?}`：分段选择器（segmented picker），
  2-6 个选项，选中段琥珀底。内部按钮**整体只计 1 个节点**。value 是选中下标；report 时报
  `idx(label)` 双份。

**数据驱动**

- `list {bind_data:'key', item:<node>, max?, empty?}`：按 data[key] 数组每元素克隆一次 item
  模板渲染，见 §7。
- `chart {bind_history:'path', points?, w?, h?}`：history 路径的折线图，见 §6.4。
- `stock_chart {symbol, name?, mode?}`：自刷新行情卡，见 §13.2。

### 5.2 label 的 role 字号阶梯（`render.cc:164-184`）

| role | 字体 | 默认色 | 字距 | 用途 |
|---|---|---|---|---|
| eyebrow | pi_mono_14 | Faint | +2 | 卡片眉题（如 "CONFIRM"） |
| kicker | pi_mono_14 | Accent | +2 | 强调眉题 |
| section | pi_mono_14 | Dim | +2 | 分组小标题 |
| title | puhui_30_4 | Tx | 0 | 卡片主标题 |
| heading | puhui_24_4 | Tx | 0 | 次级标题 |
| label | puhui_20_4 | Dim | 0 | 字段名 |
| value | pi_mono_20 | Tx | 0 | 数值（等宽） |
| caption | pi_mono_14 | Faint | 0 | 脚注 |

无 role 时按 `size`（默认 20）+`mono`（默认 false）选字体档位。**SafeFont 护栏**
（`render.cc:533-539`）：pi_mono 系列只有 ASCII 字集，文本含任何非 ASCII 字节时无条件回退
puhui_20_4 并去字距——中文进等宽字体不会变豆腐块。文本颜色优先级：tone（语义 token）>
color（#hex）> role 默认。

### 5.3 button variant（`render.cc:188-216`）

共同：radius 12、pad 20×15、无阴影。

| variant | 底色 | 按压 | 字色 | 边框 | 定位 |
|---|---|---|---|---|---|
| primary | Accent 琥珀 | AccentDim | Bg（深字压琥珀） | 无 | 唯一 CTA，一卡最多一个（超出出 Lint hint） |
| ghost | 透明 | Card2 | Tx | 1px Line | 次要动作（取消） |
| plain | 透明 | — | Accent | 无 | 文字链 |
| default | Card2 | Line | Tx | 无 | 普通按钮 |

### 5.4 通用属性与布局

通用属性：`id`（进卡片节点表，供 patch/toggle/report 引用；list 行内不注册防重名悬垂）、
`w`/`h`（像素）、`grow`（flex_grow）、`pad`、`gap`、`size`（label=字体档位，icon=像素）、
`mono`、`tone`/`color`（文字）、`fill`/`bg`（背景，fill 用语义 token 优先，自动 radius 12）、
`hidden`（初始隐藏，配 toggle/show 做"展开详情"）。

布局尺寸决策（`ApplySizing`，`render.cc:234-255`）：显式 `w` > `grow` > 类型默认。**行内**：
可生长类型（button/slider/bar/spacer/arc/choice/chart）自动 grow=1，label/icon/switch 保持
自然宽；**列内**：可生长类型和 label 全宽。整体自适应为主——工具描述明确 "layout is
adaptive — rarely need w/h"。表格对齐技巧（描述 + prompt 双处强调）：多列表格 = 堆 row，
每行**每个 label 都 grow:1**，否则 label 收缩到文本宽、列对不齐。

### 5.5 主题：语义 token 与双主题自适应

pi_theme 双主题（深 AmberGlow / 浅 PaperInk），12 个语义 token（`pi_theme.cc:13-20`）：

| Token | 深色 | 浅色 | 语义 |
|---|---|---|---|
| bg | #0E0C09 | #F2EDE2 | 页面底 |
| card | #16130E | #FBF8F1 | 卡面 |
| card2 | #181510 | #EAE5D8 | 轨道/按压底 |
| line | #2A251C | #DAD2C0 | 边框/分隔 |
| line2 | #3A3226 | #C6BCA6 | 次级线/网格 |
| tx | #EDE6D6 | #2B251B | 正文/把手 |
| dim | #97907E | #6E6552 | 次要字 |
| faint | #5F5849 | #A79D85 | 最弱字 |
| accent | #FFAE1F | #B87400 | 琥珀强调 |
| accent_dim | #8A6420 | #D9A94F | 强调按压 |
| ok | #9BC46B | #5E8A2E | 成功 |
| err | #E25B4E | #C23B2E | 错误 |

LLM 的 `tone`/`fill` 就取这些 token 名（`error`→err、`text`→tx 有别名），**自动适配明暗主题**；
也允许 `color`/`bg` 直给 `#RRGGBB`（但 prompt 不鼓励）。主题切换（`pi_theme.cc:135-147`）：
静态配色走"属性×token"惰性共享 `lv_style_t`，Set 时改样式值 +
`lv_obj_report_style_change(nullptr)` **全 UI（含已渲染卡片）即时换装，无需重建**。
例外（一次性取色不自动重刷）：qrcode（刻意固定）、chart 网格与折线色、图标的弧段——
stock_chart 用主题 listener 主动全量重绘补齐。

---

## 6. 数据面：DataHub 与绑定/刷新

DataHub（`pi_card_data.{h,cc}`）是"设备状态 → LVGL subject"的注册表：每条路径一个
`lv_subject`，控件经 LVGL 官方 bind API（`lv_slider_bind_value` 等）或 observer 挂上去。

### 6.1 路径全表（37 条静态路径）

内置 9 条在 `pi_card_data.cc:231-322`（RegisterBuiltins），其余 28 条由 pi_screen 在
`pi_screen.cc:3467-3678` 运行时注册。**rw = 可写（有 setter），hist = 进历史缓冲**：

| 路径 | 类型 | 读写 | 量程 | 来源（mhal） |
|---|---|---|---|---|
| audio.volume | Int | **rw** | 0-100 | audio Get/SetVolume |
| display.brightness | Int | **rw** | 5-100 | backlight Get/SetBrightness |
| display.sleep_s | Int | **rw** | 0-3600 | Settings ui/sleep_s + pi_sleep |
| ui.theme | Bool | **rw** | — | pi_theme IsLight/Set（直接翻转+NVS） |
| speech.tts | Bool | **rw** | — | pi_screen s_tts_on |
| battery.level | Int | ro **hist** | 0-100 | sysmon 1Hz 原子快照 |
| battery.charging | Bool | ro | — | 同快照 |
| battery.voltage_mv | Int | ro **hist** | 3000-4300 | GetBatteryExt |
| battery.current_ma | Int | ro **hist** | — | 同上（±号） |
| battery.temp_c10 / tte_min / soh_pct / fcc_mah / cycles | Int | ro | 各异 | BQ27220 扩展 |
| net.type | String | ro | — | "wifi"/"4g"（**刻意无 setter**） |
| net.rssi | Int | ro **hist** | — | WiFi dBm / 4G CSQ |
| net.ssid / net.ip / net.operator / net.cell | String | ro | — | 网络门面 |
| net.connected | Bool | ro | — | IsConnected |
| storage.sd | Bool | ro | — | IsSdMounted |
| storage.free_mb | Int | ro | — | statvfs |
| sys.cpu | Int | ro **hist** | 0-100 | GetCpuUsage |
| sys.heap_kb | Int | ro **hist** | — | GetHeapKb |
| bt.connected | Bool | ro | — | bt GetConnState |
| bt.mode | String | ro | — | rx/tx/music/none |
| imu.pitch / imu.roll | Int | ro | ±90 / ±180 | IMU 5Hz 快照 |
| power.usb_in / power.wireless_charging | Bool | ro | — | 电源检测 |
| gps.fix / sats / alt_m / speed_kmh | Bool/Int | ro | 各异 | GPS（门控默认关，no-fix 优雅回落） |
| gps.lat / gps.lon | String | ro | — | 1e-5 度格式化，no-fix 显 "--" |

派生事实：**可写路径只有 5 条**（audio.volume、display.brightness、ui.theme、speech.tts、
display.sleep_s）；history 路径 6 条（battery.level、net.rssi、voltage_mv、current_ma、
sys.cpu、sys.heap_kb）；全部 37 条对 worker 线程读安全（进 ui_render 的 state）。

**红线示例**：`net.type` 刻意只读（`data.cc:278-289`）——切网络是持久化 + esp_restart 的
大动作，绝不给 LLM 一个能静默重启设备的 setter；要切走 `invoke net.switch_type`（Confirm 级）。

### 6.2 注册机制：静态 Provider 与动态 DynProvider

**静态**：`Register(path, type, getter, setter, worker_read, lo, hi, keep_history)`
（`data.cc:204-229`），只在 LVGL 线程 Create() 期一次性调用，之后 `entries_` 结构只读、
subject 指针永生 → Has/TypeOf/Writable/ListPaths/ReadForWorker 对 worker 线程**无锁安全**。
`worker_read` 强制显式表态（Safe/Unsafe）无默认值。lo>hi 表示无量程。

**动态**（`data.h:122-137`，`data.cc:166-172`）：`DynProvider{prefix, hint, match,
on_first_acquire, on_last_release}`，解决"符号是开放集不能预注册"的场景（目前唯一实例：
`stock.` 前缀，`pi_card_stock.cc:652-666`）。设计分两层：

- **元数据层**（Has/TypeOf）走 `match` 纯函数，worker 线程安全；动态路径恒为 String/只读/
  无量程，`ReadForWorker` 恒 false（异步推送模型，没有同步 getter，不进 state）。
- **运行态层**（subject + refcount）存独立 `dyn_entries_`，只 LVGL 线程触碰；首个 Acquire
  触发 `on_first_acquire`（如建股票订阅），refcount 归零触发 `on_last_release`（退订），
  subject 一经创建永生。动态 entry 上限 kDynMax=64。

`ListPaths()` 只列静态路径；动态路径的用法靠 provider 的 hint 与
`pi_card_stock::BindPathsDesc()` 单独写进工具描述。

### 6.3 刷新机制

**活性 poll（PublishLive，`data.cc:342-373`）**：进程级**单个 1Hz lv_timer**。早退条件：
没有任何活跃绑定（active_live_count_==0）且没有 history 路径需要采样时直接 return。每 tick：

1. 对 **refcount>0 且只读** 的路径重跑 getter → 写 subject → 绑定控件经 observer 自动更新。
   即"**有卡片绑着才刷，没人看就不采**"（history 路径除外，见下）。
2. 对 keep_history 路径读值、钳量程、push 环形缓冲（容量 **120 点，1Hz ≈ 2 分钟窗口**，
   FIFO 满弹最旧），并回调 history sink 给 chart 增量喂点。history **始终记录**不管有无绑定
   （待机常驻 chart 需要"过去"的数据）。

**写入（Write，`data.cc:187-200`）**：只对有 setter 的路径生效；Int 先 `ClampRange` 收口
量程再落硬件——"亮度设 -5"、"音量 999" 之类越界值一律被钳住。Write **不改 subject**
（subject 由控件双向 bind 自己同步，避免双写）。

**双向绑定防回环（关键设计，`render.cc:318-350`）**：依据 LVGL 的一条底层性质——
**程序化改 subject 不触发 VALUE_CHANGED，只有用户交互触发**。因此把硬件回写挂在控件事件上
就天然无回环，不需要 publishing/suppress 标志。再加两道防线：

- slider/arc 的 VALUE_CHANGED 回调先判 `!lv_obj_has_state(w, LV_STATE_PRESSED)` 直接
  return——防"poll 刷 subject → observer 改控件值 → 误触发事件被当成用户操作回写"；
- 拖动中 **150ms 节流** + RELEASED 事件补终值，拖一整条 slider 不会打爆 I2C。

另一个相互配合的点：**可写路径根本不进 poll**（PublishLive 只刷无 setter 的路径）——所以
用户拖卡片 slider 时不会有 poll 来抢；代价是外部改值（如快捷面板拖亮度）不会自动回显到已
渲染卡片，这是有意取舍（`data.h:11-14`）。

### 6.4 chart 与 history

`chart` 控件（`render.cc:720-764`）不走 subject bind：渲染时用 `HistorySnapshot` 种子填点
（LINE 图，SHIFT 更新模式，points 默认 60 钳 [8,120]，3 条水平网格线，无轴无图例），然后
`AddHistorySink` 订阅增量——PublishLive 每秒采样后回调 `lv_chart_set_next_value`。Y 轴量程
优先用路径注册量程（RangeOf），否则按种子 min/max 自适应。控件 DELETE 时 RemoveHistorySink。
校验期强制 bind_history 必须是 history-enabled 路径，否则报错并列出全部可用 history 路径。

---

## 7. 卡片数据模型（data / list / bind_data）

每张卡可携带一份 **卡级数据** `data:{key: scalar|array}`（cJSON object，卡片持有，随卡销毁）。
它是"列表/模板渲染"的数据源，也是 ui_update 增量改内容的落点——改数据，不改结构。

**消费者登记**：渲染时两类节点登记为 DataConsumer（`host.h:54-70`）：

- **Label 消费者**：label 声明 `bind_data:'key'`（且无 bind）→ 显示 data[key]；text 为空直显
  Stringify(v)，否则做 `{value}`/`{v}` 模板替换。
- **List 消费者**：list 节点把 item 模板克隆进 card->json_pool 常驻，登记
  {key, item_tpl, empty_text, eff_max, depth, limits}。

**list 渲染**（`render.cc:773-831`）：行数 = min(数组长, eff_max)，eff_max = 声明的 max，
否则数组实长，否则 8，硬顶 [1,20]。每行 `cJSON_Duplicate(item)` → `SubstRecord` 占位符替换 →
`RenderNode(in_list_row=true)`。占位符只替换字符串**值**（不碰 key）：`{i}`=0 基下标、
`{n}`=1 基、`{item.FIELD}`=本行记录字段（number→%g、bool→"1"/"0"、缺失→空串）。空数组且给了
`empty` → 渲染一行 Faint 色占位文案。**行内限制**：不许嵌套 list；行内节点不注册 id；行内
action 只允许 report/set/close（行身份靠 {i}/{n}/{item.*} 编进 report 文本）。节点预算按
`eff_max × 模板节点数` 预留记账，防止 20 行 × 5 节点炸穿 64 上限。

**ui_update 的 data 四操作**（`ApplyDataOps`，`host.cc:425-500`）：`set`（逐键 upsert）、
`append`（数组不存在则建，**20 行硬顶**）、`remove`（按 index 或按行内 `id` 字段查找）、
`replace`（按 index 换整行）。所有越界/类型错**只异步报错绝不崩**。改完后
`RefreshDataConsumers`（`host.cc:504-548`）只刷 changed key 的消费者：Label 改 text；List
**保存 scroll_y → lv_obj_clean → 全量重渲子树 → 恢复 scroll_y**（用户翻到一半列表不跳回顶）。
末尾 overlay 重跑高度稳定器 / chat 卡重新滚到底。

---

## 8. 动作引擎与 invoke 命令

事件（`on_click`→CLICKED、`on_change`→VALUE_CHANGED、`on_release`→RELEASED）挂 action
**数组**，按序执行。8 种 action（`pi_card_actions.cc`，分发器 `DispatchCb :159-238`）：

| do | 参数 | 语义 | 回程 |
|---|---|---|---|
| close | — | `lv_obj_delete_async(root)` 关本卡（删祖先必须 async） | 零 |
| set | path, value? | 写 DataHub（省略 value 用触发控件当前值），经 clamp 落硬件 | 零 |
| toggle / show / hide | target:'id' | 改目标节点 HIDDEN 标志（toggle 取反）；配 hidden:true 的块做"展开详情" | 零 |
| patch | target:'id', props | 本地版 ui_update：改 text/value/checked/hidden/tone/color；props.text 支持 `{v}/{value}/{label}` 占位符 | 零 |
| invoke | cmd:'…' | 执行固件命令（见下） | 零 |
| report | text:'…{v}…{label}…' | 把文本注入回 LLM 对话 | **一整轮 LLM 往返** |

**report 细节**（`:176-189, :92-151`）：

- 占位符：`{v}`/`{value}` = 触发控件当前值；`{label}` = choice 选中段文本。
- **自动携带状态快照**：遍历卡上所有带 id 的有值控件（slider/bar/arc/switch/choice），拼
  `id=v` 空格分隔附在文本后（choice 报 `idx(label)` 双份）。设计考量：id 是模型声明"我关心
  这个值"的既有表面积；纯装饰节点无 id 天然不进噪音；绑硬件路径的控件也带上——因为模型读
  不到 DataHub，report 是值回流的唯一通道。
- **500ms 节流**保留最后一条；注入文本前缀 **「卡片操作」**，经 `pi_agent_task_inject`
  （`pi_agent_task.c:839-852`）进入对话：agent 正在跑 → `pi_agent_steer`（同 run 内接续，
  顺带掐断当前 TTS）；空闲 → 起新一轮。以 **user message** 身份进 transcript——这正是
  "report 显示为用户消息、代价三重"的机制根源。

**invoke 命令注册表**（`pi_card_cmd.{h,cc}`）三级安全模型：

| 级别 | 行为 | 当前命令 |
|---|---|---|
| Safe | 立即执行 | net.reconnect（幂等）、device.vibrate（震 200ms）、bt.reconnect（连上次设备）、gps.disable |
| Confirm | 弹**固件确认 sheet**，用户点确认才执行，模型无法绕过 | net.switch_type（"切换网络通道将重启设备"）、gps.enable（占 UART0）、session.new（"开始新对话？"） |
| forbidden | **干脆不注册**（不进能力面） | power.off、factory.reset 等 |

确认 sheet 由 pi_screen 注入 hook（`SetConfirmHook(ShowConfirmSheet)`，`pi_screen.cc:3693`），
与 pin 移除 ✕ 共用同一参数化确认 UI。命令清单同样由 `BuildCommandsClause` 运行时拼进描述
与 prompt（"safe:…; confirm(prompts user):…"）。

---

## 9. Preset：四种预置卡片

Preset 是**纯语法糖**（`ExpandPreset`，`host.cc:996-1007`）：worker 侧把 `{preset, slots}`
拼成普通 spec 树，再走完全相同的 Validate + Render——没有第二条渲染通路。缺 slot 返回具名
错误。四种模板（`host.cc:764-992`）共用 `MkHeader`（eyebrow + title 双行头）：

- **confirm** `{title, body?, confirm:{text?, report?|set?}, cancel:{text?}}`：
  eyebrow "CONFIRM" + 标题 + 正文 + 一行两钮：cancel（ghost，默认"取消"，on_click=[close]）、
  confirm（primary，默认"确认"，on_click=[动作, close]）。confirm 动作给 report 就报文本，
  给 set 就写路径，都没给回落 report 标题。
- **form** `{title, fields:[{type:slider|switch|choice, id, label, …}], submit:{text?, report?}}`：
  每字段一行（label grow:1 + 控件 id=fid grow:2），submit 是 primary 按钮（默认报"已提交"）。
  report 自动携带全部字段值——表单就是"id 控件 + 一次 report"的组合。
- **dashboard** `{title, metrics:[{label, bind, kind?:bar|value, fmt?, icon?}]}`：
  eyebrow "STATUS"，每 metric 一行：icon? + label + （bar 绑路径 | 右对齐 value 标签
  bind+fmt，默认 "%d"）。
- **menu** `{title, items:[{text, report?}], style?:'buttons'|'choice'}`：buttons 风格每项
  一个 default 按钮（on_click=[report, close]）；choice 风格一个 choice(id="menu") + primary
  "确认"钮。

system prompt 第 11 段给出四种 slot 签名，鼓励常见形状走 preset（更短、更不会写错）。

---

## 10. 卡片生命周期：chat / overlay / standby

三种 display 模式（`host.h:30`，0=chat / 1=overlay / 2=standby）：

### 10.1 chat —— 聊天流内联卡（默认）

经 FeedHooks `begin_row/end_row` 塞进聊天 feed：feed 末尾建一个全宽透明占位行，卡片 root
（自带卡片外观）挂在里面，渲染完滚到底。**流式顺序保证**：DrainQueueTick 处理 RENDER 事件前
先把当前流式文本 `AppendAssistantText + FinalizeMdView` 收尾（`pi_screen.cc:2292-2296`），
卡片后的文本另起新气泡——"文本 → 卡片 → 文本"顺序与模型输出一致。随 feed 清空（新会话）
自然销毁。

### 10.2 overlay —— 模态浮层

- 结构：全屏 scrim（吞点击、禁右滑返回）+ 居中 wrapper（80% 宽），卡片带投影浮起。
- **右上角强制加 40px 关闭钮**（`host.cc:140-154`）——防 LLM 忘给出口，用户永远关得掉。
- **保底 TTL**：`ttl = (0 < ttl_ms < 5min) ? ttl_ms : 5min`（`host.cc:404-408`）——overlay
  必定自动关闭，不存在永久模态。
- **数量上限 3**（`kMaxOverlays`，超限整次渲染丢弃并异步报错让 LLM 先 close 旧卡）。
- 高度稳定器 `ReflowOverlay`（`host.cc:251-269`）：自然高超过竖向分辨率 86% 时钉死固定高度
  并开内部滚动。不能用 max_height+SIZE_CONTENT 的原因在长注释里：LVGL 的 calc_content_height
  用滚动平移过的绝对坐标，越滚越矮正反馈失控（sim 实测 619→496）。
- 有 overlay 时禁止进入息屏（`HasOpenOverlay`）。

### 10.3 standby —— 待机屏常驻挂件（pin）

- **单槽**，固定 id `pin`（LLM 传的 card 被忽略/强制改写），新 pin 替换旧 pin。
- 挂在待机屏 pin_host（仅 Idle 态可见）；wrapper 75% 宽、透明、非 clickable——空白处按压
  穿透回 PTT（不挡语音交互）。
- **持久化**：spec+data 打成信封 `{"v":1,"root":…,"data":…}` 写 NVS `"ui"/"pin"`，
  **3KB 上限在 worker 前置校验**（超限同步拒绝，理由：留到 drain 会出现"渲染成功但没持久化"
  的三态分叉，`host.cc:1067-1079`）。开机 `RehydratePin`（`host.cc:625-656`）读 NVS 重放
  同一条 OnRenderEvent 通路；解析失败/版本不符/校验不过一律静默擦 key，坏 JSON 不卡开机。
- 屏上 ✕ 角标（36px）→ 固件确认 sheet → 确认才 `UnpinCard`（擦 NVS + 删卡）。`ui_close
  card:'pin'` 同路。**pin 被动删除（屏重建/替换）只清运行态不擦 NVS**——重启后还在。
- 不设 TTL、不占 overlay 配额、不改"最近卡片"指针。
- system prompt 特别提示：挂件里的 report 会让模型在**后台**跑（用户不在聊天界面），优先
  本地 action/invoke。

### 10.4 清理的唯一通道

每张卡 root 挂 `LV_EVENT_DELETE` → `OnRootDeleted`（`host.cc:156-180`）：无论因何而删
（ui_close、TTL、新会话 ClearFeed、屏卸载、显式替换）都汇到这一处——删 TTL timer、
Release 全部 DataHub 引用、销毁卡级 data 与 json_pool、维护 overlay 计数与 pin 状态、
出注册表。不存在第二条清理路径，也就不存在漏清理。

---

## 11. 校验、Lint 与错误回传

### 11.1 Validate：同步硬校验（worker 线程）

`Validate()`（`render.cc:1170-1176`）先两遍扫描收集全卡 id（允许 toggle/patch 前向引用），
再递归 `ValidateNode`。硬规则（错误串都带修复指引）：

- 结构：必须 object / 深度 ≤8 / 节点 ≤64（list 按 eff_max×模板节点数预留记账）/ type 在
  16 种白名单内。
- list：不许嵌套；必须有 item 与 bind_data。
- chart：bind_history 必须是 history 路径（错误里列出全部可用者）。
- qrcode：text 非空 ≤256B。choice：2-6 个字符串选项。stock_chart：symbol 合法 + 同屏 ≤3。
- **bind 存在性**：路径必须 `DataHub::Has`（含动态 provider match）；前缀命中但格式错时附
  用法提示（如 `stock.茅台.price` → 提示 symbol 要用腾讯代码）。
- **fmt 类型匹配**（`FmtSafeForType`，`render.cc:80-117`，校验器与渲染器共用）：至多 1 个
  占位符；数值路径只许 d/i/o/u/x/X/c，字符串路径只许 %s；禁 %n、禁 `*` 动态宽度。
  **%s 套数值路径直接拒绝**——newlib 的 vsnprintf 会 `strlen((char*)value)`，断网时
  net.rssi=0 就是解引用空指针崩机。这是三层防线（提示词/校验/渲染兜底）的中坚。
- **数值控件不许绑 String 路径**（slider/arc/bar/switch/choice）：bind 落 lv_subject 的
  int union，绑 String 读出来是垃圾值。
- 动作校验（`ValidateActions`，`actions.cc:291-366`）：do 在 8 种白名单内；toggle/show/hide/
  patch 的 target 必须是卡内声明过的 id；set 的 path 必须可写（错误列出全部可写路径）；
  invoke 的 cmd 必须已注册（错误列出可用命令）；list 行内只许 report/set/close。

### 11.2 错误回传的两条路

- **同步**（worker 返回值）：preset 展开失败、Validate 失败、pin 超 3KB、OOM、队列满——
  作为 tool_result 的 error 直接回 LLM，模型看到就重试修正。
- **异步**（drain 阶段才能发现的失败）：overlay 超限、渲染器 null、update 目标卡/节点已消失
  （附带说明"新会话已清空全部卡片"这类原因）、data 操作越界——`ReportAsyncError`
  （`host.cc:92-96`）把 `「卡片错误」<msg>` 经 `pi_agent_task_inject` 注回对话，模型下一轮
  能看到并自纠。ui_close 找不到卡是唯一静默不报的情形。

### 11.3 Lint：非阻塞设计建议（hints）

Validate 通过后跑 `Lint`（`render.cc:1178-1331`），产物进返回值的 `hints[]`，不挡渲染：

1. invoke 指向 Confirm 级命令 → 提醒"会弹固件确认，不会立即执行"。
2. chart 绑非 history 路径 → "will render empty"；points 越界 → "clamped to [8,120]"。
3. **控件 on_change 挂 report** → 最贵反模式警告："reports on every change and costs an
   LLM round-trip; use on_release or local patch/set/toggle"。
4. 惰性控件：choice 无 id/bind/on_change → "selection goes nowhere"；其他输入控件既无可写
   bind 又无 id/handler → "is inert… render dimmed/read-only"。
5. primary 按钮 >1 → "keep exactly one amber call-to-action"。
6. 无任何 label → 建议加标题。
7. 节点数 ≥56 或深度 ≥7 → 接近限额预警。

与 Lint 呼应的渲染期**死控件降级**（`render.cc:900-911`）：switch/slider/arc 若"无效果"
（没绑可写路径、没有 on_change/on_release、也没有 id），去掉 CLICKABLE 并降到 60% 透明度
渲染成只读态——绑只读路径的 slider 自动变成"仪表"而不是"骗人的旋钮"。

### 11.4 诊断日志

worker 侧：`ui_render REJECT [stage]: <err> | card=<整卡JSON>`（ESP_LOGE，stage ∈ preset/
no-root/validate/oom/pin-size/queue-full）与 `ui_render OK: card=<id> display=<n> (+lint
hints)`；ui_update/ui_close 同款。drain 侧：`rendered card <id> (<n> nodes)` / 各失败 WARN。
`pin envelope oversized reached drain` 是"前置校验有 bug"的哨兵 ERROR。一条日志即可定位
哪张卡、哪个阶段、为什么。

---

## 12. 线程模型与代次过滤

三个线程 + 一条队列：

| 线程 | 职责 | 碰什么 |
|---|---|---|
| agent worker（core0, prio4, 8KB 栈） | pi-c SSE 循环、工具 execute、Validate/Lint、BindStateJson、stock 工具同步抓取 | DataHub 元数据（无锁只读）、pi_ui_queue 入队；**绝不碰 LVGL** |
| LVGL 线程 | DrainQueueTick(80ms, budget 64 事件/tick)、建/改/删控件、DataHub Acquire/Release/Write/poll、事件回调、RehydratePin | s_cards 注册表等全部 UI 态（单线程持有，无需锁） |
| tts_pump / stock worker | 阻塞 TTS / 腾讯 HTTP | 各自队列 |

跨线程只有两样东西：**FreeRTOS 队列 pi_ui_queue（深 32，非阻塞入队，满即失败告知 LLM
"UI busy, retry shortly"）** 和 **原子代次**。

**代次双层**（`pi_agent_task.c:148-149`）：`g_session_gen` 新会话自增；worker 每轮 run 开跑
时快照成 `g_active_gen`，打进该轮产生的每个 UI 事件（文本与卡片同源）。drain 侧
`evt.gen != 当前代次` 直接丢弃释放——barge-in（用户打断）后旧 run 迟到的卡片与文本被**一致地**
过滤，不会出现"文本被打断了但旧卡还蹦出来"。入队时若不打代次，栈上未初始化值会让全部卡片
事件被过滤——这是 `host.cc:73-75` 特意注释的教训。

---

## 13. 股票子系统

设计原则：**行情序列绝不过 LLM**。模型只负责决定"显示哪只股票"（下发 symbol），数字快照走
stock 工具，连续行情由设备直连腾讯接口自取自画（`pi_card_stock.h:6-8`）。三条独立能力：

### 13.1 stock 工具（LLM 数字快照）

`stock_tool.cc`：`query` 模式先 smartbox 搜索（中文名/拼音/代码），取前 5 个候选批量报价；
`symbols` 模式直接报价（≤8 支）。返回紧凑 JSON `{quotes:[{sym,name,price,chg,pct,open,high,
low,prev_close,vol,amount,pe?,pb?,float_cap_yi?,total_cap_yi?}]}`——估值字段仅 >0 才带
（省 token、防 0 值误导）；数值手动 Round2 绕开 cJSON 的 %1.15g 精度噪声。同步阻塞跑在
agent worker（HTTP 6s 超时），绝不碰 LVGL。

### 13.2 stock_chart 控件（自刷新行情卡）

`{type:'stock_chart', symbol, name?, mode?:'min'|'5d'|'day'|'week', w?, h?}`。symbol 用
腾讯格式（sh/sz+6 位、hk+5 位、us+TICKER[.N|.OQ]），校验期验证；同屏最多 3 个（atomic
计数供 worker 校验）。默认 600×260（w 钳 [240,656]，h 钳 [120,400]）。

结构：头行（股票名 puhui_30_4——必须完整字集字体否则缺字 + 代码 mono | 现价 + 涨跌）、
canvas 画布区（RGB565，PSRAM 分配，四角浮动 max/min 价与涨跌% 标签，居中"加载中…"）、
脚行（模式名 + 北京时间 HH:MM + "点图切周期"）。**点击画布循环切换** 分时→五日→日K→周K
（切换即作废在途结果、立即抓新数据）。**红涨绿跌**（#FF3B30 / #26C281，固定语义色不随主题）。

**自刷新**：模块级单个 1s lv_timer 驱动所有卡片与订阅（全删光自动 pause）。每卡两条独立
抓取（报价 + 图表），节奏 = {盘中, 盘外, 重试, NTP 未同步}：报价 {5s, 60s, 10s, 15s}、
分时 {10s, 60s, …}、K线 {30s, 120s, …}。卡片滚出视口/屏幕隐藏直接跳过；盘外拉长到
60/120s，临近开收盘边界 ±2min 恢复盘中节奏抢卡边界。主题切换经 listener 全量重绘。

### 13.3 数据源与解析（`main/stock/`）

- 端点（`tencent_endpoints.h`）：报价 `qt.gtimg.cn/?q=…`（GBK 响应；美股 query 须剥 .N/.OQ
  后缀）；分时/五日/K线 `web.ifzq.gtimg.cn/appstock/app/<ctrl>/…`（三市 controller 各异）；
  搜索 `smartbox.gtimg.cn/s3/`。伪装 UA + Referer gu.qq.com，超时 6s。
- **GBK 安全解析**（`quote_parse.h`）：响应含 GBK 中文名，`~` 落在 GBK 低字节范围会切错——
  用 `~<echoCode>~` 锚点跳过名字段后再切（锚点后全 ASCII）。字段索引 2024-2026 实测三市一致：
  0 现价/1 昨收/2 今开/28 涨跌额/29 涨跌幅/30 高/31 低/33 量/34 额/35 换手/36 PE/40 振幅/
  41 流通市值/42 总市值/**43 仅 A 股是 PB**（HK/US 此位是名字串，strtof 回 0 视为缺失）。
  A 股量"手"×100、额"万元"×10000 换算。
- 分时：A 股收盘后接口用收盘价冻结补齐 15:05-15:30，按 cutoff 裁掉。五日 ~200KB 响应走
  字符串扫描不建 cJSON 树，每交易日独立抽稀 72 点、跨天不连线。K线默认日 22 根/周 26 根。
- **市场时段**（`market_hours.h`）：本地推算（北京墙钟）——A 股 9:30-11:30+13:00-15:00、
  HK 9:30-12:00+13:00-16:00、美股 21:30-次日 5:00（EDT/EST 并集，**不本地算夏令时**）；
  不管节假日——误判代价只是多几次轮询，不产生错数据。边界调度表列全 10 个切换点，
  跨午夜用 dayOffset 处理。
- 抓取 worker：单线程 + 双队列（深 8），结果堆分配经队列回 LVGL 线程排空；网络未就绪一律
  拒绝（否则 lwip assert panic 的开机竞态）；256KB PSRAM 工作缓冲 + 全局 ApiLock，所有腾讯
  抓取全局串行。

### 13.4 stock.<symbol>.<field> 动态绑定

DataHub 的 DynProvider 实例：任意 label 可 `bind:"stock.sh600519.price"`。首个绑定按 symbol
建**无控件的报价订阅**（复用同一 timer/worker/盘中 5s 节奏），一次报价推全 17 字段；卡删
refcount 归零自动退订。同时订阅 symbol ≤6。**17 字段**：price/chg/pct/open/high/low/
last_close/avg_price/amplitude/turnover/volume/amount/pe/pb/float_cap/market_cap/time——
推送侧已格式化成 String（人性化万/亿、%+.2f%%、pb 非 A 股显 "--"），label 无需 fmt。

### 13.5 图表绘制（`stock_chart_renderer.cc` + `chart_math.h`）

LVGL9 canvas layer 绘制。分时/五日：昨收虚线（dash 6/5）+ **双色折线**（段跨昨收时按
参考价拆两段分别着色）+ 分时按完整交易时段留白（盘中画到当前时刻）+ 五日画日分隔竖线。
日K/周K：≤60 根画蜡烛（**阳线空心、阴线实心**），>60 根退化为单色折线。Y 范围：分时上下
偏离独立计算、昨收线钳在中间 80% 区间；K 线 sweep 高低 + 5% padding。价格映射到
[0,10000] 整数刻度，避开 lv_coord_t int16 在价格 >327.67 时的环绕翻图。

---

## 14. 限额速查表与健壮性设计汇总

### 14.1 限额

| 项 | 值 | 出处 |
|---|---|---|
| 节点数 / 深度 | 64 / 8 | `host.h:35-38` |
| list 行数 | max 钳 [1,20]，append 后同样 20 硬顶 | `render.cc:580-585`, `host.cc:446` |
| overlay 同屏 / TTL | 3 / 保底 5min | `host.cc:43-44` |
| pin 信封 | 3KB（worker 前置拒绝） | `host.cc:48` |
| qrcode text | ≤256B | `render.cc:1088-1098` |
| choice 选项 | 2-6 | `render.cc:1099-1117` |
| chart points | 钳 [8,120] | `render.cc:1262` |
| history 缓冲 | 120 点 @1Hz | `data.h:193` |
| 动态路径 | 64 条；股票订阅 symbol ≤6 | `data.h:185`, `stock.cc:91` |
| stock_chart 同屏 | 3 | `stock.cc:39` |
| UI 事件队列 | 深 32；drain 64 事件/80ms | `pi_agent_task.c:753`, `pi_screen.cc:2043` |
| report 节流 | 500ms 保留最后一条 | `actions.cc` kThrottleMs |
| slider 回写节流 | 150ms + 松手终值 | `render.cc:331-332` |
| prompt+desc 预算 | 9216B 软告警 | `pi_agent_task.c:781-796` |

### 14.2 健壮性设计一览

- **三层 fmt 防线**：提示词警告 → 校验拒绝 → 渲染器兜底回落安全默认。sim（macOS libc）对
  这类 UB 宽容复现不了，真机 newlib-nano 必崩，所以防线必须在校验层。
- **整卡回滚**：渲染任一节点失败删 root，不留半卡。
- **唯一清理通道** OnRootDeleted：所有销毁路径汇一处。
- **代次过滤**：barge-in 后旧 run 的文本与卡片一致丢弃。
- **量程双向收口**：绑定量程覆盖 LLM 给的 min/max（读侧），Write 前 clamp（写侧）。
- **能力面裁剪**：危险操作要么不注册（net.type setter、power.off），要么 Confirm 级固件
  确认，安全不依赖模型自觉。
- **overlay 永远关得掉**：强制关闭钮 + 保底 TTL + 数量上限。
- **持久化防砖**：pin 开机重放校验失败静默擦除，坏 JSON 不卡开机。
- **队列满即拒**：非阻塞入队，把背压变成 LLM 可见的"retry shortly"。
- **GBK/精度/字体等真实世界坑**：报价锚点切分、Round2、SafeFont CJK 回退、美股后缀剥离、
  int16 坐标环绕——都有专门处理并留了注释。

### 14.3 如何验证

- host 模拟器：`cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/pi_sim`
  ——pi_screen/pi_card 代码不改直接跑 macOS SDL2 窗口，真 DeepSeek。F9 / `PI_SIM_CARD_MS`
  可注入测试卡片，`PI_SIM_CMDFILE` 喂触摸手势，F12 截图（只抓 screen 不抓 layer_top）。
- 真机：`idf.py build && idf.py -p /dev/cu.usbmodem11101 flash`，随后 `uv run
  tools/serial_cap.py` 连续抓日志，`ui_render OK/REJECT` 一眼定位。
