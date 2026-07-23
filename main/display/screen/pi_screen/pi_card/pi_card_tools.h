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
    "Render an interactive UI card. Args ARE the card spec: {display?:'chat'|"                        \
    "'overlay'|'standby', ttl_ms?:int, card?:'id', data?:{key:scalar|array}, root:[grid,…]}. root: "  \
    "ARRAY of grid blocks, top-to-bottom. ONLY container is grid (no nesting; depth fixed "           \
    "card->grid->leaf); no x/y/w/h/gaps/grow/justify/align. Returns "                                 \
    "{\"card\":\"<id>\",\"state\":{<path>:<value>},\"hints\":[…]}: state = every hardware path this " \
    "card binds (this IS your device read); hints = non-blocking tips. "                              \
    "Invalid input returns a fixable error; overlays auto-close (ttl_ms) and are capped. "            \
    "Each grid has exactly one of: "                                                                  \
    "cells:[leaf,…] (size-wrapped flow; divider/chart/choice/qrcode get their own line); "            \
    "cols?:[{title?,num?}],rows:[[leaf,…],…] (aligned TABLE, shared tracks; num:true=right-align "    \
    "mono; 1 cell/row=vertical menu); "                                                               \
    "item:<leaf>|[leaf,…],bind_rows:'key',max?,empty? (repeats item per data[key] elem; "             \
    "{i}=idx0,{n}=idx1,{item.FIELD}; row taps: report/set/close only). "                               \
    "Leaves (12, exactly these, never nested): label{text?,role?,bind?,fmt?,mono?,bind_data?}; "      \
    "button{text?,icon?,variant?,on_click}; slider{min,max,value,bind?,id?,on_change?,on_release?}; " \
    "arc{like slider}(round dial); switch{checked,bind?,id?,on_change?}; bar{min,max,value,bind?}; "  \
    "choice{options:[2-6],value?,id?,bind?,on_change?}(picker; reports idx+label); "                   \
    "icon{icon:'name'}; divider; qrcode{text}; chart{bind_history:'path',points?}(LINE chart, fixed " \
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
    "EVENTS: action arrays, zero round-trip — close | set,path,value? | toggle/show/hide,target "     \
    "(hidden:true block) | patch,target,props:{text?,value?,checked?,hidden?,tone?} ({v} in "         \
    "props.text=trigger value) | invoke,cmd (safe cmds run at once, else confirm). report,text only " \
    "when you must generate text ({v}=value,{label}=choice's text; every id'd control's value "      \
    "auto-attaches, choice as idx(label)). "                                                          \
    "DATA: ui_update mutates card data (set/append/remove/replace); bound bind_rows/bind_data "        \
    "re-render. Icons: Lucide names (wifi|battery|play|check|x…); unknown → dot. "                     \
    "Limits: 64 nodes (bind_rows reserves max×row-leaf-count), 8 grids/card. Layout auto (no "        \
    "coordinates): cells wraps by size; rows aligns to shared tracks (num=right-align,text=truncate); " \
    "bind_rows repeats template per element. See system prompt for CHOOSE tree+example."

#define PI_CARD_RENDER_SCHEMA \
    "{\"type\":\"object\",\"$defs\":{\"action\":{\"type\":\"object\",\"properties\":{\"do\":{\"type\"" \
    ":\"string\",\"enum\":[\"close\",\"set\",\"report\",\"toggle\",\"show\",\"hide\",\"patch\",\"invo" \
    "ke\"]},\"pa" \
    "th\":{\"type\":\"string\"},\"value\":{\"type\":\"number\"},\"text\":{\"type\":\"string\"},\"targ" \
    "et\":{\"type\":\"string\"},\"props\":{\"type\":\"object\"},\"cmd\":{\"type\":\"string\"}},\"req" \
    "uired\":[\"do\"]}," \
    "\"leaf\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"label\"" \
    ",\"button\",\"slider\",\"arc\",\"switch\",\"bar\",\"icon\",\"divider\",\"qrcode\",\"choice\",\"c" \
    "hart\",\"stock_chart\"]},\"symbol\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"mode\"" \
    ":{\"type\":\"string\",\"enum\":[\"min\",\"5d\",\"day\",\"week\"]},\"text\":{\"type\":\"string\"" \
    "},\"role\":{\"type\":\"string\",\"enum\":[\"eyebrow\",\"kicker\",\"section\",\"title\",\"headin" \
    "g\",\"label\",\"value\",\"caption\"]},\"variant\":{\"type\":\"string\",\"enum\":[\"primary\",\"g" \
    "host\",\"plain\",\"default\"]},\"bind\":{\"type\":\"string\"},\"bind_data\":{\"type\":\"string\"" \
    "},\"bind_history\":{\"type\":\"string\"},\"points\":{\"type\":\"number\"},\"empty\":{\"type\":\"" \
    "string\"},\"fmt\":{\"type\":\"string\"},\"icon\":{\"type\":\"string\"},\"value\":{\"type\":\"nu" \
    "mber\"},\"min\":{\"type\":\"number\"},\"max\":{\"type\":\"number\"},\"checked\":{\"type\":\"boo" \
    "lean\"},\"options\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":2,\"maxIte" \
    "ms\":6},\"mono\":{\"type\":\"boolean\"},\"tone\":{\"type\":\"string\",\"enum\":[\"accent\",\"acc" \
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
    "\"rows\":{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/leaf\"" \
    "}}}," \
    "\"item\":{\"oneOf\":[{\"$ref\":\"#/$defs/leaf\"},{\"type\":\"array\",\"items\":{\"$ref\":\"#/$d" \
    "efs/leaf\"}}]}," \
    "\"bind_rows\":{\"type\":\"string\"},\"max\":{\"type\":\"number\"},\"empty\":{\"type\":\"string\"" \
    "}," \
    "\"fill\":{\"type\":\"string\",\"enum\":[\"accent\",\"accent_dim\",\"ok\",\"err\",\"tx\",\"dim\"" \
    ",\"faint\",\"card\",\"card2\",\"line\",\"line2\",\"bg\"]}" \
    "}}}," \
    "\"properties\":{\"root\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/grid\"},\"minItems\"" \
    ":1,\"maxItems\":8},\"display\":{\"type\":\"string\",\"enum\":[\"chat\",\"overlay\",\"standby\"]" \
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
    ":\"string\"}}}},\"properties\":{\"card\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"pr" \
    "ops\":{\"$ref\":\"#/$defs/props\"},\"patches\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"" \
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
