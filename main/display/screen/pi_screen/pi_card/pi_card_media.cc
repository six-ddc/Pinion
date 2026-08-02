// pi_card_media.cc — 见头文件。媒体播放器的 AI 集成层（Stage B）。

#include "pi_card_media.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"

#include "device_config.h"  // 运行时网络电台列表（Web 后台可配，NVS 覆盖 or 内置种子）
#include "media_player/media_player.h"

#include "pi_media.h"
#include "pi_media_focus.h"    // 回合中起播排队（TTS 优先）
#include "pi_media_library.h"  // 共享曲库扫描/排序/ID3 缓存
#include "pi_ui_bridge.h"      // pi_agent_task_is_running / pi_agent_tts_enabled

namespace {

constexpr char TAG[] = "pi_card_media";

using media::MediaController;
using media::MediaItem;
using media::MediaState;

constexpr size_t kMaxItems = 50;      // search 返回上限（防爆 token；库全量见 total 字段）
constexpr size_t kMaxPlaylist = 50;   // 一次 play 显式点名队列的曲目上限（全库扩列不受此限）
constexpr size_t kTracksForLlm = 40;  // play 返回给模型的 tracks 条数上限（防爆上下文）

// ---- 小工具 ----------------------------------------------------------------

char* DupString(const std::string& s) {
    char* out = static_cast<char*>(malloc(s.size() + 1));
    if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

char* Fail(bool* is_error, const std::string& msg) {
    *is_error = true;
    return DupString(msg);
}

const char* GetStr(const cJSON* n, const char* k) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(n, k);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : nullptr;
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// 大小写不敏感子串（仅折叠 ASCII 字母；中文按字节原样比较——UTF-8 下等价于精确匹配，
// 用户输入的中文片段能命中标题里的同一串字节，够用）。
bool CaseInStr(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        for (; j < needle.size(); j++) {
            if (LowerAscii(hay[i + j]) != LowerAscii(needle[j])) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

const char* StateName(MediaState s) {
    switch (s) {
        case MediaState::Loading: return "loading";
        case MediaState::Playing: return "playing";
        case MediaState::Paused: return "paused";
        case MediaState::Error: return "error";
        default: return "stopped";
    }
}

// ---- 三个 mode ------------------------------------------------------------

char* RunSearch(const cJSON* args, bool* is_error) {
    const char* query = GetStr(args, "query");
    // 全库扫描（目录序）+ 逐首补 ID3 供按真歌名匹配——首次全量读 tag 慢（几百首数秒，
    // worker 线程、LLM 工具往返本身秒级），此后进程内缓存秒回。
    uint32_t t0 = lv_tick_get();
    std::vector<MediaItem> all = pi_media_library::ScanLibrary();
    for (MediaItem& it : all) pi_media_library::ApplyId3(it);
    ESP_LOGI(TAG, "search: %d tracks scanned+id3 in %ums", (int)all.size(),
             (unsigned)(lv_tick_get() - t0));

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "kind", "local");
    cJSON* items = cJSON_AddArrayToObject(out, "items");
    int matched = 0;
    int shown = 0;
    for (const MediaItem& t : all) {
        if (query != nullptr && !CaseInStr(t.title, query) && !CaseInStr(t.subtitle, query)) continue;
        matched++;
        if ((size_t)shown >= kMaxItems) continue;  // 只截输出，total 仍如实计数
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", shown++);
        cJSON_AddStringToObject(o, "title", t.title.c_str());
        cJSON_AddStringToObject(o, "album", t.subtitle.c_str());
        cJSON_AddStringToObject(o, "path", t.path_or_url.c_str());
        cJSON_AddItemToArray(items, o);
    }
    cJSON_AddNumberToObject(out, "total", matched);  // 匹配总数（items 只给前 kMaxItems 条）
    cJSON_AddStringToObject(out, "hint",
                            matched > 0
                                ? "to play, call media mode:'play' with paths:[ONE file path] — the "
                                  "device queues the WHOLE library from that track (folder order); "
                                  "pass several paths only for an explicit custom queue"
                                : "no mp3 found under Music/ or Podcasts/ on the SD card");
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (printed == nullptr) return Fail(is_error, "OOM");
    return printed;
}

char* RunRadio(const cJSON* args, bool* is_error) {
    const char* query = GetStr(args, "query");
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "kind", "radio");
    cJSON* stations = cJSON_AddArrayToObject(out, "stations");
    const std::vector<device_config::RadioStation>& list = device_config::RadioStations();
    for (size_t i = 0; i < list.size(); i++) {
        const device_config::RadioStation& s = list[i];
        if (query != nullptr && !CaseInStr(s.name, query) && !CaseInStr(s.genre, query)) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", static_cast<double>(i));
        cJSON_AddStringToObject(o, "name", s.name.c_str());
        cJSON_AddStringToObject(o, "genre", s.genre.c_str());
        cJSON_AddItemToArray(stations, o);
    }
    cJSON_AddStringToObject(out, "hint",
                            "to play, call media mode:'play' with station_indices:[...] — include "
                            "multiple stations (requested one first) so the user can switch channels "
                            "with next/prev");
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (printed == nullptr) return Fail(is_error, "OOM");
    return printed;
}

// play 落地：把 items 交给 MediaController，返回 now-playing 快照 + tracks（供 AI 喂给
// ui_render 的 list 数据）。kind: "local" | "radio"。
char* BuildPlayResult(bool* is_error, const std::vector<MediaItem>& items, int start,
                      const char* kind) {
    MediaController& mc = MediaController::Instance();
    // 回合进行中且 TTS 开着：不立即起播——起播的 FlushPlayback 会清掉已缓冲的 TTS 并
    // 推进播放代次，把正在/即将播报的回复掐成整段无声。改为只暂存列表，把起播意图排给
    // pi_media_focus，播报排空后（tts_ended/turn_ended 的去抖检查）自动开播。TTS 关闭时
    // 没有可保护的播报，立即起播才是对的（否则平白等到回合结束）。
    const bool queued = pi_agent_task_is_running() && pi_agent_tts_enabled();
    if (queued) {
        mc.StagePlaylist(items, -1);  // start<0：只暂存列表，不 Teardown/不 Flush/不起播
        pi_media_focus_queue_play(start);
    } else {
        mc.StagePlaylist(items, start);  // 后台线程起播；本调用即返回
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "kind", kind);
    cJSON_AddNumberToObject(out, "count", static_cast<double>(items.size()));
    cJSON_AddNumberToObject(out, "index", start);
    cJSON_AddStringToObject(out, "state", queued ? "queued" : StateName(mc.state()));
    if (queued) {
        cJSON_AddBoolToObject(out, "queued", true);
        cJSON_AddStringToObject(out, "note",
                                "playback auto-starts right after your spoken reply finishes — phrase "
                                "it as about to play, not already playing");
    }
    if (start >= 0 && start < static_cast<int>(items.size())) {
        cJSON_AddStringToObject(out, "title", items[start].title.c_str());
        cJSON_AddStringToObject(out, "subtitle", items[start].subtitle.c_str());
    }
    // tracks：AI 可原样塞进 ui_render 的 data.tracks，让 list bind_data:'tracks' 渲染列表。
    // 全库扩列后必须封顶（几百条 JSON 会吃满模型上下文）：从起播曲开始最多 kTracksForLlm
    // 条；count 字段已是全量，截断时补 tracks_note 说明。
    size_t lo = (start >= 0 && start < static_cast<int>(items.size())) ? (size_t)start : 0;
    cJSON* tracks = cJSON_AddArrayToObject(out, "tracks");
    for (size_t k = lo; k < items.size() && k - lo < kTracksForLlm; k++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "title", items[k].title.c_str());
        cJSON_AddStringToObject(o, "subtitle", items[k].subtitle.c_str());
        cJSON_AddItemToArray(tracks, o);
    }
    if (items.size() > kTracksForLlm) {
        cJSON_AddStringToObject(out, "tracks_note",
                                "tracks lists only the next 40 from the playing index; count is the "
                                "full queue length");
    }
    // 播放器卡片已整体删除（设备内置播放器 UI 自动出现），明确告知别渲染卡片、别引用
    // media.* 路径（渲染层同步硬拦，见 pi_card_host / pi_card_preview）。
    cJSON_AddStringToObject(out, "hint",
                            "the device's built-in player UI is already showing — do NOT ui_render "
                            "any media/player card and do NOT reference media.* paths; just confirm "
                            "in text");
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (printed == nullptr) return Fail(is_error, "OOM");
    return printed;
}

char* RunPlay(const cJSON* args, bool* is_error) {
    const cJSON* jstations = cJSON_GetObjectItemCaseSensitive(args, "station_indices");
    const cJSON* jpaths = cJSON_GetObjectItemCaseSensitive(args, "paths");

    if (cJSON_IsArray(jstations) && cJSON_GetArraySize(jstations) > 0) {
        const std::vector<device_config::RadioStation>& list = device_config::RadioStations();
        auto station_item = [&list](int i) {
            const device_config::RadioStation& s = list[i];
            MediaItem m;
            m.title = s.name;
            m.subtitle = s.genre;
            m.path_or_url = s.url;
            m.is_stream = true;
            m.duration_s = 0;
            return m;
        };
        std::vector<int> idxs;
        const cJSON* el = nullptr;
        cJSON_ArrayForEach(el, jstations) {
            if (idxs.size() >= kMaxPlaylist) break;
            if (!cJSON_IsNumber(el)) continue;
            int i = static_cast<int>(el->valuedouble);
            if (i < 0 || i >= static_cast<int>(list.size())) continue;
            idxs.push_back(i);
        }
        if (idxs.empty()) return Fail(is_error, "no valid station_indices (0-based into the radio list)");
        std::vector<MediaItem> items;
        // 单台兜底：只点一个台时把播放列表扩成全部电台（自然序）、从所点台起播——
        // 保证 Next/Prev 永远有台可切（收音机换台语义），不受模型是否传多台影响。
        if (idxs.size() == 1) {
            for (size_t i = 0; i < list.size() && items.size() < kMaxPlaylist; i++) {
                items.push_back(station_item(static_cast<int>(i)));
            }
            return BuildPlayResult(is_error, items, idxs[0], "radio");
        }
        for (int i : idxs) items.push_back(station_item(i));
        return BuildPlayResult(is_error, items, 0, "radio");
    }

    if (cJSON_IsArray(jpaths) && cJSON_GetArraySize(jpaths) > 0) {
        std::vector<std::string> req;
        const cJSON* el = nullptr;
        cJSON_ArrayForEach(el, jpaths) {
            if (req.size() >= kMaxPlaylist) break;
            if (!cJSON_IsString(el) || el->valuestring[0] == '\0') continue;
            req.push_back(el->valuestring);
        }
        if (req.empty()) return Fail(is_error, "paths is empty or has no valid file path");
        // 单曲兜底：镜像电台"单台扩全台"——列表扩成全曲库（目录序）、从所点曲起播，
        // Next/Prev 永远有歌可切且跨目录连播。路径陈旧（不在库里）退回单曲列表。
        if (req.size() == 1) {
            std::vector<MediaItem> lib = pi_media_library::ScanLibrary();
            int at = pi_media_library::IndexOfPath(lib, req[0]);
            if (at >= 0) {
                pi_media_library::ApplyId3(lib[at]);  // 起播曲补真名（模型要在回复里报）
                return BuildPlayResult(is_error, lib, at, "local");
            }
        }
        // 显式点名队列（模型给的顺序即队列）：逐首补 ID3+时长（≤kMaxPlaylist，秒内）。
        std::vector<MediaItem> items;
        for (const std::string& p : req) {
            MediaItem m;
            m.title = pi_media_library::BaseNoExt(p);
            m.subtitle = pi_media_library::ParentDirName(p);
            m.path_or_url = p;
            m.is_stream = false;
            pi_media_library::ApplyId3(m);
            items.push_back(std::move(m));
        }
        int start = 0;
        const cJSON* jstart = cJSON_GetObjectItemCaseSensitive(args, "start_index");
        if (cJSON_IsNumber(jstart)) start = static_cast<int>(jstart->valuedouble);
        if (start < 0) start = 0;
        if (start >= static_cast<int>(items.size())) start = 0;
        return BuildPlayResult(is_error, items, start, "local");
    }

    return Fail(is_error, "play needs paths:[...] (local files from search) or station_indices:[...]");
}

// control：语音里的"暂停/继续/下一首/上一首/停/打开播放页"直接落地，不走 search+play 接力。
// toggle/next/prev/stop 直接转发 MediaController；
// pause/resume 该控制器没有独立方法，只有 Toggle（Playing<->Paused），故按当前状态判断
// 幂等语义：已是目标态就什么都不做（成功返回原状态），否则调 Toggle。open 不受 Stopped
// 门槛限制（全屏播放页任何时候都能开，Stage C 语义）；其余 action 在 Stopped/Error（无东西
// 可控）下返回明确错误 JSON，不去碰 Toggle/Next 等——它们在空列表上本就是安全 no-op，但
// 那样会让 AI 误判"已执行"，不如显式告知"当前没有在播的东西"。
char* RunControl(const cJSON* args, bool* is_error) {
    const char* action = GetStr(args, "action");
    if (action == nullptr) {
        return Fail(is_error, "control needs action: toggle|pause|resume|next|prev|stop|open");
    }
    MediaController& mc = MediaController::Instance();

    if (std::strcmp(action, "open") == 0) {
        pi_media::Open();
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "state", StateName(mc.state()));
        char* printed = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        return printed != nullptr ? printed : Fail(is_error, "OOM");
    }

    // stop/pause 先作废排队中的起播意图——必须在下面的 Stopped 早退**之前**做：意图
    // 排队期间 state 就是 Stopped，"叫停"若只落进 nothing-playing 分支，音乐会在回合
    // 结束时反而响起。
    if (std::strcmp(action, "stop") == 0 || std::strcmp(action, "pause") == 0) {
        pi_media_focus_clear_queued_play();
    }

    MediaState st = mc.state();
    if (st == MediaState::Stopped || st == MediaState::Error) {
        *is_error = true;
        return DupString("{\"error\":\"nothing playing\"}");
    }

    // 回合中且 TTS 开着：会**起播/换曲**的动作（resume/next/prev、等效 resume 的
    // toggle-on-Paused）不立即执行——它们都经 StartPump 的 FlushPlayback，立即执行会把
    // 正在播报的 TTS 掐成无声（"好的，下一首"话音未落）。排给 pi_media_focus，播报排空
    // 后执行。pause/stop 保持立即：挂起态无泵，Teardown 早退不 Flush，对 TTS 无害。
    if (pi_agent_task_is_running() && pi_agent_tts_enabled()) {
        int defer = -1;
        if (std::strcmp(action, "next") == 0) {
            defer = PI_MEDIA_QUEUE_NEXT;
        } else if (std::strcmp(action, "prev") == 0) {
            defer = PI_MEDIA_QUEUE_PREV;
        } else if (st == MediaState::Paused && (std::strcmp(action, "resume") == 0 ||
                                                std::strcmp(action, "toggle") == 0)) {
            defer = PI_MEDIA_QUEUE_RESUME;
        }
        if (defer != -1) {
            pi_media_focus_queue_play(defer);
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON_AddBoolToObject(out, "queued", true);
            cJSON_AddStringToObject(out, "state", "queued");
            cJSON_AddStringToObject(out, "note",
                                    "applies right after your spoken reply finishes — announce as "
                                    "upcoming, not done");
            char* printed = cJSON_PrintUnformatted(out);
            cJSON_Delete(out);
            return printed != nullptr ? printed : Fail(is_error, "OOM");
        }
    }

    if (std::strcmp(action, "toggle") == 0) {
        mc.Toggle();
    } else if (std::strcmp(action, "pause") == 0) {
        if (st == MediaState::Playing) mc.Toggle();  // 已暂停/加载中：无害 no-op
    } else if (std::strcmp(action, "resume") == 0) {
        if (st == MediaState::Paused) mc.Toggle();  // 已在播：无害 no-op
    } else if (std::strcmp(action, "next") == 0) {
        mc.Next();
    } else if (std::strcmp(action, "prev") == 0) {
        mc.Prev();
    } else if (std::strcmp(action, "stop") == 0) {
        mc.Stop();
    } else {
        return Fail(is_error, "unknown action (use toggle|pause|resume|next|prev|stop|open)");
    }

    MediaItem cur = mc.current();
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "state", StateName(mc.state()));
    cJSON_AddNumberToObject(out, "index", mc.index());
    if (!cur.title.empty()) cJSON_AddStringToObject(out, "title", cur.title.c_str());
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (printed == nullptr) return Fail(is_error, "OOM");
    return printed;
}

}  // namespace

extern "C" char* pi_media_tool_run(const cJSON* args, bool* is_error) {
    *is_error = false;
    const char* mode = GetStr(args, "mode");
    if (mode == nullptr) return Fail(is_error, "give mode: 'search' | 'radio' | 'play' | 'control'");
    if (std::strcmp(mode, "search") == 0) return RunSearch(args, is_error);
    if (std::strcmp(mode, "radio") == 0) return RunRadio(args, is_error);
    if (std::strcmp(mode, "play") == 0) return RunPlay(args, is_error);
    if (std::strcmp(mode, "control") == 0) return RunControl(args, is_error);
    return Fail(is_error, "unknown mode (use 'search' | 'radio' | 'play' | 'control')");
}

namespace pi_card_media {

// 递归扫 spec：任意字符串值以 "media." 开头即视为媒体卡。media.* 数据路径/invoke 命令
// 已随媒体卡整体删除（播控只走内置播放器 UI），此扫描只剩一个用途：pi_card_host /
// pi_card_preview 在渲染与流式预览两级拦掉 LLM 不听话画出的播放器卡。误报只可能来自
// LLM 写的字面文案恰好以 media. 开头，可接受。
bool SpecUsesMedia(const cJSON* node) {
    if (node == nullptr) return false;
    if (cJSON_IsString(node)) {
        const char* v = cJSON_GetStringValue(node);
        return v != nullptr && std::strncmp(v, "media.", 6) == 0;
    }
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        for (const cJSON* child = node->child; child != nullptr; child = child->next) {
            if (SpecUsesMedia(child)) return true;
        }
    }
    return false;
}

namespace {

// spec 里是否存在 bind_data == key 的节点（list 数据绑定声明）。
bool SpecUsesBindData(const cJSON* node, const char* key) {
    if (node == nullptr) return false;
    if (cJSON_IsObject(node)) {
        const cJSON* bd = cJSON_GetObjectItemCaseSensitive(node, "bind_data");
        const char* v = cJSON_GetStringValue(bd);
        if (v != nullptr && std::strcmp(v, key) == 0) return true;
    }
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        for (const cJSON* child = node->child; child != nullptr; child = child->next) {
            if (SpecUsesBindData(child, key)) return true;
        }
    }
    return false;
}

}  // namespace

void MaybeFillTracks(const cJSON* spec_root, cJSON* data) {
    if (spec_root == nullptr || data == nullptr) return;
    const cJSON* existing = cJSON_GetObjectItemCaseSensitive(data, "tracks");
    if (cJSON_IsArray(existing) && cJSON_GetArraySize(existing) > 0) return;  // LLM 带了就尊重
    if (!SpecUsesBindData(spec_root, "tracks")) return;
    MediaController& mc = MediaController::Instance();
    const int n = mc.playlist_size();
    if (n <= 0) return;  // 没在播/没列表：留给 list 的空态处理（无 empty 文案则整体收起）
    if (existing != nullptr) cJSON_DeleteItemFromObjectCaseSensitive(data, "tracks");
    cJSON* tracks = cJSON_AddArrayToObject(data, "tracks");
    if (tracks == nullptr) return;
    // 窗口化：全库列表几百条会撑爆节点预算前的 data 内存——从当前曲起最多 kTracksForLlm
    // 条（与 play 工具返回的 tracks 同窗口语义）。
    const int cap = static_cast<int>(kTracksForLlm);
    int lo = mc.index();
    if (lo < 0 || lo >= n) lo = 0;
    int filled = 0;
    for (int i = lo; i < n && filled < cap; i++, filled++) {
        const MediaItem it = mc.item_at(i);
        cJSON* o = cJSON_CreateObject();
        if (o == nullptr) break;
        cJSON_AddStringToObject(o, "title", it.title.c_str());
        cJSON_AddStringToObject(o, "subtitle", it.subtitle.c_str());
        cJSON_AddItemToArray(tracks, o);
    }
    ESP_LOGI(TAG, "tracks fallback filled: %d rows from MediaController (from idx %d)", filled, lo);
}

}  // namespace pi_card_media
