# pi_screen 设计规范

本文档记录 `pi` 终端 App(`main/display/screen/pi_screen/`)的设计来源、视觉令牌、
布局规格、状态机与 LVGL 落地约定。目的是让后续任何人往这个屏幕加功能、改视觉,
或者在别的屏幕复用同一套语言时,有一份权威参照,不用重新逆向 `pi_screen.cc`。

## 0. 唯一真相源

视觉规格的唯一真相源是设计原型 HTML(720×720 原生像素,深色仪表盘主题):

```
/private/tmp/claude-501/-Users-cappudu-Code-six-ddc-pi-c/d6d6113d-188c-4220-967f-bcb73157272d/scratchpad/pi-terminal.html
```

这份文件是可交互的网页原型(四状态切换 + FLOW/ZEN 模式切换 + pi_ai 事件流动画回放),
里面的 CSS 自定义属性、`px` 尺寸、状态描述、事件表、LVGL 落地对照表都是**设计决策
的原始记录**,不是随手写的示例。`pi_screen.cc` 是这份规格的 LVGL 翻译,翻译时的取舍
(字体合并、颜色复用、四态精简成三视图等)在下面逐条列出。**改视觉先改这份 HTML 达
成一致,再回来改 `pi_screen.cc`,不要反过来**——HTML 不联网、不依赖 IDF,改起来快,
是最省成本的设计验证环境。

## 1. 色彩令牌

六色一律纯色填充,不用渐变/阴影/半透明混合(LVGL 上这些都贵)。层次全靠 1px 细线
和字重区分。CSS 变量 → LVGL 常量(`pi_screen.cc:45-56`)一一对应:

| CSS 变量 | 十六进制 | LVGL 常量 | 用途 |
|---|---|---|---|
| `--bg0`(屏底,原型页面背景) | `#0B0A08` | 不直接用 | 网页背景,不是屏幕背景 |
| 屏幕自身背景 `.screen` | `#0E0C09` | `kBg` | 屏幕背景色 |
| `--bg1` | `#12100C` | `kCard` | 工具卡 / 选中态背景 |
| `--bg2` | `#181510` | `kCard2` | CTX 进度条槽 / 模式按钮按下态背景 |
| `--line` | `#2A251C` | `kLine` | 1px 细线(几乎所有分隔线) |
| `--line2` | `#3A3226` | `kLine2` | STOP 按钮描边等稍重的边框 |
| `--tx` | `#EDE6D6` | `kText` | 正文 |
| `--dim` | `#97907E` | `kDim` | 次级文字(用户消息、标签值) |
| `--faint` | `#5F5849` | `kFaint` | 三级文字(占位符、静态标签) |
| `--amber` | `#FFAE1F` | `kAmber` | **唯一强调色**,呼吸点/光标/激活态全用它 |
| `--amber-dim` | `#8A6420` | `kAmberDim` | 弱化琥珀(未激活的 peek 文案等) |
| `--ok` | `#9BC46B` | `kOk` | 工具完成态(呼吸点转绿) |
| `--err` | `#E25B4E` | `kErr` | 错误横幅 |

**新增颜色前先问:这真的是第七种语义,还是能复用已有六色之一?** 原型的设计前提就是
"六色够用",破例需要在这份文档里加一行说明理由。

## 2. 字体

原型规格用了思源黑体 SC 子集(26/30px 正文)+ JetBrains Mono(13/17/132px 数字/等宽)。
实际落地时,出于 flash 预算(§16.5 / R7')**没有新造任何 CJK 字库**,做了如下归并
(见 `pi_fonts.h`):

| 规格里的字号 | 实际用的字体 | 备注 |
|---|---|---|
| assistant 正文 30/36(ZEN 放大) | `font_puhui_30_4`(复用,xiaozhi-fonts 已编入) | 30/36 归并到 30 |
| user 消息 26/22(ZEN 缩小) | `font_puhui_20_4`(复用) | 26/22 归并到 20 |
| tool/label mono 13/14/15/16/17/18/20 | `font_pi_mono_14` / `font_pi_mono_17`(新增,仅 ASCII+`·°`) | 归并成两档 |
| clock mono 132 | `font_pi_clock_132`(新增,仅 `0-9:` 十一字形,bpp2) | 唯一大字号,严格限制字符集控制体积 |

3 个新字体用 `lv_font_conv` 生成(命令模板见文件头注释)。**踩过的坑,务必记住**:

> **`lv_font_conv` 默认输出压缩位图格式(`bitmap_format=1`),必须搭配
> `sdkconfig` 里 `CONFIG_LV_USE_FONT_COMPRESSED=y`,否则真机会在 `draw_letter_cb`
> 里读到压缩字节当原始位图用,产生 `Load access fault` 崩溃(Guru Meditation)。**
> 生成新字体时如果加了 `--no-compress` 就不需要这个开关;两者要匹配,别默认漏掉。
> 这个开关已经在 `sdkconfig.defaults` 里固化(`CONFIG_LV_USE_FONT_COMPRESSED=y`,
> 注释写明原因),新增压缩字体不用再操心,但如果哪天所有字体都换成 `--no-compress`
> 生成,记得这个开关理论上可以关掉省一点点体积——不是必须,只是可以。

规格里的圆点●、方块▊、竖条▮(呼吸点/光标/thinking 图标)**不进字体**,一律用
`lv_obj`(圆形/矩形)+ `lv_anim` 画,原型里也是这个思路(见 HTML 里 `.breath`/
`.msg-a .cursor`/`.think::before` 全是 CSS 图形不是字符)。

## 3. 布局规格(720×720)

| 区域 | 高度 | 说明 |
|---|---|---|
| 状态栏 `sbar` | 56px | 全屏共用,logo + 模型名/REC 状态 + CTX 进度条 + wifi |
| 待机/聆听中部 `mid` | 552px(`kMidH`) | 时钟或声纹+ASR |
| 待机/聆听提示条 `hint` | 112px(`kHintH`) | 底部操作提示 |
| 对话流 `feed` | 552px(`kFeedH`) | flex column,可滚动,内边距 30/32/12 |
| 底部坞 `dock` | 112px(`kDockH`) | TALK/STOP 按钮 + token/耗时统计 |

规格里 S3(工具详情展开)**在实现里不是独立屏**——蓝图判定"实为三视图":点开工具行
是 S2 的 `feed` 里就地展开,不新建 `lv_obj_t*` 屏。`ViewState` 只有三态:
`Idle` / `Listen` / `Chat`(`pi_screen.cc:78`),Chat 视图内部再用 `zen`/`peeking`
标志位表达 ZEN 折叠和临时展开,不是第四个 `ViewState`。

## 4. 对话流(feed)的追加规则 —— 一个真实踩过的坑

`feed` 用 `lv_obj` + `LV_FLEX_FLOW_COLUMN`,新内容**永远只 append,不整屏重建**
(原型 LVGL 落地表里写的 "增量只 append 子对象")。子对象创建顺序 = 视觉顺序,
LVGL 默认 `lv_obj_create(parent)` 把新对象加到最后一位。

`s_act_line` 是一条**常驻不删除**的"当前活动状态行"(ZEN 模式下显示,FLOW 模式下
隐藏但仍占一个子对象位置),由 `BuildActLine()` 在屏幕刚建好、对话还没开始时创建,
天然是当时的第一个子对象。**它的设计意图是"永远贴着 feed 末尾",代表最新状态**,
但代码不会自动帮你维持这个不变量——`lv_obj_create` 不知道你想要 act_line 排在最后,
你得在每次往 feed 里插入新内容时手动把它挪回真正的末尾。

之前的实现反过来做了(把新造的 thinking 行/工具卡 `move_to_index` 到 `s_act_line`
**当时**的 index),而 `s_act_line` 自己从来没被挪动过、永远停在建屏时的 index 0,
结果就是**每一轮新内容都被插到了整个 feed 的最顶端,排到了用户消息前面**——一个
真实上线过的 bug(2026-07-11 修复)。正确写法、也是以后加新的"追加到 feed 末尾,
但要顶着 act_line"逻辑时必须遵守的规则:

```cpp
// 新建的行/卡此时已经是 s_feed 的最后一个子对象(lv_obj_create 的默认行为)。
lv_obj_t* row = CreateXxxRow(s_feed);
// 把 act_line 重新钉回真正的末尾,而不是把新对象挪到 act_line 的旧位置。
lv_obj_move_to_index(s_act_line, lv_obj_get_child_count(s_feed) - 1);
```

**规则**:任何要"贴着 act_line 之前插入"的新增内容,都用上面这个顺序(先建、
再把 act_line 钉回末尾),不要把新对象移到 `lv_obj_get_index(s_act_line)`。
`s_peek_container`(ZEN"查看过程"临时展开容器,`pi_screen.cc:1023`)用的也是
同一个 `lv_obj_get_index(s_act_line)` 写法,目前之所以没触发同样的 bug 是因为
它只在 ZEN 模式且当轮已结束时创建、此时 feed 里没有其他会被推到顶部的活跃内容;
但如果以后 ZEN 模式下也开始追加实时内容,要连带检查这条逻辑要不要一起改成上面
的"钉回末尾"写法。

## 5. 状态机与渲染模式

- **`ViewState`**:`Idle`(S0 待机)→ `Listen`(S1 聆听,假 ASR 逐字回显)→
  `Chat`(S2 生成 / 就地展开的 S3)。三兄弟容器一次建好,`Go(state)` 只做
  `LV_OBJ_FLAG_HIDDEN` 显隐切换,不 delete/rebuild。
- **FLOW / ZEN**(`s_zen` 标志,状态栏点按切换,持久保存):
  - FLOW(默认):thinking 行、工具卡实时创建/追加,`s_act_line` 隐藏。
  - ZEN:thinking/工具卡**根本不创建**(省 RAM 和重绘),只更新 `s_act_line`
    一行文字;"查看过程"点击后才从 `s_tool_cache`/`s_turn_had_thinking`
    临时重建卡片(`OnPeekClicked`),再点收起即删除临时容器。
- 实体键(PWR_KEY)是**按住说话**:按住录音、松开发送、快速轻点无反应;息屏时
  按下仅唤醒;生成中按下即打断。语义靠 IOExpander 的 onPress/onLongPress(按住
  阈值 kKeyHoldToTalkMs)/onRelease 三段边沿实现,配 s_listen_owner(触屏/实体键
  谁拥有本次聆听)与 s_key_ignore_until_release/s_key_finish_pending 两个护栏。
  快捷面板不再由实体键呼出(只留状态栏下拉)。上滑取消、状态栏切模式仍走触屏。
- 触屏 PTT(`s_ptt_layer`/dock TALK 钮)同样是**按住说话**:按下不立即进聆听,按住达
  kTouchHoldToTalkMs(一次性 timer)才进,快速轻点不再"闪一下"进说话界面;松开发送、
  上滑取消。触屏与实体键靠 s_listen_owner 互不抢占。

## 6. pi_ai 事件 → UI 映射

原型的"事件表"是唯一真相,`pi_screen.cc` 里 `DrainQueueTick` 的 `switch` 就是这张
表的直接翻译,别加规格之外的分支。核心映射(完整版见原型 HTML「pi_ai 事件 → UI
反应」表):

| 事件 | UI 反应 |
|---|---|
| `AGENT_START` | 底部坞切 STOP 条,开始计时/token 计数 |
| `THINKING_START/DELTA` | FLOW:出现 `◌ thinking · n.ns`,只计时不渲染正文;ZEN:更新活动线 |
| `TOOL_START` | FLOW:append 工具卡(琥珀呼吸点+函数名);ZEN:只更活动线 |
| `TOOL_ARGS` | 展开态下把 partial_json 流式补进卡片 body |
| `TOOL_END` | 呼吸点转绿(`kOk`),右侧显示 `输出 · 耗时` |
| `TEXT_DELTA` | 正文追加,方块光标(`lv_obj`)常驻行尾,更新 `↓ token` |
| `ERROR` | 红色细线横幅(`kErr`),可点重试,不弹窗 |
| `DONE` | 光标消失,STOP→TALK,CTX 比例更新;ZEN 活动线转绿点摘要 |

## 7. LVGL 实现约定

- **线程模型**:`pi_agent_task.c`(C,独立 FreeRTOS 任务)阻塞跑 agent loop,
  `on_event` 里把事件深拷贝进 `pi_ui_evt_t`,`xQueueSend` 到 `pi_ui_queue()`;
  `pi_screen.cc` 用一个 ~80ms 的 `lv_timer`(`DrainQueueTick`)在 **LVGL 线程**
  排空队列落 widget。**agent 线程永远不碰 widget,LVGL 线程/drain 内永远不用
  加 `esp_lv_adapter_lock`**(锁只用于非 LVGL 线程改 UI 的场景,drain 本身已经
  在 LVGL 线程里,加锁反而会自锁死锁)。
- **文本攒批**:一次 drain tick 内所有 `UI_TEXT_DELTA` 先拼接再一次
  `lv_label_ins_text`,不要逐条 delta 都触发一次重排。
- **字体声明**:统一走 `pi_fonts.h` 的 `LV_FONT_DECLARE`,新屏幕/新组件要用这几
  个字体直接 `#include "pi_fonts.h"`,不要在别处重复 `LV_FONT_DECLARE`。
- **CMakeLists**:新增源文件/字体只在 `main/CMakeLists.txt` 的 `SOURCES`/
  `INCLUDE_DIRS` 里加一次,不要为了图省事在别的地方再 `file(GLOB ...)` 一遍——
  这个目录下的文件是显式列出来的,和整个工程"屏幕源码逐个手加"的约定一致。
- **Flash 预算**:app 分区剩余量用 `idf.py size` 盯着,新增字体/资源前后各跑
  一次对比,别等到接近上限才发现。

## 8. 给后来者的检查清单

往这个 App 加新东西之前,过一遍:

1. 视觉改动:先改 `pi-terminal.html`(或对应设计稿),跑起来看一眼,再翻译成
   `pi_screen.cc`。不要跳过原型直接在 LVGL 里试颜色/尺寸。
2. 新颜色:先看能不能用现有六色之一;真要加,更新第 1 节的表格。
3. 新字体:确认 `lv_font_conv` 的压缩/非压缩选项和 `CONFIG_LV_USE_FONT_COMPRESSED`
   是否匹配(见第 2 节);生成后跑一次 `idf.py size` 看净增量是否在预算内。
4. 往 `s_feed` 追加内容:确认追加顺序不会把新内容插到 `s_act_line` 之前的用户
   消息或历史内容前面(见第 4 节的"钉回末尾"规则)。
5. 跨线程:任何从 `pi_agent_task.c`(或别的非 LVGL 线程)发起的 UI 变化,必须
   经过 `pi_ui_bridge.h` 的队列,不能直接碰 `lv_obj_t*`。
6. 改完跑 `idf.py build`,通过后再跑一次 `idf.py size` 确认 app 分区余量,
   有条件的话烧到真机看一眼(尤其是新增/改动字体渲染路径,压缩格式的坑不会在
   编译期报错,只有真机跑起来才会崩)。
