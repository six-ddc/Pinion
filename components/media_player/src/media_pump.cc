// media_pump — 媒体管线的两条后台线程（reader + decoder），操作 controller 创建的 Pump 上下文。
//
// reader 线程：按 playlist 从 start_index 起逐曲开源（文件/流），阻塞 Read → 压缩字节环；
//   曲末（文件 EOF）与 decoder 握手后推进下一曲（自动连播），播完最后一曲 → OnAllFinished；
//   无限流不 EOF。流式播放用 PlaybackFilled() 做节流阀：已缓冲 PCM > kStreamBufferMaxSec 秒
//   就暂停读，让整条链路自时钟、字节环有界。
// decoder 线程：从字节环取压缩字节 → MediaDecoder（按 track_codec 分派：真机
//   esp_audio_codec 官方库 MP3+AAC；sim 端 minimp3(MP3)+AudioToolbox(AAC)，见
//   media_decoder.h）榨干 → 降混+重采样到 16k mono → FeedPlayback（带 pump 起播代次，
//   打断残音竞态收口，背压落在本线程）。
//
// 参照范式：components/volc_speech/src/volc_tts.cc 的 pump 任务（环形缓冲→解码器榨干→
// FeedPlayback、滑窗 memmove 已消费字节）。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "media_decoder.h"
#include "media_hls.h"
#include "media_internal.h"
#include "media_player/media_http_stream.h"
#include "media_player/media_source.h"
#include "media_resampler.h"
#include "metalio_hal/audio_pipeline.h"

namespace media {

namespace {
const char* TAG = "media_pump";

constexpr uint32_t kFeedTimeoutMs = 10000;  // 抖动队列满时对 decoder 的最大背压时长
constexpr size_t kReaderChunk = 4096;       // 单次 Read 上限
constexpr size_t kDecoderPull = 16384;      // 单次从字节环搬进解码器的上限
constexpr int kDecoderPaceSec = 3;          // 解码喂入不超前播放缓冲这么多秒（自时钟节流阀）

const char* StateName(MediaState s) {
    switch (s) {
        case MediaState::Stopped: return "Stopped";
        case MediaState::Loading: return "Loading";
        case MediaState::Playing: return "Playing";
        case MediaState::Paused: return "Paused";
        case MediaState::Error: return "Error";
    }
    return "?";
}

// —— 主机侧调试：把喂给 FeedPlayback 的 PCM 同步 dump 成 16k mono 16-bit WAV ——
// 仅 sim（非 ESP）编译，由环境变量 PI_SIM_MEDIA_WAV 激活。真机不带这段。
#ifndef ESP_PLATFORM
class WavDump {
 public:
    ~WavDump() { Finalize(); }
    void MaybeOpen() {
        if (f_ != nullptr || tried_) return;
        tried_ = true;
        const char* path = getenv("PI_SIM_MEDIA_WAV");
        if (path == nullptr || path[0] == '\0') return;
        f_ = fopen(path, "wb");
        if (f_ == nullptr) return;
        uint8_t hdr[44] = {0};
        fwrite(hdr, 1, 44, f_);  // 占位头，Finalize 时回填
    }
    void Write(const int16_t* pcm, size_t samples) {
        if (f_ == nullptr || samples == 0) return;
        fwrite(pcm, sizeof(int16_t), samples, f_);
        data_bytes_ += (uint32_t)(samples * sizeof(int16_t));
    }
    void Finalize() {
        if (f_ == nullptr) return;
        const uint32_t sr = 16000, ch = 1, bits = 16;
        const uint32_t byte_rate = sr * ch * bits / 8;
        const uint32_t riff = 36 + data_bytes_;
        uint8_t h[44];
        std::memcpy(h + 0, "RIFF", 4);
        std::memcpy(h + 4, &riff, 4);
        std::memcpy(h + 8, "WAVE", 4);
        std::memcpy(h + 12, "fmt ", 4);
        uint32_t fmt_len = 16;
        std::memcpy(h + 16, &fmt_len, 4);
        uint16_t pcm_fmt = 1, chans = (uint16_t)ch, ba = (uint16_t)(ch * bits / 8), bps = (uint16_t)bits;
        std::memcpy(h + 20, &pcm_fmt, 2);
        std::memcpy(h + 22, &chans, 2);
        std::memcpy(h + 24, &sr, 4);
        std::memcpy(h + 28, &byte_rate, 4);
        std::memcpy(h + 32, &ba, 2);
        std::memcpy(h + 34, &bps, 2);
        std::memcpy(h + 36, "data", 4);
        std::memcpy(h + 40, &data_bytes_, 4);
        fseek(f_, 0, SEEK_SET);
        fwrite(h, 1, 44, f_);
        fclose(f_);
        f_ = nullptr;
    }

 private:
    FILE* f_ = nullptr;
    bool tried_ = false;
    uint32_t data_bytes_ = 0;
};
#endif  // ESP_PLATFORM

}  // namespace

// ============================ reader 线程 ============================

// 阻塞写入字节环：满则等 decoder 腾空间或 stop。更新历史高水位（验证流式节流有界）。
static void RingWrite(Pump* p, const uint8_t* buf, size_t n) {
    size_t off = 0;
    while (off < n && !p->stop) {
        std::unique_lock<std::mutex> lk(p->mu);
        p->cv.wait(lk, [&] { return p->stop.load() || p->ring.size() < kByteRingCap; });
        if (p->stop) return;
        size_t space = kByteRingCap - p->ring.size();
        size_t chunk = std::min(space, n - off);
        p->ring.append(buf + off, chunk);
        size_t hw = p->ring.size();
        lk.unlock();
        if (hw > p->ring_high_water.load()) p->ring_high_water = hw;
        p->cv.notify_all();  // 唤醒 decoder
        off += chunk;
    }
}

static void ReaderRun(Pump* p) {
    const int count = (int)p->playlist.size();
    int idx = p->start_index;
    if (idx < 0 || idx >= count) {
        p->stop = true;
        p->cv.notify_all();
        if (p->host) p->host->OnAllFinished(p);
        return;
    }

    const int sr = mhal::audio_pipeline::SampleRate();
    // 连续（未产出任何数据的）本地文件曲目失败计数：单曲打开/读取失败不该终止整个播放
    // 列表（如 SD 卡里混了一首损坏的 MP3）——跳到下一曲即可；只有连续失败数达到整个
    // playlist 长度（即整轮全败，真出了系统性问题：SD 卡拔出/目录整体不可读）才终止报
    // Error。任一曲成功产出过数据即清零。流式（电台）track 不计入——维持现状读错即
    // Error（那是 60s 重连放弃后的结果，必须让用户知道，不能被"跳下一曲"悄悄吞掉）。
    int consec_fail = 0;
    for (;;) {
        if (p->stop) break;
        const MediaItem item = p->playlist[idx];
        // 转发给字节源：网络流断线重连时切 Loading，恢复数据时切回 Playing。可能从本
        // reader 线程（esp 的 Reconnect 同步跑在这）或字节源自己的后台线程（curl 的
        // bg thread）调用，捕获 p 的裸指针安全——字节源在 reader 线程退出前必被 Close()
        // 并 join，不会有跨越 pump 生命周期的悬挂调用。
        auto on_reconnecting = [p](bool reconnecting) {
            if (p->host) p->host->OnReconnecting(p, reconnecting);
        };
        // 源/编解码三分路由：.m3u8 → HLS 源（TS 解封装吐 ADTS，AAC 解码）；
        // 其余流 → 裸 MP3 无限流；文件 → SD MP3。
        const bool is_hls = item.is_stream && UrlIsHls(item.path_or_url.c_str());
        std::unique_ptr<MediaSource> src =
            is_hls           ? OpenHlsStreamSource(item.path_or_url.c_str(), on_reconnecting)
            : item.is_stream ? OpenHttpStreamSource(item.path_or_url.c_str(), on_reconnecting)
                             : OpenFileSource(item.path_or_url.c_str());
        if (!src) {
            if (item.is_stream) {
                if (p->host) p->host->OnTrackError(p, idx, "open failed");
                break;
            }
            // 本地文件打开失败：本曲从未起播（reader_epoch 未推进），decoder 无需握手，
            // 直接跳下一曲。
            ESP_LOGW(TAG, "track %d open failed, skip", idx);
            if (++consec_fail >= count) {
                if (p->host) p->host->OnTrackError(p, idx, "all tracks failed");
                break;
            }
            idx++;
            if (idx >= count) {
                p->stop = true;
                p->cv.notify_all();
                if (p->host) p->host->OnAllFinished(p);
                return;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(p->mu);
            p->current_source = src.get();  // teardown 路径据此 Abort() 快速打断阻塞的 Read()
        }

        // 起新曲：清 input_done、bump epoch（通知 decoder 复位并接新流）、标记本曲编解码。
        p->cur_index = idx;
        {
            std::lock_guard<std::mutex> lk(p->mu);
            p->input_done = false;
            p->reader_epoch++;
            p->track_codec = is_hls ? MediaCodec::AacAdts : MediaCodec::Mp3;
        }
        p->cv.notify_all();
        if (p->host) p->host->OnTrackStarted(p, idx);

        uint8_t buf[kReaderChunk];
        bool read_err = false;
        bool any_data = false;  // 本曲是否至少成功产出过一次读到的数据
        for (;;) {
            if (p->stop) break;
            // 流式节流阀：已缓冲 PCM 超过阈值就暂停读（PlaybackFilled 字节 /(sr*2) = 秒）。
            while (item.is_stream && !p->stop &&
                   mhal::audio_pipeline::PlaybackFilled() / (size_t)(sr * 2) >
                       (size_t)kStreamBufferMaxSec) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (p->stop) break;
            int r = src->Read(buf, sizeof(buf));
            if (r < 0) {
                read_err = true;
                break;
            }
            if (r == 0) break;  // EOF（无限流不会到这）
            RingWrite(p, buf, (size_t)r);
            any_data = true;
        }
        {
            std::lock_guard<std::mutex> lk(p->mu);
            p->current_source = nullptr;  // 之后 teardown 路径不会再对本已关闭的源调 Abort()
        }
        src->Close();

        if (p->stop) break;
        if (read_err) {
            if (item.is_stream) {
                if (p->host) p->host->OnTrackError(p, idx, "read error");
                break;
            }
            // 本地文件曲中读失败（损坏/SD 抖动）：已产出的数据仍走下面的正常握手交给
            // decoder 榨干，随后跳下一曲；未达连续失败阈值前不终止整场。
            ESP_LOGW(TAG, "track %d read error, skip to next", idx);
        }
        if (!read_err || any_data) {
            consec_fail = 0;  // 成功放完/至少产出过数据，不计入连续失败
        } else if (++consec_fail >= count) {
            // 本曲一字节都没读到就失败，且连续失败已达整个 playlist 长度：真出了系统性
            // 问题，报错收场（本 epoch decoder 还没收到任何数据，无需等握手）。
            if (p->host) p->host->OnTrackError(p, idx, "all tracks failed");
            break;
        }

        // 文件 EOF（或非流失败已跳过）：标记 input_done，等 decoder 把本曲字节全部解码
        // 搬完（握手）。
        int my_epoch;
        {
            std::unique_lock<std::mutex> lk(p->mu);
            p->input_done = true;
            my_epoch = p->reader_epoch;
            p->cv.notify_all();
            p->cv.wait(lk, [&] { return p->stop.load() || p->decoder_epoch == my_epoch; });
        }
        if (p->stop) break;

        // 自动连播：下一曲；越界即整场播完。
        idx++;
        if (idx >= count) {
            p->stop = true;
            p->cv.notify_all();
            if (p->host) p->host->OnAllFinished(p);
            return;
        }
    }

    // 停止/错误退出：确保 decoder 能观察到 stop 退出。
    p->stop = true;
    p->cv.notify_all();
}

// ============================ decoder 线程 ============================

// 带 skip 的喂入：续播/恢复时先丢弃 skip_remaining 个输出样本（文件 seek 近似），
// 其余阻塞喂播放管线（带起播代次，打断即丢弃）。fed_samples 累计已播样本 → position。
// 返回本次真正喂进播放管线的样本数（跳过部分不计入）——调用方据此判断"是否真的出声了"，
// 驱动 Loading→Playing 的唯一触发点（见 PumpDecoderMain 里的 flowing_reported）。
static size_t FeedWithSkip(Pump* p, std::vector<int16_t>& pcm, uint64_t& skip_remaining,
#ifndef ESP_PLATFORM
                           WavDump& wav,
#endif
                           uint32_t gen) {
    size_t off = 0;
    if (skip_remaining > 0) {
        size_t drop = std::min<uint64_t>(skip_remaining, pcm.size());
        skip_remaining -= drop;
        off = drop;  // 被跳过的样本已在 position 起点计入，不喂不 dump
    }
    size_t n = pcm.size() - off;
    if (n == 0) return 0;
    size_t fed = mhal::audio_pipeline::FeedPlayback(pcm.data() + off, n, kFeedTimeoutMs, gen);
    p->fed_samples += fed;
#ifndef ESP_PLATFORM
    wav.Write(pcm.data() + off, fed);
#endif
    return fed;
}

static void DecoderRun(Pump* p) {
    // 解码器按 track_codec 惰性创建：codec 未变时换曲只 Reset() 复用（解码器大块状态
    // 都在堆上，见 media_decoder.h 约定），变了才重建。
    std::unique_ptr<MediaDecoder> decoder;
    MediaCodec decoder_codec = MediaCodec::Mp3;
    Resampler resampler;
    std::vector<int16_t> out;   // 重采样输出缓冲（复用）
#ifndef ESP_PLATFORM
    WavDump wav;
    wav.MaybeOpen();
#endif

    const int sr = mhal::audio_pipeline::SampleRate();
    int adopted = 0;             // 本 decoder 已接手的 epoch
    uint64_t skip_remaining = 0;
    const int64_t start_us = esp_timer_get_time();
    int64_t last_log_us = start_us;
    // 分段耗时探针（真机排查解码追不上实时用；随 5s wall 日志打印后清零）。
    // us_decode 为 Feed 总耗时刨去回调内的重采样/喂入耗时，即纯解码时间。
    int64_t us_decode = 0, us_resamp = 0, us_feed = 0;
    // 本曲是否已上报过 OnPlaybackFlowing（Loading→Playing）：每接手一个新 epoch（新曲）
    // 重置，跳过阶段（续播 seek）不算数——必须真有样本喂进播放管线才算"在出声"。
    bool flowing_reported = false;

    // 每帧回调：重采样 → 带 skip 喂播放管线 → 首帧触发 Loading→Playing → 追赶限速。
    // 构造一次跨曲复用（捕获全为引用/指针）；返回 false 要求解码器尽快中止（stop 信号）。
    const MediaDecoder::FrameFn on_frame = [&](const int16_t* pcm, int frames, int channels, int hz) -> bool {
        p->decoded_frames++;
        out.clear();
        int64_t tp = esp_timer_get_time();
        resampler.Process(pcm, frames, channels, hz, out);
        us_resamp += esp_timer_get_time() - tp;
        p->resampled_samples += out.size();
        tp = esp_timer_get_time();
        size_t fed = FeedWithSkip(p, out, skip_remaining,
#ifndef ESP_PLATFORM
                                  wav,
#endif
                                  p->playback_gen);
        us_feed += esp_timer_get_time() - tp;
        // 首个真正喂进播放管线的帧（跳过阶段不算）：Loading→Playing 的唯一触发点。
        // 修复此前"源一打开就报 Playing"导致从未连上的流长时间误报播放中。
        if (fed > 0 && !flowing_reported) {
            flowing_reported = true;
            if (p->host) p->host->OnPlaybackFlowing(p, p->cur_index.load());
        }
        // 追赶限速：缓冲已 ≥1s 后每帧让出 2ms。此前起播/切曲要全速灌满 3s 水位，
        // decoder 连续满负荷 5-8s 与 LVGL 同核同优先级抢时间片（真机实测该窗口
        // 每帧解码从稳态 1.3ms 劣化到 25ms、核1 拉满、UI 掉帧卡顿）。限速后
        // 首秒仍全速（快出声），其后 ~8×实时温和填充，3s 水位 <1s 建立。
        if (!p->stop && mhal::audio_pipeline::PlaybackFilled() >= (size_t)(sr * 2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !p->stop;
    };

    for (;;) {
        if (p->stop) break;

        // 边下边播节流阀（文件/流统一）：不超前播放缓冲 kDecoderPaceSec 秒再解码喂入，
        // 令 position(已喂/16000)≈实时、内存有界，并把整条链路压成 16k 实时自时钟。
        while (!p->stop &&
               mhal::audio_pipeline::PlaybackFilled() / (size_t)(sr * 2) >= (size_t)kDecoderPaceSec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (p->stop) break;

        std::vector<uint8_t> pulled;
        bool input_done_snap = false, ring_empty_snap = false;
        bool adopt_new = false;
        MediaCodec adopt_codec = MediaCodec::Mp3;
        {
            std::unique_lock<std::mutex> lk(p->mu);
            p->cv.wait(lk, [&] {
                return p->stop.load() || p->reader_epoch > adopted || !p->ring.empty() ||
                       (p->input_done && p->ring.empty());
            });
            if (p->stop) break;
            if (p->reader_epoch > adopted) {
                // 新曲：接手 epoch，快照本曲编解码（与 reader 同锁写读，无独立竞态）。
                adopted = p->reader_epoch;
                adopt_new = true;
                adopt_codec = p->track_codec;
            }
            if (!p->ring.empty()) {
                pulled.resize(std::min(p->ring.size(), kDecoderPull));
                p->ring.pop_front(pulled.data(), pulled.size());
            }
            input_done_snap = p->input_done;
            ring_empty_snap = p->ring.empty();
        }
        if (!pulled.empty()) p->cv.notify_all();  // 唤醒 reader（腾出空间）

        if (adopt_new) {
            // 复位解码器（codec 不变则复用实例）与重采样，重置位置/skip/出声上报。
            if (decoder == nullptr || decoder_codec != adopt_codec) {
                decoder = CreateMediaDecoder(adopt_codec);
                decoder_codec = adopt_codec;
            } else {
                decoder->Reset();
            }
            resampler.Reset();
            // skip 仅用于首曲（续播/恢复的起播索引）；自动连播的后续曲从 0 起。
            skip_remaining = (adopted == 1) ? p->skip_out_samples : 0;
            p->fed_samples = skip_remaining;
            flowing_reported = false;  // 新曲：Loading→Playing 需要重新用首帧触发
            if (decoder == nullptr) {
                // codec 无解码器（如 sim 端 AAC 未接入）或内存不足：报错停泵。
                ESP_LOGE(TAG, "decoder create failed (codec=%d)", (int)adopt_codec);
                if (p->host) p->host->OnTrackError(p, p->cur_index.load(), "decoder unavailable");
                p->stop = true;
                p->cv.notify_all();
                break;
            }
        }

        // 榨干本次拉取：解码器逐帧回调 on_frame（重采样→喂入），at_eof 时放开水位榨干尾部。
        const bool at_eof = input_done_snap && ring_empty_snap;
        if (decoder != nullptr && (!pulled.empty() || at_eof)) {
            const int64_t cb_before = us_resamp + us_feed;
            const int64_t tp = esp_timer_get_time();
            const bool ok = decoder->Feed(pulled.empty() ? nullptr : pulled.data(), pulled.size(), at_eof, on_frame);
            us_decode += (esp_timer_get_time() - tp) - ((us_resamp + us_feed) - cb_before);
            if (!ok && !p->stop) {
                ESP_LOGE(TAG, "unrecoverable decode error (codec=%d)", (int)decoder_codec);
                if (p->host) p->host->OnTrackError(p, p->cur_index.load(), "decode error");
                p->stop = true;
                p->cv.notify_all();
                break;
            }
        }

        // 每 5s 打一行进度（state/position/已解码帧数/重采样后样本数/字节环高水位）。
        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= 5000000) {
            last_log_us = now;
            ESP_LOGI(TAG,
                     "wall=%ds state=%s pos=%ds frames=%u resamp=%u ring=%uB ring_hi=%uB "
                     "dec=%dms rs=%dms feed=%dms",
                     (int)((now - start_us) / 1000000), StateName(MediaController::Instance().state()),
                     MediaController::Instance().position_s(),
                     (unsigned)p->decoded_frames.load(), (unsigned)p->resampled_samples.load(),
                     (unsigned)p->ring.size(), (unsigned)p->ring_high_water.load(),
                     (int)(us_decode / 1000), (int)(us_resamp / 1000), (int)(us_feed / 1000));
            us_decode = us_resamp = us_feed = 0;
        }

        // 本曲是否解码搬完：input_done + 字节环空（at_eof）即榨干完毕（Feed 在 at_eof 下
        // 已放开保留约束逐帧到底并丢弃无法成帧的尾部）→ 握手告知 reader。
        if (at_eof && !p->stop) {
            {
                std::lock_guard<std::mutex> lk(p->mu);
                if (p->input_done && p->ring.empty()) p->decoder_epoch = adopted;
            }
            p->cv.notify_all();
            // 等 reader 起下一曲或 stop
            std::unique_lock<std::mutex> lk(p->mu);
            p->cv.wait(lk, [&] { return p->stop.load() || p->reader_epoch > adopted; });
        }
    }

#ifndef ESP_PLATFORM
    wav.Finalize();
#endif
    ESP_LOGI(TAG, "decoder exit: frames=%u resamp=%u fed=%u cleared_tail=%uB",
             (unsigned)p->decoded_frames.load(), (unsigned)p->resampled_samples.load(),
             (unsigned)p->fed_samples.load(), (unsigned)(decoder ? decoder->discarded_tail() : 0));
}

// ============================ 线程入口包装 ============================

// 跑完线程体后发退出信号：设备端 teardown 靠它等线程真正结束（不能 pthread_join，
// 见 media_internal.h Pump::exit_sem 注释）。PumpSignalExit 之后不得再触碰 p。
void PumpReaderMain(Pump* p) {
    ReaderRun(p);
    PumpSignalExit(p);
}

void PumpDecoderMain(Pump* p) {
    DecoderRun(p);
    PumpSignalExit(p);
}

}  // namespace media
