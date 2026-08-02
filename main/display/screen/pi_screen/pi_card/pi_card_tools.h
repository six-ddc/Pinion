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

/* ---- 喂给 LLM 的工具描述与参数 schema（编译期常量）---- */

/* ui_render 参数 schema：只声明裸 {"xml":string}，零内联约束零 required——pi-c 解析 $ref 且
 * 强制一切内联约束（CARD_XML.md §10.1），任何 required/type 收紧都是"干拒绝+截断 echo→弱
 * 模型原样重试"的硬拒面。root/display 等键只为内部/测试通道（sim 语料、脚手架直喂编译后
 * spec）留门，不在提示词/描述里教。 */
#define PI_CARD_RENDER_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"xml\":{\"type\":\"string\"},\"root\":{\"type\":\"array\"" \
    "},\"display\":{\"type\":\"string\"},\"ttl_ms\":{\"type\":\"number\"},\"card\":{\"type\":\"strin" \
    "g\"},\"data\":{\"type\":\"object\"}}}"

/* PI_CARD_DESC_HEAD + <运行时路径清单> + PI_CARD_DESC_TAIL = ui_render 完整描述。拆两段是
 * 因为路径清单必须插在「双向 bind 目标」与「fmt/事件/限额补充」两段之间。线格式 = HTML 式
 * XML（docs/CARD_XML.md §2）：词表/属性/动作微语法一次讲全——示例在 system prompt。 */
#define PI_CARD_DESC_HEAD                                                                             \
    "Render an interactive UI card. Args: {\"xml\":\"<card>…</card>\"} — one HTML-like XML string; "  \
    "the device compiles and lays out everything (you never write x/y/w/h/columns/CSS). Returns "     \
    "{\"card\":\"<id>\",\"state\":{<path>:<value>},\"hints\":[…]}: state = every hardware path this " \
    "card binds (this IS your device read); hints = advice for FUTURE cards — the card IS on "        \
    "screen, never re-render because of a hint. Invalid input returns a fixable error; overlays "     \
    "auto-close (ttl) and are capped. "                                                               \
    "<card display='chat|overlay|standby' ttl='30s' id?> stacks BLOCK elements top-to-bottom "        \
    "(each takes fill?/id?/hidden?; toggling a block id folds a section): "                           \
    "<grid>leaves</grid> (a flow wrapped by size — headers, control rows like icon+slider+"           \
    "value, button groups; divider/chart/choice/qrcode take their own line); "                        \
    "<table cols='项,值:num'><tr><td>…</td>…</tr>…</table> (aligned TABLE sharing column tracks; "    \
    "':num' = number column, right-aligned mono; cols optional — auto-inferred; a <td> holds text "   \
    "or ONE leaf; one <td> per row = vertical menu); "                                                \
    "<list bind='key' max? empty?>row-template leaves</list> (repeats the template once per "         \
    "element of data 'key'; {i}=idx0,{n}=idx1,{item.FIELD} in strings; row tap = tap on a template "  \
    "leaf, report/set/close only); "                                                                  \
    "<divider/>; "                                                                                    \
    "<data> card data: scalar <temp>24</temp>; list rows by repeating <tracks title='七里香'/>. "     \
    "LEAVES (12, exactly these, self-closing unless text content): "                                  \
    "<label text? role? bind? fmt? mono? bind_data?/> or <label>text</label>; "                       \
    "<button text? icon? variant? tap?>text</button>; <slider min? max? value? bind? id? change? "    \
    "release?/>; <arc like slider/>(round dial); <switch checked? bind? id? change?/>; "              \
    "<bar min? max? value? bind?/>; <choice options='a|b|c'(2-6) value? id? bind? change?/>"          \
    "(picker; reports idx+label); <icon name='wifi'/>(decor; tappable=<button icon>); "               \
    "<qrcode text?/>; <chart bind='path' points?/>(LINE chart, fixed height); "                       \
    "<stock_chart symbol name? mode?/>(live CN/HK/US chart, self-refreshing, timeframe buttons, "     \
    "hold-to-inspect; symbol from stock tool; mode min|5d|day|week). "                                \
    "role ramp: eyebrow|kicker|section|title|heading|label|value|caption (header=eyebrow+title; "     \
    "big number=value). variant: primary(ONE amber CTA)|ghost|plain|default. Common attrs: id, "      \
    "bind, tone, hidden, side='end' (push cell to the row's right edge); mono/hidden/checked are "    \
    "bare booleans. Grid fill=bg box. tone/fill: semantic token (auto light/dark): accent|"           \
    "accent_dim|ok|err|tx|dim|faint|card|card2|line|line2|bg — never hex. "

#define PI_CARD_DESC_TAIL                                                                             \
    "Bound label fmt: ONE placeholder matching the type — number->%d/'%d%%', string->%s (%s on a "    \
    "number path crashes); mono for numbers. bind_data label shows card data[key] ({value} in its "   \
    "text inlines it). "                                                                              \
    "EVENTS: tap(click)/change/release attrs = comma-separated steps 'verb:payload', zero "           \
    "round-trip — close | set:path=value ('{i}' in a row template) | toggle:id / show:id / hide:id "  \
    "(a hidden leaf) | invoke:cmd (safe cmds run at once, else confirm). report:text only when you "  \
    "must generate text or a NEW decision (quote a payload holding a comma: report:'a, b'; every "    \
    "id'd control's value auto-attaches, choice as idx(label)). "                                     \
    "Escape & as &amp; in attributes. "                                                               \
    "DATA: ui_update (JSON args, unchanged) mutates card data (set/append/remove/replace); bound "    \
    "list/bind_data re-render. Icons: Lucide names (wifi|battery|play|check|x…); unknown → dot. "     \
    "Limits: 64 leaves, >8 blocks->first 8 render, so prefer 3-5 blocks/card & split big "            \
    "dashboards. Layout auto: grid wraps by size; table aligns to shared tracks. Off-vocabulary "     \
    "tags/attrs tolerated but noted. See system prompt for CHOOSE tree+examples."

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
