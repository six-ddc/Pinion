# pi_card XML 线格式前端

> 状态：**已全量落地，XML 是 ui_render 唯一线格式**（编译器 `pi_card_xml.{cc,h}` +
> card_xml_test + SAX 流式预览 + 真机验证）。D1 已裁决：Haiku 四复杂任务 XML 臂零语法错 +
> 真机 DeepSeek 对照（JSON 连吃两拒 vs XML 首发即过）后，JSON 线格式提示词/schema/cardfmt
> 开关已整体退役；`root` 参数仅存内部/测试通道（sim 语料与脚手架直喂编译后 spec）。
> 背景结论与实证依据见 §1；As-built 偏离与补充设计见 §12；避坑清单见 §10。

## 0. 一句话

给 pi_card 增加一条 **HTML 式 XML** 线格式通道：LLM 产出 `<card>…</card>` 字符串，
设备端编译成现有 cJSON spec 后走完全不变的 Validate/Repair/solver/渲染器/DataHub/actions
管线；流式期用 SAX 事件驱动预览。JSON 通道保留并存，Haiku A/B 实测首错率与 token
成本后再决定 JSON 通道与 partial-JSON 预览机器的去留。

## 1. 为什么（实证依据，均来自 2026-08-01 真机/仿真攻防）

- **JSON 是流式 LLM UI 最不友好的线格式**：前缀必须括号平衡（催生了整套
  partial parser + 预览签名 + adoption + 迟到属性回刷机器，preview_prefix_test
  25239 checks 是复杂度自白）；键序敏感（data 必须排 root 后）；嵌套层级是弱模型
  高频错型（真机实录：rows 写成一维叶子数组连拒两次、`{"divider":null}` 裸叶子当块）。
- **模型先验决定首错率**：五 few-shot 示例上线后 Haiku 表格/复合任务从连拒变
  3/3 一次通过——证明"没在示例里见过的形态永远写不对，few-shot > 规则文字"。
  HTML 是预训练里最海量的结构化格式，`<tr><td>` 先验刻在权重里，一维表格错型
  在 XML 里几乎不会发生。
- **属性原子性**：JSON 流式时对象的键逐个滴达（节点建好属性还在路上）；XML 元素
  的全部属性随开标签 `>` 一次到齐——"迟到属性"问题类整体消失。
- **pi-c 校验层实证**（关键）：pi-c **解析 $ref 且强制一切内联约束**（错误路径
  `root.3.rows.0: Expected array` 证明），任何 schema 内联 type/enum/min/max 都是
  "干拒绝+截断echo→弱模型原样重试"的硬拒点。XML 通道的工具 schema 只声明
  `{"xml":{"type":"string"}}`，零硬拒面。
- **DS strict 模式不可用**：要求全 required + additionalProperties:false，与
  键存在性形态分发/开放 data 对象范式冲突，且文档未承诺约束解码。
- token 成本：XML ≈ JSON 的 6 折（闭标签重复元素名，比行式 DSL 贵但先验强得多）。

## 2. 线格式规格（v1）

### 2.1 顶层

```xml
<card display="overlay" ttl="30s" id="c1">
  …块级元素序列，自上而下…
</card>
```

- `display`: chat（默认）| overlay | standby；`ttl`: 数字毫秒或 `30s` 缩写；`id` 可选。
- 深度钳制：card → 块 → 叶子（table 多一层 tr/td）。超深自动提升/剥壳 + note，不硬拒。

### 2.2 块级元素（对应 v2 grid 三形态 + divider 块）

| XML | 编译到 cJSON spec | 说明 |
|---|---|---|
| `<grid fill="card2">…叶子…</grid>` | `{"fill":"card2","cells":[…]}` | cells 形态 |
| `<table cols="项,值:num"><tr><td>…</td>…</tr>…</table>` | `{"cols":[…],"rows":[[…],…]}` | rows 形态；`:num` 后缀=数值列；cols 可省（沿用 solver 自动推断） |
| `<list bind="tracks" max="8" empty="暂无">…行模板叶子…</list>` | `{"bind_rows":"tracks","max":8,"empty":"暂无","item":[…]}` | bind_rows 形态 |
| `<divider/>` | `{"cells":[{"type":"divider"}]}` | 块级分隔线（模型直觉即合法语法） |

`<td>` 内容二选一：纯文本（→ label 叶子，td 自身属性并入）或恰一个叶子元素。

### 2.3 叶子元素（12 种，与 v2 同集）

```xml
<label role="value" bind="battery.level" fmt="%d%%" mono="1"/>
<label role="title">设备总览</label>
<button variant="primary" icon="play" side="end" tap="report:开始,close">开始</button>
<slider bind="audio.volume"/>  <arc …/>  <switch bind="ui.theme"/>  <bar min="0" max="100" value="50"/>
<choice id="dur" options="15分|30分|60分"/>
<icon name="wifi"/>  <qrcode text="https://…"/>
<chart bind="battery.level" points="60"/>   <!-- 编译为 bind_history -->
<stock_chart symbol="sh600519" mode="day"/>
```

- 元素文本内容 = `text` 属性；两者都给时属性优先。
- 布尔属性：`mono`/`hidden`/`checked` 接受 `1|true|空值`（`<label mono>` 即 true）。
- `chart` 的 `bind` 编译为 `bind_history`（对模型统一心智：绑路径一律叫 bind）。

### 2.4 动作语法（属性内联微语法）

事件属性：`tap`（on_click）、`change`（on_change）、`release`（on_release）。
值 = 逗号分隔的动作步序列，每步 `动词[:载荷]`：

| 步 | 编译到 action |
|---|---|
| `close` | `{"do":"close"}` |
| `set:audio.volume=50`（列表行内可用 `={i}`） | `{"do":"set","path":…,"value":…}` |
| `report:选了{item.title}`（含逗号时用引号包） | `{"do":"report","text":…}` |
| `toggle:hist` / `show:hist` / `hide:hist` | `{"do":"toggle","target":…}` 等 |
| `invoke:net.reconnect` | `{"do":"invoke","cmd":…}` |

v1 不做 `patch`（bind/data 覆盖其主要场景）；确需时留 JSON 通道，v2 再议（开放决策 D3）。

### 2.5 HTML 子集容错映射（先验外溢转免费容错，宽进严出）

`div/section`→grid、`h1`→label role=title、`h2`→section、`h3`→heading、`p/span`→label、
`hr`→divider、`ul/ol/li`→单列 table（每 li 一行）、`input type=range`→slider、
`input type=checkbox`→switch、`style/class/onclick` 等未知属性剥除+note、
未知标签剥壳保留子树或整个跳过+note。绝不因词表外溢拒卡。

### 2.6 转义与宽容解析

- 标准 XML 转义（`&amp;` 等）；喂 expat 前做一次**裸 `&` 预转义**（LLM 高频遗漏点）。
- 流式收尾/异常：未闭合标签全部自动闭合（浏览器传统）；expat 报错时从错误位置截断、
  已收标签照编译 + note，绝不整卡失败。

## 3. 架构与文件落位

```
LLM ──xml 串──► pi_card_tool_render (worker) ──► XmlCompile() ──► cJSON spec
                                                     │                │
                                              （新，纯函数，双端）      ▼
                                                          既有 Repair→Validate→入队
                                                                        ▼
                                                     既有 solver→渲染器→DataHub/actions
流式：UI_TOOL_ARGS delta ──► 增量 SAX(expat) ──► 块级/叶级预览（P3）
```

- **新增** `main/display/screen/pi_screen/pi_card/pi_card_xml.{cc,h}`：
  `bool XmlCompile(const char* xml, size_t len, cJSON** out_args, std::vector<std::string>* notes, std::string& err)`
  纯函数、无 LVGL 依赖、双端编译（sim 单测直链）。产出的 out_args 形如
  `{"display":…,"ttl_ms":…,"card":…,"data":…,"root":[…]}`，与 JSON 通道汇入**同一漏斗**
  （Repair/Validate/Lint/hints/state 快照全复用，pi_card_host.cc 不分叉）。
- **工具接线**（pi_card_host.cc / pi_card_tools.h）：`ui_render` args 增加互斥键
  `xml`（string）。host 侧 `pi_card_tool_render` 开头检测：有 `xml` → XmlCompile 替换出
  root/data/display 后走原逻辑；`xml` 与 `root` 并存时 xml 优先+note。schema 只加
  `"xml":{"type":"string"}`，无任何内联约束。
- **解析器**：优先用 ESP-IDF 自带 expat（组件 `expat`，SAX、增量喂入；sim 端 macOS 系统
  libexpat）。P1 先确认组件可用并测 PSRAM/栈占用；不可用则退路 = 自研 ~600 行宽容
  SAX（词表封闭、无 DTD/命名空间，工作量可控）。
- **提示词**（pi_card_host.cc `pi_card_system_prompt` + DESC 宏）：新增 XML 版描述与
  五示例（§6）；通过编译开关或 NVS `ui/cardfmt`（0=json 1=xml，默认 json）二选一注入，
  A/B 期间两套并存于代码、单次启动只注入一套（省预算）。
- **预览**（P3，pi_card_preview.cc）：xml 通道的 delta 不再走 partial-JSON parser；
  增量 SAX 驱动，闭合一个块级元素即整块渲染（复用 RenderGridBlockPreview）。
  叶级"生长"作 P3b 增强（开标签即建叶）。JSON 通道预览机器原样保留（A/B 对照）。

## 4. 与既有机制的关系（全部复用，不分叉）

- Validate/Repair/Lint、64 节点/8 grid 限额、报错带实际数、media.* 话术、hints 压舱句：
  编译产物进同一漏斗，自动全部生效。
- DataHub bind / state 快照回传（渲染即读取）、bind_rows/data、overlay/ttl/pin：不变。
- ui_update/ui_close：不变（更新面仍是 id/patch/data；XML 只是 render 的线格式）。

## 5. Haiku A/B 方法论（前会话已验证的成套工装）

1. `PI_SIM_DUMP_PROMPT=<file> ./sim/build/pi_sim` 导出与固件逐字节一致的
   system prompt + DESC + schema（两套格式各导一份）。
2. Haiku subagent（Agent 工具 model:haiku）读 dump 扮演设备助手，输出工具 args 原文。
   任务集 = 伤疤语料：越全越好仪表盘 / 分区+分隔线+表格 / 电池网络参数表 /
   闹钟列表点选 / 播放器诱饵（应拒画）/ 复合折叠卡 / overlay 确认卡。每任务 ≥3 次采样。
3. 产物写入 scratchpad `tests/corpus/`（sim CorpusDir 按 cwd 探测，覆写
   kCorpusFiles 已知文件名即可按 `PI_SIM_CARD_IDX` 喂入），
   `PI_SIM_CARD_MS=900 PI_SIM_CARD_IDX=N PI_SIM_EXIT_MS=2000` 逐张渲染。
4. 指标：**首错率**（REJECT/FAILED 次数）、hints 数、字节数（token 代理）、
   目测截图（`PI_SIM_SHOT=… PI_SIM_SHOT_MS=…`；chat 卡看不见时临时改 overlay 截）。

## 6. 提示词 v4（XML 版）要点

- 五示例逐一翻译成 XML（控制卡/表格/列表/折叠复合/overlay 确认），示例必须
  **原文过管线零 hints**（红线）。NEVER 清单改写为 XML 语境（如"叶子必须在块内"
  大多已被语法消化，保留 media 卡/emoji 两条）。
- 预算闸门现为 11264B（pi_agent_task.c 软告警）；XML 版示例略长，超了就再提
  （闸门只是提醒，注释里写清换取什么）。
- `BuildPathsClause`/`BuildCommandsClause` 动态清单两套提示词共用，不出现第三份硬编码。

## 7. 测试计划

- **新单测** `sim/build/card_xml_test`（比照 card_solver_test 的注册方式，sim/CMakeLists
  加 target）：≥30 用例——2.2/2.3/2.4 全映射、HTML 容错映射逐条、裸 & 转义、未闭合
  截断、td 双形态、动作微语法（含引号包逗号）、错误降级 note 断言。
- **伤疤语料 XML 版**入 `sim/tests/corpus/`（新文件名 x0_…开头，挂进 kCorpusFiles 或
  新增 env 指定路径），全部 `(ok)` 且 note 符合预期。
- 既有回归不许碰坏：26 张 JSON 语料、card_solver_test 616 checks、
  preview_prefix_test 25239 checks、双端 build。
- 真机：烧录后跑一轮伤疤话术（带表格的状态卡/设备总览/复合折叠），串口零
  REJECT/FAILED（采集：先 `pkill -f serial_cap`，烧后 `OUT=… uv run tools/serial_cap.py`）。

## 8. 实施阶段（/fable 编排建议）

| 阶段 | 内容 | 建议路由 | 验收 |
|---|---|---|---|
| P0 | 分支 `card-xml`（worktree 亦可：source 主仓 .idf-env.sh + cp sdkconfig 两步即可构建）；确认 expat 组件在 P4 目标可链接 | fast-worker | idf.py build 过、expat 符号可链 |
| P1 | `pi_card_xml.cc` 编译器纯函数 + card_xml_test | deep-reasoner 定内部结构 → fast-worker 实现 | 单测 ≥30 用例全绿，双端 build |
| P2 | 工具接线（xml 键）+ 提示词 v4 + cardfmt 开关 | fast-worker | dump 出 XML 版提示词、示例原文过管线零 hints、JSON 通道回归不坏 |
| P3 | SAX 流式预览（P3a 块级；P3b 叶级生长） | deep-reasoner 定与既有 preview 的接缝 → fast-worker | previewscene 式截图取证；JSON 预览回归不坏 |
| P4 | 全量回归 + verifier 独立核验 | verifier（model:opus） | §7 清单逐项证据 |
| P5 | Haiku 三格式 A/B（JSON 现状 vs XML；行式 DSL 可选第三臂）+ 真机烧录烟测 | 编排者自持（subagent 派发 Haiku） | §5 指标表 + 结论建议（JSON 通道去留） |

## 9. 开放决策（实施时拍板）

- **D1 通道去留**：**已裁决退役**（2026-08-02，Haiku A/B + 真机对照后用户拍板）。JSON 版
  提示词/DESC/schema/cardfmt 开关全删；预览要求 xml 键；`root` 参数保留为内部/测试通道
  （26 张 JSON 语料是编译后 spec 的测试夹具，不是线格式语料，继续服务 solver/校验回归）。
- **D2 cardfmt 开关形态**：NVS（免刷机 A/B）vs 编译宏（省 flash）。建议 NVS。
- **D3 patch 动作**：v1 缺席。若实测模型确有需求再定微语法（候选 `patch:id.text=…`）。
- **D4 `<td>` 是否允许多叶子**：v1 恰一个；表格单元格塞按钮组的需求出现再放开。

## 10. 避坑清单（前会话实证，新会话必读）

1. **pi-c 校验**：解析 $ref、强制一切内联约束；schema 里任何 type/enum/min/max 都可能
   变成"干拒绝+截断echo→原样重试"。新键一律裸 `{"type":"string"}`。
2. **提示词预算**：pi_agent_task.c 软告警（现 11264B）；`PI_SIM_DUMP_PROMPT` 是唯一
   可信测量（BuildPathsClause 动态注册，静态估算必偏）。
3. **示例红线**：few-shot 示例原文必须过管线零 hints（曾因示例本身是死控件教坏模型）。
4. **hidden/id 只在叶子级生效**（grid 块不注册 id）；折叠目标必须是整行独占叶子。
5. **真机格式串红线**：newlib-nano 无 %zu/%lld（不消费变参，后随 %s 直接崩）；
   `lv_snprintf` 同落 nano。一律 %u/%d + 显式强转。sim 复现不了这类 UB。
6. **sim 工装**：CorpusDir 按 cwd 探测（scratchpad 放 tests/corpus/ 即可喂自定卡，
   需含 00_device_ctl.json 供探测）；`PI_SIM_CARD_IDX` 按 kCorpusFiles 下标；
   启动前清残留 cmdfile；截图 `PI_SIM_SHOT`(bmp)+`sips` 转 png。
7. **串口/烧录**：永远 `/dev/cu.usbmodem11101`；serial_cap 占口会吞硬复位（跑旧固件），
   烧前 `pkill -f serial_cap`、烧后核验 uptime 归零。
8. **风格红线**：代码中文串直写不用 \x 转义；.clang-format 加载不了（手动保格式，
   120 列 4 缩进）；文档只述当前状态不写沿革；重构不留包袱（旧实现能删则删）。
9. **cJSON 陷阱**：RepairGrid 返回新指针时**不得**先 cJSON_Delete 旧 grid——调用方
   ReplaceItemInArray 会再删（UAF，已修过一次，编译器同型结构注意）。
10. **媒体墓碑**：media.* 路径/命令已注销，bind/invoke 命中给"内置播放器"专用话术，
    XML 编译层不需要额外处理（同一漏斗自动生效）。

## 12. As-built（实现与本方案的偏离/补充）

- **解析器是自研宽容 SAX，不是 expat**：IDF v5.5.4 组件表里没有 expat（v5 已移除），且 §2.6
  的宽容语义本就不是严格解析器的行为——`pi_card_xml.cc` 内置 ~400 行闭词表 SAX（小写化、
  实体+数字实体解码、裸 & 字面量、void 元素免闭合、错配闭标签 HTML 弹栈恢复、EOF 残缺
  token 丢弃+自动闭合、深度钳 16）。纯函数双端编译，流式预览逐帧复用同一函数。
- **schema 随 cardfmt 运行时二选一**（`pi_card_render_schema()`，pi_agent_task.c 与
  description 同一循环改填）：json 版维持原样含 `required:["root"]`；xml 版是零约束的
  `{"xml":string}`（另列 root/display 等键，弱模型 JSON 惯性回退也不会被 pi-c 硬拒）。
  只加 xml 键复用 json schema 不可行——required:["root"] 会在 pi-c 层干拒 xml-only args。
- **`<data>` 线格式**（方案 §2 未定义，补充设计）：`<data>` 是 card 的子元素；标量
  `<temp>24</temp>`（数值样式转 number）；列表行 = 重复出现的带属性元素
  `<tracks title="七里香"/>`（属性→记录字段）；同名标量二次出现升级成标量数组。
- **xml 参数通道常开**，不受 cardfmt 开关控制（开关只决定注入哪套提示词/schema）；
  `xml` 与 `root` 并存时 xml 优先 + note。
- **cardfmt 开关已随 D1 裁决删除**（NVS `ui/cardfmt` 不再读取）：XML 是唯一线格式，
  提示词/DESC/schema 单套注入。
- **块级 `<divider/>` 并入散叶子流**：不单独包块，跟相邻散叶子聚进同一个 cells grid
  （leaf divider 在 cells 里本就 SPAN_ALL 独占行，视觉等价，少一个块预算）。
- **提示词共享段**：`kPromptIntro` + `SharedPromptTail(invoke_syntax)` 两套线格式共用
  （json 版重构后 dump 与重构前逐字节一致，已核验）；实测 json 10985B / xml 11203B，
  都在 11264B 软闸内（xml 余量 61B，后续扩写先精简 DESC）。
- **语料**：`sim/tests/corpus/x0-x7.xml` 追加在 kCorpusFiles 末尾（既有下标稳定）；
  `.xml` 文件由 sim 包成 `{"xml":…}` args；x0-x4 是提示词五示例原文（零 hints 已核验），
  x5-x7 是伤疤话术卡（仪表盘/HTML 容错/列表点选）。

## 11. 参考

- `docs/CARD_V2.md`：v2 grid-only 规格（cJSON spec 的权威定义，编译目标）。
- `docs/AI_TO_UI.md`：AI→UI 总体架构。
- 相关源码：`pi_card_tools.h`（DESC/schema 宏）、`pi_card_host.cc`（system prompt、
  工具桥、BuildPathsClause）、`pi_card_render.cc`（Validate/Repair/Lint/预览渲染）、
  `pi_card_preview.cc`（流式预览）、`pi_agent_task.c`（TOOLS[]、预算闸门）、
  `sim/main.cc`（corpus 播放器、PI_SIM_* 工装）。
