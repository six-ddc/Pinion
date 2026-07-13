#ifndef PI_CARD_TOOLS_H
#define PI_CARD_TOOLS_H

/* pi_card 声明式 UI —— pi-c 工具桥（C 可包含）。
 *
 * pi_agent_task.c（C）把下面三个工具注册进 agent 的 TOOLS[]：LLM 调用时，工具
 * execute 在 agent worker 线程调下面的 pi_card_tool_*（C++ 实现于 pi_card_host.cc）
 * 做**同步校验**（不碰 LVGL，错误同步回给 LLM 重试），校验过再把 spec 入
 * pi_ui_queue()，由 pi_screen 的 DrainQueueTick（LVGL 线程）按流式顺序真正建控件。
 *
 * 描述/schema 是编译期常量（TOOLS[] 静态初始化需要），故用宏而非 static const。 */

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

/* ---- 喂给 LLM 的工具描述与 JSON-Schema（编译期常量）---- */
#define PI_CARD_RENDER_DESC                                                                          \
    "Render an interactive UI card into the chat conversation. The arguments ARE the card spec: "    \
    "{display?:'chat'|'overlay', ttl_ms?:int, card?:'id', root:<node>}. Returns {\"card\":\"<id>\"}"  \
    "; on invalid input returns an error to fix and retry. display chat(default)=inline card in "    \
    "the chat feed; overlay=floating panel over the screen (a close button is added; ttl_ms>0 "      \
    "auto-closes). Node={\"type\":..}. Types: column|row{children:[]}, label{text,role?,bind?,fmt?}, "\
    "button{text,variant?,on_click}, slider{min,max,value,bind?,on_change?,on_release?}, "            \
    "switch{checked,bind?,on_change?}, bar{min,max,value,bind?}, icon{icon:'name',size?}, divider, "  \
    "spacer. Keep overlays to a few at once (a small cap applies); over-cap renders are reported "     \
    "back as an async error. DESIGN — lean on these, don't hand-style: label role sets a designed type ramp " \
    "(eyebrow=tiny spaced mono kicker, section=group label, title, heading, label=secondary, "        \
    "value=mono number, caption). button variant: primary(the ONE amber call-to-action) | "           \
    "ghost(outlined, for secondary/cancel) | plain(text-only). Give a card a header (eyebrow + "      \
    "title) and group rows; use exactly one primary button. Amber is precious — the theme already "   \
    "puts it on slider fills and the on-switch, so DON'T also color titles/most buttons amber; keep " \
    "text tx/dim. Common props: id,w,h,grow,pad,gap,tone,fill,hidden. tone(text) and fill(bg) take a " \
    "SEMANTIC token that auto-adapts to light/dark — PREFER over raw #hex: accent|ok|err|tx|dim|"     \
    "faint|card|card2|line. Two-way bind paths — WRITABLE (bind a slider/switch to control hardware " \
    "directly, no action needed; values are clamped to each path's valid range): audio.volume(0-100),"\
    " display.brightness(5-100), display.sleep_s(screen-off seconds, 0=never) via slider; "            \
    "ui.theme(0=dark/1=light), speech.tts(0/1 read-aloud) via switch. READ-ONLY: battery.level, "      \
    "battery.charging, net.type, net.rssi, net.ssid, net.connected. "                                  \
    "A label with bind shows the live value (fmt like \"%d%%\"; use mono:true for numbers). Events " \
    "are action arrays: {do:'close'} | {do:'set',path,value?} | {do:'report',text:'..{v}..'} "       \
    "({v}=this widget's value; report tells you what the user chose — put it on buttons/switches, "  \
    "NOT on hardware-bound sliders). Icon names: volume|mute|sun|battery|charging|wifi|cellular|"    \
    "check|close|plus|minus|gear|chevron|info|warning|clock|dot. Limits: 64 nodes, depth 8. Layout " \
    "is adaptive (a row shares width across buttons, a column fills width) — you rarely need w/h. "   \
    "Example: {\"root\":{\"type\":\"column\",\"gap\":14,\"children\":[{\"type\":\"row\",\"children\":"\
    "[{\"type\":\"icon\",\"icon\":\"volume\"},{\"type\":\"slider\",\"bind\":\"audio.volume\"},"       \
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"audio.volume\",\"fmt\":\"%d%%\"}]}]}}"

#define PI_CARD_RENDER_SCHEMA                                                                        \
    "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"object\"},"                            \
    "\"display\":{\"type\":\"string\",\"enum\":[\"chat\",\"overlay\"]},"                              \
    "\"ttl_ms\":{\"type\":\"number\"},\"card\":{\"type\":\"string\"}},\"required\":[\"root\"]}"

#define PI_CARD_UPDATE_DESC                                                                          \
    "Patch a node inside a rendered card. Args {card?:'' (latest), id:'node-id', "                   \
    "props:{text?,value?,checked?,hidden?,tone?}}. The node must have been given an \"id\" at "      \
    "render time. If the card or node is gone (closed, TTL-expired, or a new conversation cleared "  \
    "all cards) the failure is reported back to you asynchronously, not via this call's return."

#define PI_CARD_UPDATE_SCHEMA                                                                        \
    "{\"type\":\"object\",\"properties\":{\"card\":{\"type\":\"string\"},"                            \
    "\"id\":{\"type\":\"string\"},\"props\":{\"type\":\"object\"}},\"required\":[\"id\",\"props\"]}"

#define PI_CARD_CLOSE_DESC "Close a rendered card. Args {card?:'' (latest)}."

#define PI_CARD_CLOSE_SCHEMA "{\"type\":\"object\",\"properties\":{\"card\":{\"type\":\"string\"}}}"

#ifdef __cplusplus
}
#endif
#endif /* PI_CARD_TOOLS_H */
