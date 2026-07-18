// pi_card_media.cc — 见头文件。媒体播放器的 AI 集成层（Stage B）。

#include "pi_card_media.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"

#include "media_player/media_id3.h"
#include "media_player/media_player.h"
#include "media_player/radio_stations.h"
#include "metalio_hal/storage.h"

#include "pi_card_cmd.h"
#include "pi_card_data.h"
#include "pi_media.h"

namespace {

constexpr char TAG[] = "pi_card_media";

using media::MediaController;
using media::MediaItem;
using media::MediaState;

constexpr size_t kMaxItems = 50;      // search 返回上限（防爆 token / 内存）
constexpr size_t kMaxPlaylist = 50;   // 一次 play 的曲目上限

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

bool HasMp3Ext(const char* name) {
    size_t n = std::strlen(name);
    if (n < 4) return false;
    const char* e = name + n - 4;
    return e[0] == '.' && LowerAscii(e[1]) == 'm' && LowerAscii(e[2]) == 'p' && e[3] == '3';
}

std::string BaseNoExt(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

std::string ParentDirName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    std::string parent = path.substr(0, slash);
    size_t slash2 = parent.find_last_of('/');
    return (slash2 == std::string::npos) ? parent : parent.substr(slash2 + 1);
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

// ---- 本地扫描 --------------------------------------------------------------

struct LocalTrack {
    std::string title;
    std::string album;
    std::string path;
};

// 用 ID3v2 标签覆盖标题/副信息（Stage E）：有 TIT2 用作 title，否则保留调用方
// 已填的文件名兜底；TALB/TPE1 组成 "专辑 · 艺人"（缺一个就显示另一个），都缺
// 保留调用方已填的目录名兜底。只读 tag 头几 KB（media_id3::ReadTags 内部
// 流式解析，不载入整曲），50 个文件量级 <1s（Stage E 实测见工作包报告）。
void ApplyId3Meta(std::string& title, std::string& subtitle, const std::string& path) {
    media_id3::Tags t = media_id3::ReadTags(path);
    if (!t.title.empty()) title = t.title;
    if (!t.album.empty() && !t.artist.empty()) {
        subtitle = t.album + " \xc2\xb7 " + t.artist;  // "专辑 · 艺人"
    } else if (!t.album.empty()) {
        subtitle = t.album;
    } else if (!t.artist.empty()) {
        subtitle = t.artist;
    }
}

// 递归收集 dir 下的 .mp3；album = 该文件所在的直接父目录名（顶层文件用 category）。
void ScanInto(const std::string& dir, const std::string& album, std::vector<LocalTrack>& out) {
    if (out.size() >= kMaxItems) return;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;  // 目录不存在/无权限：跳过
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && out.size() < kMaxItems) {
        const char* name = ent->d_name;
        if (name[0] == '.') continue;  // 跳过 "." ".." 及隐藏项
        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ScanInto(full, name, out);  // 子目录名即专辑名
        } else if (S_ISREG(st.st_mode) && HasMp3Ext(name)) {
            std::string title = BaseNoExt(name);
            std::string sub = album;
            ApplyId3Meta(title, sub, full);
            out.push_back({title, sub, full});
        }
    }
    closedir(d);
}

// ---- 三个 mode ------------------------------------------------------------

char* RunSearch(const cJSON* args, bool* is_error) {
    const char* query = GetStr(args, "query");
    const std::string root = mhal::storage::GetMountPoint();
    std::vector<LocalTrack> all;
    ScanInto(root + "/Music", "Music", all);
    ScanInto(root + "/Podcasts", "Podcasts", all);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "kind", "local");
    cJSON* items = cJSON_AddArrayToObject(out, "items");
    int idx = 0;
    for (const LocalTrack& t : all) {
        if (query != nullptr && !CaseInStr(t.title, query) && !CaseInStr(t.album, query)) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", idx++);
        cJSON_AddStringToObject(o, "title", t.title.c_str());
        cJSON_AddStringToObject(o, "album", t.album.c_str());
        cJSON_AddStringToObject(o, "path", t.path.c_str());
        cJSON_AddItemToArray(items, o);
    }
    cJSON_AddStringToObject(out, "hint",
                            idx > 0 ? "to play, call media mode:'play' with paths:[the file paths above]"
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
    for (size_t i = 0; i < media::kRadioStationCount; i++) {
        const media::RadioStation& s = media::kRadioStations[i];
        if (query != nullptr && !CaseInStr(s.name, query) && !CaseInStr(s.genre, query)) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", static_cast<double>(i));
        cJSON_AddStringToObject(o, "name", s.name);
        cJSON_AddStringToObject(o, "genre", s.genre);
        cJSON_AddItemToArray(stations, o);
    }
    cJSON_AddStringToObject(out, "hint",
                            "to play, call media mode:'play' with station_indices:[the index above]");
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
    mc.StagePlaylist(items, start);  // 后台线程起播；本调用即返回

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "kind", kind);
    cJSON_AddNumberToObject(out, "count", static_cast<double>(items.size()));
    cJSON_AddNumberToObject(out, "index", start);
    cJSON_AddStringToObject(out, "state", StateName(mc.state()));  // 多半是 loading，异步转 playing
    if (start >= 0 && start < static_cast<int>(items.size())) {
        cJSON_AddStringToObject(out, "title", items[start].title.c_str());
        cJSON_AddStringToObject(out, "subtitle", items[start].subtitle.c_str());
    }
    // tracks：AI 可原样塞进 ui_render 的 data.tracks，让 list bind_data:'tracks' 渲染整份列表。
    cJSON* tracks = cJSON_AddArrayToObject(out, "tracks");
    for (const MediaItem& it : items) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "title", it.title.c_str());
        cJSON_AddStringToObject(o, "subtitle", it.subtitle.c_str());
        cJSON_AddItemToArray(tracks, o);
    }
    cJSON_AddStringToObject(out, "hint",
                            "now render a control card with ui_render (bind media.title/media.state/"
                            "media.progress_pct; list rows set media.play_index; buttons "
                            "{icon:'skip-back'|'play'|'skip-forward'} invoke media.prev/toggle/next)");
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (printed == nullptr) return Fail(is_error, "OOM");
    return printed;
}

char* RunPlay(const cJSON* args, bool* is_error) {
    const cJSON* jstations = cJSON_GetObjectItemCaseSensitive(args, "station_indices");
    const cJSON* jpaths = cJSON_GetObjectItemCaseSensitive(args, "paths");

    if (cJSON_IsArray(jstations) && cJSON_GetArraySize(jstations) > 0) {
        std::vector<MediaItem> items;
        const cJSON* el = nullptr;
        cJSON_ArrayForEach(el, jstations) {
            if (items.size() >= kMaxPlaylist) break;
            if (!cJSON_IsNumber(el)) continue;
            int i = static_cast<int>(el->valuedouble);
            if (i < 0 || i >= static_cast<int>(media::kRadioStationCount)) continue;
            const media::RadioStation& s = media::kRadioStations[i];
            MediaItem m;
            m.title = s.name;
            m.subtitle = s.genre;
            m.path_or_url = s.url;
            m.is_stream = true;
            m.duration_s = 0;
            items.push_back(std::move(m));
        }
        if (items.empty()) return Fail(is_error, "no valid station_indices (0-based into the radio list)");
        return BuildPlayResult(is_error, items, 0, "radio");
    }

    if (cJSON_IsArray(jpaths) && cJSON_GetArraySize(jpaths) > 0) {
        std::vector<MediaItem> items;
        const cJSON* el = nullptr;
        cJSON_ArrayForEach(el, jpaths) {
            if (items.size() >= kMaxPlaylist) break;
            if (!cJSON_IsString(el) || el->valuestring[0] == '\0') continue;
            MediaItem m;
            m.title = BaseNoExt(el->valuestring);
            m.subtitle = ParentDirName(el->valuestring);
            ApplyId3Meta(m.title, m.subtitle, el->valuestring);  // ID3 覆盖（Stage E）
            m.path_or_url = el->valuestring;
            m.is_stream = false;
            // Xing/VBRI 帧数或 CBR 码率估算（同一文件已开过一次读 tag，这里再开一次只
            // 读首帧头几 KB）；0 = 未知，UI 显示 --:--。
            m.duration_s = media_id3::ProbeDurationS(el->valuestring);
            items.push_back(std::move(m));
        }
        if (items.empty()) return Fail(is_error, "paths is empty or has no valid file path");
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
// toggle/next/prev/stop 直接转发 MediaController（同 RegisterCommands 里的 invoke 命令）；
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

    MediaState st = mc.state();
    if (st == MediaState::Stopped || st == MediaState::Error) {
        *is_error = true;
        return DupString("{\"error\":\"nothing playing\"}");
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

// ---------------------------------------------------------------------------
// media.* DataHub 路径 + invoke 命令（LVGL 线程，Create 期）。MediaController 快照读全部
// 加锁拷贝、非阻塞、任意线程安全 → getter 既供 Acquire 首帧种子，也供 1Hz PublishLive 刷新
// （只读路径），无需自建 timer / SetOnState 编组。
// ---------------------------------------------------------------------------
namespace pi_card_media {

void RegisterDataPaths() {
    static bool done = false;  // Init() 会被多处调用；DataHub::Register 本就幂等，这里只省日志噪音
    if (done) return;
    done = true;
    using pi_card::DataHub;
    using pi_card::HubType;
    using pi_card::HubValue;
    using pi_card::WorkerRead;
    auto& hub = DataHub::Instance();
    auto& mc = MediaController::Instance();

    hub.Register("media.state", HubType::String,
                 [&mc]() -> HubValue { return std::string(StateName(mc.state())); }, nullptr,
                 WorkerRead::Safe);
    hub.Register("media.title", HubType::String,
                 [&mc]() -> HubValue { return mc.current().title; }, nullptr, WorkerRead::Safe);
    hub.Register("media.subtitle", HubType::String,
                 [&mc]() -> HubValue { return mc.current().subtitle; }, nullptr, WorkerRead::Safe);
    hub.Register("media.position_s", HubType::Int,
                 [&mc]() -> HubValue { return mc.position_s(); }, nullptr, WorkerRead::Safe);
    hub.Register("media.duration_s", HubType::Int,
                 [&mc]() -> HubValue { return mc.duration_s(); }, nullptr, WorkerRead::Safe);
    hub.Register("media.progress_pct", HubType::Int,
                 [&mc]() -> HubValue {
                     int dur = mc.duration_s();
                     if (dur <= 0) return 0;  // 未知/直播：恒 0
                     int pct = mc.position_s() * 100 / dur;
                     return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
                 },
                 nullptr, WorkerRead::Safe, 0, 100);
    hub.Register("media.playing", HubType::Bool,
                 [&mc]() -> HubValue { return mc.state() == MediaState::Playing; }, nullptr,
                 WorkerRead::Safe);
    hub.Register("media.index", HubType::Int, [&mc]() -> HubValue { return mc.index(); }, nullptr,
                 WorkerRead::Safe);

    // 可写：跳到指定曲目。getter 回读当前索引，setter 调 PlayIndex（越界由 controller 忽略）。
    // 量程 0-99：让它进入 ui_render 的「可写路径」清单（可被 list 行 set 引用/被发现），
    // 钳制无害——播放列表本就 ≤50 曲。
    hub.Register("media.play_index", HubType::Int, [&mc]() -> HubValue { return mc.index(); },
                 [&mc](const HubValue& v) { mc.PlayIndex(std::get<int>(v)); }, WorkerRead::Safe, 0,
                 99);

    ESP_LOGI(TAG, "media.* data paths registered");
}

void RegisterCommands() {
    static bool done = false;  // 同上：CommandRegistry::Register 幂等，这里只省日志噪音
    if (done) return;
    done = true;
    using pi_card::CmdLevel;
    using pi_card::CommandRegistry;
    auto& reg = CommandRegistry::Instance();
    auto& mc = MediaController::Instance();

    // 预算超标后精简 desc 措辞（命令语义不变，只删口水词：full=true 的 COMMANDS 子句才含
    // desc，false 变体只列名，故这里的每字节都进了 ui_render DESC 的预算）。
    reg.Register("media.toggle", "play/pause", CmdLevel::Safe, [&mc]() { mc.Toggle(); });
    reg.Register("media.next", "next", CmdLevel::Safe, [&mc]() { mc.Next(); });
    reg.Register("media.prev", "prev", CmdLevel::Safe, [&mc]() { mc.Prev(); });
    reg.Register("media.stop", "stop playback", CmdLevel::Safe, [&mc]() { mc.Stop(); });
    // media.open：推出全屏 Now-Playing 页（Stage C）。invoke 在 LVGL 线程执行，直接调
    // pi_media::Open()（用 CreateMiniBar 记住的 parent，重复调用 no-op）。
    reg.Register("media.open", "open full-screen player", CmdLevel::Safe,
                 []() { pi_media::Open(); });

    ESP_LOGI(TAG, "media.* invoke commands registered");
}

}  // namespace pi_card_media
