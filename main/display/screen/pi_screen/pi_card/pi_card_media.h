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

// media 工具主体（agent worker 线程）。mode 分派 search / radio / play。
char *pi_media_tool_run(const cJSON *args, bool *is_error);

#define PI_MEDIA_TOOL_DESC                                                                            \
    "Play music/podcasts from the SD card, or live radio. mode:'search' scans the SD card "          \
    "(dirs under Music/ and Podcasts/ are albums/shows; returns {items:[{index,title,album,path}]}, " \
    "query filters by fuzzy name); mode:'radio' lists built-in CN radio stations "                    \
    "({stations:[{index,name,genre}]}, query filters); mode:'play' starts playback — give paths:"    \
    "[\"...\"] (local file paths from a prior search) with optional start_index, OR station_indices:" \
    "[int] (indices from the radio list). Returns a now-playing snapshot. After a successful play, "  \
    "FOLLOW with ui_render to show a control card, e.g. root=column of: a label bind:'media.title' "  \
    "(str) + a label bind:'media.state', a bar bind:'media.progress_pct' (0-100), a row of buttons "  \
    "invoke media.prev / media.toggle / media.next, and a list bind_data:'tracks' whose rows tap "    \
    "{do:'set',path:'media.play_index',value:'{i}'} to jump to that track. Never poll — the card "    \
    "auto-refreshes from media.* paths."

#define PI_MEDIA_TOOL_SCHEMA                                                                         \
    "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"search\","       \
    "\"radio\",\"play\"],\"description\":\"search local mp3 | list radio | start playback\"},"        \
    "\"query\":{\"type\":\"string\",\"description\":\"fuzzy name filter for search/radio\"},"         \
    "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"local file "     \
    "paths (from search) to play\"},\"start_index\":{\"type\":\"number\",\"description\":\"index "    \
    "into paths to start at (default 0)\"},\"station_indices\":{\"type\":\"array\",\"items\":"        \
    "{\"type\":\"number\"},\"description\":\"radio station indices (from the radio list) to play\"}}" \
    ",\"required\":[\"mode\"]}"

#ifdef __cplusplus
}  // extern "C"

namespace pi_card_media {

// 注册 media.* DataHub 路径（只读播放态 + 可写 media.play_index）。pi_card::Init 调，幂等。
void RegisterDataPaths();

// 注册 media.* invoke 命令（toggle/next/prev/stop/open，全 Safe）。pi_card::Init 调，幂等。
void RegisterCommands();

}  // namespace pi_card_media

#endif  // __cplusplus
#endif  // PI_CARD_MEDIA_H
