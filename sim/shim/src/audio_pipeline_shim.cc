// sim shim — mhal::audio_pipeline. No real capture: on_frame is never called
// (volc_asr_feed is a no-op on host anyway); IsVoiceDetected() mirrors the
// simulated typing session so pi_screen's key-mode VAD auto-send works.
//
// Playback side is a scaled-down but behaviorally faithful model of the real
// task (components/metalio_hal/src/audio/audio_pipeline.cc): a bounded
// sample queue drained by a background "playback" thread at real 16kHz
// pacing, FeedPlayback blocking on a full queue up to timeout_ms, and the
// same Flush/feeder race + drained-callback locking as the real
// implementation — so the backpressure / flush-race / drained-callback bugs
// fixed there on hardware are reproducible on host too. Sim has no amplifier
// to power on/off, so that part of the real task is simply not modeled.
#include "metalio_hal/audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "sim_hooks.h"

namespace mhal::audio_pipeline {

namespace {
std::atomic<bool> g_capturing{false};
}

int SampleRate() { return 16000; }

bool StartCapture(const CaptureConfig& cfg, CaptureCallbacks cbs) {
    (void)cfg;
    (void)cbs;
    if (g_capturing.exchange(true)) return false;
    return true;
}

void StopCapture() { g_capturing = false; }

bool IsCapturing() { return g_capturing; }

bool IsVoiceDetected() { return g_capturing && sim_asr_voice_detected(); }

// ============================ 播放侧 ============================

namespace {

constexpr size_t kPlayerChunk = 4096 / sizeof(int16_t);  // 单次消费上限：128ms@16k，同真机

struct PlaybackState {
    std::atomic<bool> ensured{false};
    PlaybackConfig cfg;
    std::deque<int16_t> queue;
    std::mutex mu;
    std::condition_variable not_full;
    std::condition_variable not_empty;
    std::atomic<bool> flushing{false};
    std::atomic<bool> writing{false};   // 正在"写"（IsPlaybackIdle 判据之一）
    std::atomic<bool> had_data{false};  // 本轮是否播放过数据（排空回调条件）
    std::atomic<bool> prebuffering{true};
    std::atomic<int> feeding{0};  // 在途 FeedPlayback 数（Flush 竞态收口）
    std::mutex drained_mutex;
    std::function<void()> drained_cb;
};

PlaybackState s_play;

size_t CapacitySamples() { return s_play.cfg.queue_bytes / sizeof(int16_t); }

size_t PlaybackFilled() {
    std::lock_guard<std::mutex> lock(s_play.mu);
    return s_play.queue.size();
}

void FireDrainedCb() {
    // 持锁执行回调：OnPlaybackDrained(nullptr) 的注销方取得锁即保证不再有
    // 在途回调，消费者可安全释放自身。
    std::lock_guard<std::mutex> lock(s_play.drained_mutex);
    if (!s_play.drained_cb)
        return;
    auto cb = std::move(s_play.drained_cb);
    s_play.drained_cb = nullptr;
    cb();
}

void PlaybackTask() {
    while (true) {
        // 低水位预启动：空转后首批数据到达时，先积累 prestart_ms 再开播。
        if (s_play.prebuffering && PlaybackFilled() > 0 && !s_play.flushing) {
            const size_t prestart_samples = (size_t)s_play.cfg.prestart_ms * SampleRate() / 1000;
            for (uint32_t waited = 0; PlaybackFilled() < prestart_samples &&
                                      waited < s_play.cfg.prestart_ms * 3 && !s_play.flushing;
                 waited += 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            s_play.prebuffering = false;
        }

        std::vector<int16_t> chunk;
        {
            std::unique_lock<std::mutex> lock(s_play.mu);
            s_play.not_empty.wait_for(lock, std::chrono::milliseconds(50),
                                      [] { return !s_play.queue.empty(); });
            size_t take = std::min(s_play.queue.size(), kPlayerChunk);
            if (take > 0) {
                chunk.assign(s_play.queue.begin(), s_play.queue.begin() + take);
                s_play.queue.erase(s_play.queue.begin(), s_play.queue.begin() + take);
            }
        }
        if (!chunk.empty()) {
            s_play.not_full.notify_all();
            if (!s_play.flushing) {
                s_play.writing = true;
                s_play.had_data = true;
                // 真机在此写 I2S（受硬件节拍约束）；sim 用等时长 sleep 复现同样
                // 的实时消费速率，让背压/排空时序与真机同构。
                std::this_thread::sleep_for(
                    std::chrono::microseconds(chunk.size() * 1000000ULL / SampleRate()));
                s_play.writing = false;
            }
            continue;
        }

        // 队列排空
        if (s_play.had_data) {
            s_play.had_data = false;
            s_play.prebuffering = true;
            FireDrainedCb();
        }
    }
}

}  // namespace

bool EnsurePlayback(const PlaybackConfig& cfg) {
    if (s_play.ensured.exchange(true))
        return true;
    s_play.cfg = cfg;
    s_play.cfg.queue_bytes &= ~(size_t)1;  // 偶数字节数：16bit 对齐，同真机
    std::thread(PlaybackTask).detach();
    return true;
}

size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms) {
    if (!s_play.ensured || samples == 0)
        return 0;
    if (s_play.flushing)
        return samples;  // 打断中：静默丢弃（仅入口判一次，同真机）

    s_play.feeding++;
    size_t capacity = CapacitySamples();
    size_t remaining = samples;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    {
        std::unique_lock<std::mutex> lock(s_play.mu);
        while (remaining > 0 && !s_play.flushing) {
            size_t chunk = std::min(remaining, kPlayerChunk);
            // 等到有 chunk 个空位或超时——真机 xRingbufferSend 的阻塞语义。
            // 注意：FlushPlayback 排空腾出空间也会唤醒这里满足谓词，即使此刻
            // 已经置位 flushing（要到下一轮循环开头才会看到）；这一步塞入的
            // ≤1 块残帧由 FlushPlayback 的二次排空收口，语义对齐真机的
            // "喂入竞态收口" 修复。
            bool ok = s_play.not_full.wait_until(
                lock, deadline, [&] { return s_play.queue.size() + chunk <= capacity; });
            if (!ok)
                break;  // 总 deadline 用尽：放弃剩余，返回值告知
            const int16_t* src = pcm + (samples - remaining);
            s_play.queue.insert(s_play.queue.end(), src, src + chunk);
            remaining -= chunk;
            lock.unlock();
            s_play.not_empty.notify_one();
            lock.lock();
        }
    }
    s_play.feeding--;
    return samples - remaining;
}

void FlushPlayback() {
    if (!s_play.ensured)
        return;
    s_play.flushing = true;
    {
        std::lock_guard<std::mutex> lock(s_play.mu);
        s_play.queue.clear();
    }
    s_play.not_full.notify_all();  // 唤醒阻塞喂入方——可能因此再塞进一块残帧

    // 喂入竞态收口：等阻塞的喂入方退出后二次排空，防止上面唤醒时溜进的
    // ≤1 块残帧在 flushing 清零后被播出。
    {
        const int kFeederExitTimeoutMs = 200;
        int waited = 0;
        while (s_play.feeding.load() > 0 && waited < kFeederExitTimeoutMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waited += 10;
        }
        std::lock_guard<std::mutex> lock(s_play.mu);
        s_play.queue.clear();
    }

    // 等正在"写"的残帧落地，保证返回后不再有排队播放；超时兜底避免调用方
    // 被卡死（同真机）。
    {
        const int kWriteDrainTimeoutMs = 300;
        int waited = 0;
        while (s_play.writing && waited < kWriteDrainTimeoutMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waited += 10;
        }
    }
    s_play.had_data = false;
    s_play.prebuffering = true;  // 打断后下一场播报同样先积累 prestart_ms
    s_play.flushing = false;
    FireDrainedCb();  // 打断即视为一次排空
}

void OnPlaybackDrained(std::function<void()> cb) {
    if (!cb) {  // 注销：FireDrainedCb 持锁执行，取得锁即无在途回调
        std::lock_guard<std::mutex> lock(s_play.drained_mutex);
        s_play.drained_cb = nullptr;
        return;
    }
    if (!s_play.ensured || IsPlaybackIdle()) {
        cb();
        return;
    }
    bool fire_now = false;
    {
        std::lock_guard<std::mutex> lock(s_play.drained_mutex);
        s_play.drained_cb = std::move(cb);
        fire_now = IsPlaybackIdle();  // 注册与排空竞态：注册后已经空了就立即补触发
    }
    if (fire_now)
        FireDrainedCb();
}

bool IsPlaybackIdle() {
    if (!s_play.ensured)
        return true;
    return PlaybackFilled() == 0 && !s_play.writing;
}

}  // namespace mhal::audio_pipeline
