// media_player — 媒体播放核心（SD 卡 MP3 + 网络电台 MP3/HLS-AAC 流共用一条可移植管线）。
//
// 管线：MP3/AAC-ADTS 字节源 → MediaDecoder 解码（按 track_codec 分派：真机 esp_audio_codec
// 官方库 MP3+AAC；sim 端 minimp3(MP3)+AudioToolbox(AAC)——helix-aac 在 64 位宿主有 UB 已弃）
// → stereo 降混 + 定点线性重采样到 16kHz mono → mhal::audio_pipeline::FeedPlayback。核心
// device/sim 同源（C++17 线程 + POSIX 文件），只有 HTTP 流字节源分双端实现
// （media_http_esp.cc / media_http_curl.cc）。
//
// 与 TTS 共用同一条 FeedPlayback 播放管线（2MB PSRAM 抖动队列，EnsurePlayback 幂等）：
// media 播放时占用该队列；TTS 要发声时通过 SuspendForSpeech/ResumeFromSpeech 让路。
//
// 线程模型：MediaController 是单例，对外方法全部线程安全（内部一把 mutex 串行化控制、
// 快照读拷贝）。真正的读/解码/喂入跑在 pump 的两条后台线程上；状态或曲目变化时从**内部
// 线程**回调 SetOnState 注册的函数——回调方自己负责编组回 LVGL 线程（本组件不碰 UI）。
//
// 本阶段（Stage A）只提供播放核心与 API 契约，不接任何 UI；SuspendForSpeech/
// ResumeFromSpeech 的语义已实现，接线（TTS 触发）由 Stage D 完成。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace media {

// 播放器状态机。Loading = 已下命令、正在打开源/首帧解码前；Playing = 正在出声；
// Paused = 用户暂停或为 TTS 让路挂起（位置已记）；Error = 打开/读取失败。
enum class MediaState { Stopped, Loading, Playing, Paused, Error };

// 播放列表条目。path_or_url：文件绝对路径或流 URL；is_stream=true 表示无限电台流；
// duration_s：曲目秒长，0 表示未知或直播（LIVE）。title/subtitle 供 UI 展示。
// meta_filled：title/subtitle/duration_s 已由 ID3 回填（或构建方确认无需回填）——
// false 的文件条目在起播时由 controller 惰性补读（全库列表扫描期不逐首开文件）。
struct MediaItem {
    std::string title;
    std::string subtitle;
    std::string path_or_url;
    bool is_stream = false;
    int duration_s = 0;  // 0 = 未知或 LIVE
    bool meta_filled = false;
};

// 用 ID3 标签回填条目显示信息：TIT2→title、TALB/TPE1→subtitle（"专辑 · 艺人"，缺一显
// 一）、探测时长→duration_s；tag 缺失的字段保留调用方已填的文件名/目录名兜底。完成后
// 置 meta_filled。流条目 no-op。同步读文件头几 KB（毫秒级），勿在 UI 线程对整列表循环调。
void FillItemMetaFromId3(MediaItem& item);

class MediaController {
 public:
    // 进程级单例。
    static MediaController& Instance();

    // 载入播放列表（替换现有列表）。start_index >= 0 则立即从该曲起播；< 0 只暂存不播
    //（保持当前 Stopped/播放态不变）。索引越界按边界钳制/忽略。
    void StagePlaylist(std::vector<MediaItem> items, int start_index = -1);

    // 播放指定索引（越界忽略）。会干净停掉当前播放再起新曲。
    void PlayIndex(int index);

    // 播放/暂停切换：Playing→Paused（记位，文件可续播），Paused/Stopped→Playing（续播/起播）。
    void Toggle();

    // 停止：停线程、清播放队列、位置归零、状态→Stopped（保留播放列表与当前索引）。
    void Stop();

    // 下一曲 / 上一曲（到边界后钳制在首/尾；空列表忽略）。
    void Next();
    void Prev();

    // TTS 让路：挂起 media 播放交出 FeedPlayback 队列。文件记录播放位置以便续播；
    // 流保持"稍后重连回直播边缘"的语义（不记位，Resume 时重新连流拿最新音频）。
    void SuspendForSpeech();
    // 从 TTS 让路中恢复：文件从记录位置续播，流重连回直播边缘。若 Suspend 时非播放态则无操作。
    void ResumeFromSpeech();

    // —— 快照读接口（全部加锁拷贝，可从任意线程调用）——
    MediaState state();
    int index();          // 当前曲索引，-1 = 无
    int position_s();     // 当前曲已播放秒数（由已喂样本数/16000 计）
    int duration_s();     // 当前曲总秒数（0 = 未知/LIVE）
    MediaItem current();  // 当前曲拷贝（无当前曲返回空 item）
    int playlist_size();
    MediaItem item_at(int index);  // 越界返回空 item

    // 注册状态/曲目变化回调。**从内部后台线程调用**——回调方必须自己编组回 LVGL 线程
    //（如投递到 LVGL 异步队列）后再触碰任何 UI。传 nullptr 注销。同一时刻只保留一个回调。
    void SetOnState(std::function<void()> cb);

 private:
    MediaController();
    ~MediaController();
    MediaController(const MediaController&) = delete;
    MediaController& operator=(const MediaController&) = delete;

    struct Impl;
    Impl* impl_;
};

}  // namespace media
