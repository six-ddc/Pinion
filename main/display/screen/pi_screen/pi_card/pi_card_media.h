#ifndef PI_CARD_MEDIA_H
#define PI_CARD_MEDIA_H

// ---------------------------------------------------------------------------
// pi_card_media —— 媒体播放器的 AI 集成层（Stage B）
//
// 两半：
//   1) media 工具（C 桥，pi_agent_task.c 注册进 TOOLS[]）——LLM 用它搜 SD 卡本地
//      音乐、列内置电台、起播。worker 线程同步执行：扫盘/建表在几十 ms 内返回，
//      真正的解码/播放跑在 MediaController 的后台线程（本工具只 StagePlaylist 后即回）。
//      绝不碰 LVGL。返回串 malloc，调用方 free（与 stock_tool.h 同契约）。
//   2) media.* DataHub 只读/可写路径 + media.* invoke 命令（C++，pi_card_host::Init 调）。
//      播放态经 MediaController 快照 getter 暴露，DataHub 1Hz PublishLive 自动刷新绑定
//      控件；控制动作（toggle/next/prev/stop）走本地 invoke 命令，零回程 LLM。
//
// 契约同 stock_tool.h + pi_card_stock.h：C 部分 extern "C"，C++ 部分 __cplusplus 保护。
// ---------------------------------------------------------------------------

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// media 工具主体（agent worker 线程）。mode 分派 search / radio / play / control。
char *pi_media_tool_run(const cJSON *args, bool *is_error);

// 预算超标后精简（保留 mode 语义/search·radio·play·control 参数形状/接力引导本身，只删口水词）。
#define PI_MEDIA_TOOL_DESC                                                                            \
    "Play SD-card music/podcasts, or live radio. mode:'search' scans SD (Music/Podcasts "            \
    "subdirs=albums/shows) -> {items:[{index,title,album,path}]} (query=fuzzy filter); mode:'radio' " \
    "lists built-in CN stations -> {stations:[{index,name,genre}]} (query filters); mode:'play' "     \
    "starts playback: paths:[\"...\"] (from search, +start_index) OR station_indices:[int,...] (from " \
    "radio list; pass several stations, requested one first, so next/prev can switch channels) -> "   \
    "now-playing snapshot; during a spoken reply play/resume/next/prev are queued (result "           \
    "queued:true) and auto-apply when speech ends — announce as upcoming, not done. The device "      \
    "shows its own player UI automatically — never ui_render a media/player card and never "          \
    "reference media.* paths in a card. "                                                              \
    "mode:'control' with action runs pause/resume/next/prev/stop/open immediately, no search+play "   \
    "round-trip — use it for spoken pause/continue/next/prev/stop/open-player requests. "              \
    "Never poll."

#define PI_MEDIA_TOOL_SCHEMA                                                                         \
    "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"search\","       \
    "\"radio\",\"play\",\"control\"],\"description\":\"search local mp3 | list radio | start "        \
    "playback | pause/resume/next/prev/stop/open now\"},"                                             \
    "\"query\":{\"type\":\"string\",\"description\":\"fuzzy name filter for search/radio\"},"         \
    "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"local file "     \
    "paths (from search) to play\"},\"start_index\":{\"type\":\"number\",\"description\":\"index "    \
    "into paths to start at (default 0)\"},\"station_indices\":{\"type\":\"array\",\"items\":"        \
    "{\"type\":\"number\"},\"description\":\"radio station indices (from the radio list) to play; "   \
    "pass multiple for channel switching\"},"                                                          \
    "\"action\":{\"type\":\"string\",\"enum\":[\"toggle\",\"pause\",\"resume\",\"next\",\"prev\","    \
    "\"stop\",\"open\"],\"description\":\"for mode:'control' — what to do to the current playback\"}"  \
    "},\"required\":[\"mode\"]}"

#ifdef __cplusplus
}  // extern "C"

namespace pi_card_media {

// spec 树里是否引用了 media.*（bind/invoke/path 等任意字符串值以 "media." 开头）——
// 播放器卡片已整体删除（播控只走内置播放器 UI），host 与 preview 据此在正式渲染与
// 流式预览两级拦掉 LLM 不听话画出的媒体卡。
bool SpecUsesMedia(const cJSON* spec_root);

// tracks 兜底注入：spec 有 list bind_data:'tracks' 而 data 里 tracks 缺失/为空时，从
// MediaController 当前播放列表直接构造（格式与 play 工具返回的 tracks 一致），不再依赖
// LLM 把工具结果复制进 ui_render data。列表为空（没在播）则不动 data。
void MaybeFillTracks(const cJSON* spec_root, cJSON* data);

}  // namespace pi_card_media

#endif  // __cplusplus
#endif  // PI_CARD_MEDIA_H
