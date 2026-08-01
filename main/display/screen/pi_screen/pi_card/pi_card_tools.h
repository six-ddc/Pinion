#ifndef PI_CARD_TOOLS_H
#define PI_CARD_TOOLS_H

/* pi_card 声明式 UI —— pi-c 工具桥（C 可包含）。
 *
 * pi_agent_task.c（C）把下面三个工具注册进 agent 的 TOOLS[]：LLM 调用时，工具
 * execute 在 agent worker 线程调下面的 pi_card_tool_*（C++ 实现于 pi_card_host.cc）
 * 做**同步校验**（不碰 LVGL，错误同步回给 LLM 重试），校验过再把 spec 入
 * pi_ui_queue()，由 pi_screen 的 DrainQueueTick（LVGL 线程）按流式顺序真正建控件。
 *
 * ui_render 的描述是**运行时动态生成**的（pi_card_render_desc()，实现见
 * pi_card_host.cc）：静态 HEAD/TAIL 骨架宏之间拼一段由 DataHub::ListPaths() 生成
 * 的「本机实际可绑路径」权威清单，永远与活体注册表同步、不随硬编码示例过时。
 * update/close 的描述仍是纯编译期常量，用宏（TOOLS[] 静态初始化需要）。 */

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 各函数：读 args（已解析的 cJSON），校验通过则入队并返回给 LLM 的结果串
 * （malloc，调用方 free），失败置 *is_error 并返回错误串。worker 线程调用。 */
char *pi_card_tool_render(const cJSON *args, bool *is_error);
char *pi_card_tool_update(const cJSON *args, bool *is_error);
char *pi_card_tool_close(const cJSON *args, bool *is_error);

/* ui_render 工具描述：编译期静态 HEAD/TAIL 骨架 + 运行时动态拼出的路径清单
 * （pi_card_host.cc 的 pi_card_render_desc()）。pi_agent_task.c 的 TOOLS[] 是
 * static（非 const）数组，在 pi_agent_task_start 里 ensure_env() 之后、
 * create_agent() 之前，把 ui_render 项的 description 由 "" 改填成本函数返回的
 * 指针——该指针指向 function-static std::string，常驻整个固件运行期，满足
 * pi_agent_create 浅拷贝工具结构体、借用 description 指针的契约。 */
const char *pi_card_render_desc(void);

/* system prompt：与 pi_card_render_desc 同款 function-static 缓存 + 常驻指针契约（骨架
 * 固定 + BuildPathsClause(false) 插入活体路径清单，与 ui_render 的 DESC 共用同一份路径清单
 * 生成器，不出现第三份硬编码路径）。create_agent() 里 cfg.system_prompt 借用它；pi-c 深拷贝
 * system_prompt（pi_agent.c:58-60 pi_strdup），故 new_session 重建 agent 时再次借用同一
 * 指针是安全的。实现见 pi_card_host.cc。 */
const char *pi_card_system_prompt(void);

/* ---- 喂给 LLM 的工具描述与 JSON-Schema（编译期常量）---- */

/* PI_CARD_DESC_HEAD + <运行时路径清单> + PI_CARD_DESC_TAIL = ui_render 完整描述（CARD_V2）。
 * 拆两段是因为路径清单必须插在「双向 bind 目标」与「事件/图标/示例」这两段说明
 * 之间——HEAD 收尾在类型清单/设计规范，TAIL 从"路径清单之后的补充说明"接着讲
 * fmt/事件/图标/示例。v2：root 是 grid 块数组，只有一种容器（grid），无嵌套；
 * preset/slots/column/row/旧grid/justify/align/grow/gap/pad/w/h/size/color/bg/span/children
 * 全部删除（docs/CARD_V2.md §1）。 */
#define PI_CARD_DESC_HEAD                                                                            \
    "Render an interactive UI card. Args ARE the card spec: {root:[grid,…], data?:{key:scalar|"       \
    "array}, display?:'chat'|'overlay'|'standby', ttl_ms?:int, card?:'id'} — emit root before "      \
    "data. root: "                                                                                    \
    "ARRAY of grid blocks, top-to-bottom. ONLY container is grid (no nesting; depth fixed "           \
    "card->grid->leaf); no x/y/w/h/gaps/grow/justify/align. Returns "                                 \
    "{\"card\":\"<id>\",\"state\":{<path>:<value>},\"hints\":[…]}: state = every hardware path this " \
    "card binds (this IS your device read); hints = advice for FUTURE cards — the card IS on "        \
    "screen, never re-render because of a hint. "                                                     \
    "Invalid input returns a fixable error; overlays auto-close (ttl_ms) and are capped. "            \
    "Each grid has exactly one of: "                                                                  \
    "cells:[leaf,…] (size-wrapped flow; divider/chart/choice/qrcode get their own line); "            \
    "cols?:[{title?,num?}],rows:[[leaf,…],…] (aligned TABLE, 2-D: each row is an ARRAY of leaves; "   \
    "shared tracks; num:true=right-align mono; 1 cell/row=vertical menu); "                           \
    "item:<leaf>|[leaf,…],bind_rows:'key',max?,empty? (repeats item per data[key] elem; "             \
    "{i}=idx0,{n}=idx1,{item.FIELD}; row tap = on_click on an item leaf (not the grid), "          \
    "report/set/close only). "                                                                     \
    "Leaves (12, exactly these, never nested): label{text?,role?,bind?,fmt?,mono?,bind_data?}; "      \
    "button{text?,icon?,variant?,on_click}; slider{min,max,value,bind?,id?,on_change?,on_release?}; " \
    "arc{like slider}(round dial); switch{checked,bind?,id?,on_change?}; bar{min,max,value,bind?}; "  \
    "choice{options:[2-6],value?,id?,bind?,on_change?}(picker; reports idx+label); "                   \
    "icon{icon:'name'}(decor; tappable=button{icon}); divider; qrcode{text}; "                     \
    "chart{bind_history:'path',points?}(LINE chart, fixed " \
    "height); stock_chart{symbol,name?,mode?}(live CN/HK/US chart, self-refreshing, timeframe "        \
    "buttons, hold-to-inspect; symbol from stock tool). "                                              \
    "role ramp: eyebrow|kicker|section|title|heading|label|value|caption (header=eyebrow+title; "     \
    "big number=value). variant(button): primary(ONE amber CTA)|ghost|plain|default. Common props: "  \
    "id,bind,tone,hidden,side (side:'end'=push cell to row's right edge). Grid fill=bg box. "         \
    "tone/fill: semantic token (auto light/dark): accent|accent_dim|ok|err|tx|dim|faint|card|card2|"  \
    "line|line2|bg — never hex. "

/* PI_CARD_DESC_TAIL：路径清单之后——fmt 安全/事件模型/图标/限额/布局提要。 */
#define PI_CARD_DESC_TAIL                                                                             \
    "Bound label fmt: ONE placeholder matching the type — number->%d/\"%d%%\", string->%s (%s on a "  \
    "number path crashes); mono:true for numbers. bind_data label shows card data[key] ({value} in "  \
    "its text inlines it). "                                                                          \
    "EVENTS: action arrays, zero round-trip — close | set,path,value?(number, or '{i}' in a row "     \
    "template) | toggle/show/hide,target "     \
    "(hidden:true block) | patch,target,props:{text?,value?,checked?,hidden?,tone?} ({v} in "         \
    "props.text=trigger value) | invoke,cmd (safe cmds run at once, else confirm). report,text only " \
    "when you must generate text ({v}=value,{label}=choice's text; every id'd control's value "      \
    "auto-attaches, choice as idx(label)). "                                                          \
    "DATA: ui_update mutates card data (set/append/remove/replace); bound bind_rows/bind_data "        \
    "re-render. Icons: Lucide names (wifi|battery|play|check|x…); unknown → dot. "                     \
    "Limits: 64 nodes (bind_rows reserves max×row-leaf-count); >8 grids->first 8 render, so prefer "  \
    "3-5 grids/card & split big dashboards. Layout auto (no coordinates): cells wraps by size; "      \
    "rows aligns to shared tracks. See system prompt for CHOOSE tree+example."

#define PI_CARD_RENDER_SCHEMA \
    "{\"type\":\"object\",\"$defs\":{\"action\":{\"type\":\"object\",\"properties\":{\"do\":{\"type\"" \
    ":\"string\",\"enum\":[\"close\",\"set\",\"report\",\"toggle\",\"show\",\"hide\",\"patch\",\"invo" \
    "ke\"]},\"pa" \
    /* action.value 允许 string：行模板 set 的 value:"{i}"（渲染期替换成行号）是文档教的合法
     * 用法，schema 只写 number 会误导照 schema 写卡的模型。 */ \
    "th\":{\"type\":\"string\"},\"value\":{\"type\":[\"number\",\"string\"]},\"text\":{\"type\":\"st" \
    "ring\"},\"targ" \
    "et\":{\"type\":\"string\"},\"props\":{\"type\":\"object\"},\"cmd\":{\"type\":\"string\"}},\"req" \
    "uired\":[\"do\"]}," \
    "\"leaf\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"label\"" \
    ",\"button\",\"slider\",\"arc\",\"switch\",\"bar\",\"icon\",\"divider\",\"qrcode\",\"choice\",\"c" \
    "hart\",\"stock_chart\"]},\"symbol\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"mode\"" \
    ":{\"type\":\"string\",\"enum\":[\"min\",\"5d\",\"day\",\"week\"]},\"text\":{\"type\":\"string\"" \
    "},\"role\":{\"type\":\"string\",\"enum\":[\"eyebrow\",\"kicker\",\"section\",\"title\",\"headin" \
    "g\",\"label\",\"value\",\"caption\"]},\"variant\":{\"type\":\"string\",\"enum\":[\"primary\",\"g" \
    "host\",\"plain\",\"default\"]},\"bind\":{\"type\":\"string\"},\"bind_data\":{\"type\":\"string\"" \
    /* "empty" 是 grid（bind_rows 形态）的属性，之前误混进 leaf——已移回 grid 独有。 */ \
    "},\"bind_history\":{\"type\":\"string\"},\"points\":{\"type\":\"number\"}" \
    ",\"fmt\":{\"type\":\"string\"},\"icon\":{\"type\":\"string\"},\"value\":{\"type\":\"nu" \
    "mber\"},\"min\":{\"type\":\"number\"},\"max\":{\"type\":\"number\"},\"checked\":{\"type\":\"boo" \
    /* options 无 minItems/maxItems：rows 一维失败实证 pi-c 解析 $ref 并强制内联约束，7 个
     * 选项会在 pi-c 层吃 "Expected array length" 干拒绝；host 侧有友好的 2-6 校验。 */ \
    "lean\"},\"options\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}" \
    "},\"mono\":{\"type\":\"boolean\"},\"tone\":{\"type\":\"string\",\"enum\":[\"accent\",\"acc" \
    "ent_dim\",\"ok\",\"err\",\"tx\",\"dim\",\"faint\",\"card\",\"card2\",\"line\",\"line2\",\"bg\"]}" \
    ",\"id\":{\"type\":\"string\"},\"side\":{\"type\":\"string\",\"enum\":[\"end\"]},\"hidden\":{\"ty" \
    "pe\":\"boolean\"},\"on_click\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/action\"}},\"on" \
    "_change\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/action\"}},\"on_release\":{\"type\"" \
    ":\"array\",\"items\":{\"$ref\":\"#/$defs/action\"}}},\"required\":[\"type\"]}," \
    "\"col\":{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"num\":{\"type\":" \
    "\"boolean\"}}}," \
    "\"grid\":{\"type\":\"object\",\"properties\":{" \
    "\"cells\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/leaf\"}}," \
    "\"cols\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/col\"}}," \
    /* rows 不写内层 items 约束：真机实录弱模型把 rows 写成一维叶子数组，被 pi-c 的内联
     * type:"array" 硬拒（"Expected array"+截断echo）后原样重试两次——$defs 引用内部 pi-c
     * 不查，但内联约束查。形状修复交给 host Repair（裸叶子包成单格行）。 */ \
    "\"rows\":{\"type\":\"array\"}," \
    "\"item\":{\"oneOf\":[{\"$ref\":\"#/$defs/leaf\"},{\"type\":\"array\",\"items\":{\"$ref\":\"#/$d" \
    "efs/leaf\"}}]}," \
    "\"bind_rows\":{\"type\":\"string\"},\"max\":{\"type\":\"number\"},\"empty\":{\"type\":\"string\"" \
    "}," \
    "\"fill\":{\"type\":\"string\",\"enum\":[\"accent\",\"accent_dim\",\"ok\",\"err\",\"tx\",\"dim\"" \
    ",\"faint\",\"card\",\"card2\",\"line\",\"line2\",\"bg\"]}" \
    "}}}," \
    /* 无 maxItems/minItems：>8 grids 由 host Repair() 截断+hint、空 root 由 Validate 给可修复
     * 错误（宽进严出），pi-c 层硬拒的干错误（"Expected array length…"+全量 args echo）会让
     * 弱模型连环重试越改越错。 */ \
    "\"properties\":{\"root\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/grid\"}" \
    "},\"display\":{\"type\":\"string\",\"enum\":[\"chat\",\"overlay\",\"standby\"]" \
    "},\"ttl_ms\":{\"type\":\"number\",\"minimum\":0},\"card\":{\"type\":\"string\"},\"data\":{\"typ" \
    "e\":\"object\"}},\"required\":[\"root\"]}"

#define PI_CARD_UPDATE_DESC                                                                          \
    "Patch a node inside a rendered card, or mutate card data. Args {card?:'' (latest), id?:"        \
    "'node-id', props?:{text?,value?,checked?,hidden?,tone?,color?}} — give both id+props to patch " \
    "that node (must have been given an \"id\" at render time). Or patch several nodes in one call: " \
    "patches?:[{id,props},…] (up to 16; each id independent — a missing id is reported async but "    \
    "doesn't abort the rest). Or mutate card data: {card?, data:"                                    \
    "{set?:{k:v,…}, append?:{key,item}, remove?:{key,index|id}, replace?:{key,index,item}}} — any "  \
    "list bound via bind_data (or a bind_data label) re-renders from the new data. Give one of "     \
    "(id+props), patches, or data. If the card/node is gone (closed, TTL-expired, or a new "          \
    "conversation cleared all cards) the failure is reported back to you asynchronously, not via "    \
    "this call's return."

#define PI_CARD_UPDATE_SCHEMA \
    "{\"type\":\"object\",\"$defs\":{\"props\":{\"type\":\"object\",\"properties\":{\"text\":{\"type" \
    "\":\"string\"},\"value\":{\"type\":\"number\"},\"checked\":{\"type\":\"boolean\"},\"hidden\":{\"" \
    "type\":\"boolean\"},\"tone\":{\"type\":\"string\",\"enum\":[\"accent\",\"accent_dim\",\"ok\",\"e" \
    "rr\",\"tx\",\"dim\",\"faint\",\"card\",\"card2\",\"line\",\"line2\",\"bg\"]},\"color\":{\"type\"" \
    /* patches 无 maxItems：>16 由 worker 侧 pi_card_tool_update 给可修复错误（同 root 无
     * maxItems 的理由——pi-c 层硬拒会引发弱模型重试螺旋）。 */ \
    ":\"string\"}}}},\"properties\":{\"card\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"pr" \
    "ops\":{\"$ref\":\"#/$defs/props\"},\"patches\":{\"type\":\"array\",\"items\":{\"" \
    "type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"props\":{\"$ref\":\"#/$defs/pr" \
    "ops\"}},\"required\":[\"id\",\"props\"]}},\"data\":{\"type\":\"object\",\"properties\":{\"set\"" \
    ":{\"type\":\"object\"},\"append\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"stri" \
    "ng\"},\"item\":{}},\"required\":[\"key\",\"item\"]},\"remove\":{\"type\":\"object\",\"propertie" \
    "s\":{\"key\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"},\"id\":{\"type\":\"string\"}" \
    "},\"required\":[\"key\"]},\"replace\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"" \
    "string\"},\"index\":{\"type\":\"number\"},\"item\":{}},\"required\":[\"key\",\"index\",\"item\"" \
    "]}}}}}"

#define PI_CARD_CLOSE_DESC "Close a rendered card. Args {card?:'' (latest)}."

#define PI_CARD_CLOSE_SCHEMA "{\"type\":\"object\",\"properties\":{\"card\":{\"type\":\"string\"}}}"

#ifdef __cplusplus
}
#endif
#endif /* PI_CARD_TOOLS_H */
