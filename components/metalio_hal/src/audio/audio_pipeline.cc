#include "metalio_hal/audio_pipeline.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

#include "audio_codec.h"
#include "metalio_hal/audio.h"

// 剥离自旧 xiaozhi AudioService（MetalioClaw5/main/audio/audio_service.cc）：
// - 采集任务模型 = AudioInputTask（去 wake_word/testing 分支）+ 120ms 预热
//   （audio_input_need_warmup_）+ 闲置断电（CheckAndUpdateAudioPowerState）。
// - 播放任务模型 = AudioOutputTask 的队列→OutputData 循环，Opus 编解码层
//   整体不剥离（火山两向都是裸 PCM，无消费者）。
// - VAD：AFE(esp-sr) 的 vadnet 依赖模型分区与 esp-sr 组件（提取时已裁），
//   且本板 mono mic 无回采参考通道，AEC/NS 收益有限；以能量迟滞 VAD 兜底，
//   处理器挂点保持 Feed→帧回调同形，后续可无缝换 AFE。
namespace mhal::audio_pipeline {

static const char* TAG = "audio_pipe";

int SampleRate() { return mhal::audio::Codec()->input_sample_rate(); }

// ============================ 采集侧 ============================

namespace {

struct CaptureState {
    TaskHandle_t task = nullptr;
    volatile bool stop_requested = false;
    volatile bool running = false;
    volatile bool speaking = false;
    CaptureConfig cfg;
    CaptureCallbacks cbs;
    // VAD 迟滞
    uint32_t silence_ms = 0;
};

CaptureState s_cap;

// 帧平均幅度（|x| 均值）——能量 VAD 的特征量
int FrameMeanAbs(const int16_t* pcm, size_t samples) {
    int64_t sum = 0;
    for (size_t i = 0; i < samples; i++) {
        sum += pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
    }
    return (int)(sum / (int64_t)samples);
}

void CaptureVad(const int16_t* pcm, size_t samples) {
    int level = FrameMeanAbs(pcm, samples);
    if (s_cap.speaking) {
        if (level < s_cap.cfg.vad_exit_threshold) {
            s_cap.silence_ms += s_cap.cfg.frame_ms;
            if (s_cap.silence_ms >= (uint32_t)s_cap.cfg.vad_hangover_ms) {
                s_cap.speaking = false;
                if (s_cap.cbs.on_vad) s_cap.cbs.on_vad(false);
            }
        } else {
            s_cap.silence_ms = 0;
        }
    } else if (level >= s_cap.cfg.vad_enter_threshold) {
        s_cap.speaking = true;
        s_cap.silence_ms = 0;
        if (s_cap.cbs.on_vad) s_cap.cbs.on_vad(true);
    }
}

void CaptureTask(void*) {
    const int frame_samples = s_cap.cfg.frame_ms * SampleRate() / 1000;
    auto* buf = (int16_t*)heap_caps_malloc(frame_samples * sizeof(int16_t),
                                           MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "capture frame alloc failed");
        s_cap.running = false;
        vTaskDelete(nullptr);
        return;
    }

    mhal::audio::EnableInput(true);
    // mic 预热：丢弃前 120ms（对齐旧固件 audio_input_need_warmup_）
    for (int discarded = 0; discarded < 120 && !s_cap.stop_requested;
         discarded += s_cap.cfg.frame_ms) {
        mhal::audio::ReadPcm(buf, frame_samples);
    }

    ESP_LOGI(TAG, "capture started: %dms/frame (%d samples)",
             s_cap.cfg.frame_ms, frame_samples);
    uint32_t dbg_ms = 0;
    int dbg_peak = 0;
    while (!s_cap.stop_requested) {
        int got = mhal::audio::ReadPcm(buf, frame_samples);
        // ReadPcm 底层是 I2S slave 的有界阻塞读（bt_audio_codec 里 200ms 超时）：
        // 停止后最多一个超时周期即返回。若返回时已 StopCapture（并随后
        // volc_asr_stop 注销了会话），绝不能再把这最后一帧喂给已释放的消费者
        // ——立即退出。BT 模组掉时钟时 got 会持续为 0，下面的分支温和轮询。
        if (s_cap.stop_requested) break;
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(s_cap.cfg.frame_ms));
            continue;
        }
        // 每秒一条 mic 电平：判断"没识别出来"是没采到声还是链路问题
        int level = FrameMeanAbs(buf, got);
        if (level > dbg_peak) dbg_peak = level;
        dbg_ms += s_cap.cfg.frame_ms;
        if (dbg_ms >= 1000) {
            ESP_LOGI(TAG, "mic level: peak %d (1s)", dbg_peak);
            dbg_ms = 0;
            dbg_peak = 0;
        }
        if (s_cap.cfg.enable_vad) CaptureVad(buf, got);
        if (s_cap.cbs.on_frame) s_cap.cbs.on_frame(buf, got);
    }

    mhal::audio::EnableInput(false);
    free(buf);
    s_cap.running = false;
    ESP_LOGI(TAG, "capture stopped");
    vTaskDelete(nullptr);
}

}  // namespace

bool StartCapture(const CaptureConfig& cfg, CaptureCallbacks cbs) {
    if (s_cap.running) return false;
    if (cfg.frame_ms < 10 || cfg.frame_ms > 200) return false;
    s_cap.cfg = cfg;
    s_cap.cbs = std::move(cbs);
    s_cap.stop_requested = false;
    s_cap.speaking = false;
    s_cap.silence_ms = 0;
    s_cap.running = true;
    // 优先级对齐旧固件 audio_input 任务（8）：采集不能被 UI 饿死。栈 6KB：
    // on_frame 消费者可能在本任务栈上跑 gzip + TLS 写（volc_asr_feed 路径）。
    if (xTaskCreate(CaptureTask, "audio_capture", 6144, nullptr, 8,
                    &s_cap.task) != pdPASS) {
        s_cap.running = false;
        return false;
    }
    return true;
}

void StopCapture() {
    if (!s_cap.running) return;
    s_cap.stop_requested = true;
    // ReadPcm 底层是 I2S slave 的有界阻塞读（bt_audio_codec 里 200ms 超时）：
    // 即便 BT 模组掉时钟（未上电/PowerCycle/复位），采集任务也会在 ≤200ms 内
    // 从 ReadPcm 返回并看到 stop_requested 退出、清零 running，故 1000ms join
    // 正常必然成功。下面的超时放弃仅作极端兜底（如底层驱动异常长时间不返回），
    // 避免把调用线程（pi_voice 语音控制任务）拖死——放弃后任务仍会自行退出且
    // 已被 stop 守卫拦住不再触碰消费者，最坏是下一轮 StartCapture 因 running
    // 未清而暂时失败，而非整条语音链死锁。
    const int kJoinTimeoutMs = 1000;
    int waited = 0;
    while (s_cap.running && waited < kJoinTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (s_cap.running) {
        ESP_LOGW(TAG, "StopCapture: capture task stuck (I2S no clock?), abandoning join");
    }
}

bool IsCapturing() { return s_cap.running; }

bool IsVoiceDetected() { return s_cap.speaking; }

// ============================ 播放侧 ============================

namespace {

constexpr size_t kPlayerChunk = 4096;  // 单次写块上限：128ms，打断延迟可控

struct PlaybackState {
    RingbufHandle_t rb = nullptr;
    TaskHandle_t task = nullptr;
    PlaybackConfig cfg;
    volatile bool flushing = false;
    volatile bool writing = false;   // 正在写 I2S（IsPlaybackIdle 判据之一）
    volatile bool had_data = false;  // 本轮是否播放过数据（排空回调条件）
    volatile bool prebuffering = true;  // 下批数据到达时先做低水位预积累
    std::atomic<int> feeding{0};     // 在途 FeedPlayback 数（Flush 竞态收口）
    std::atomic<uint32_t> gen{0};    // 播放代次，每次 FlushPlayback 进入即 ++
    std::mutex drained_mutex;
    std::function<void()> drained_cb;
};

PlaybackState s_play;

size_t PlaybackFilled() {
    return s_play.cfg.queue_bytes - xRingbufferGetCurFreeSize(s_play.rb);
}

void FireDrainedCb() {
    // 持锁执行回调：OnPlaybackDrained(nullptr) 的注销方取得锁即保证不再有
    // 在途回调（回调约定禁止阻塞/回注册，见头文件），消费者可安全释放自身。
    std::lock_guard<std::mutex> lock(s_play.drained_mutex);
    if (!s_play.drained_cb) return;
    auto cb = std::move(s_play.drained_cb);
    s_play.drained_cb = nullptr;
    cb();
}

void PlaybackTask(void*) {
    bool output_on = false;
    uint32_t idle_ms = 0;

    while (true) {
        // 低水位预启动：空转后首批数据到达时，先积累 prestart_ms 再开播，
        // 避免流式下行初期欠载卡顿；积累超时（3x）则不再等。
        if (s_play.prebuffering && PlaybackFilled() > 0 && !s_play.flushing) {
            const size_t prestart_bytes =
                (size_t)s_play.cfg.prestart_ms * SampleRate() * 2 / 1000;
            for (uint32_t waited = 0;
                 PlaybackFilled() < prestart_bytes &&
                 waited < s_play.cfg.prestart_ms * 3 && !s_play.flushing;
                 waited += 10) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            s_play.prebuffering = false;
        }

        size_t got = 0;
        auto* item = (uint8_t*)xRingbufferReceiveUpTo(
            s_play.rb, &got, pdMS_TO_TICKS(50), kPlayerChunk);
        if (item) {
            idle_ms = 0;
            if (!s_play.flushing && got >= 2) {
                if (!output_on) {
                    mhal::audio::EnableOutput(true);
                    output_on = true;
                }
                s_play.writing = true;
                s_play.had_data = true;
                mhal::audio::WritePcm((const int16_t*)item, got / 2);
                s_play.writing = false;
            }
            vRingbufferReturnItem(s_play.rb, item);
            continue;
        }

        // 队列排空
        if (s_play.had_data) {
            s_play.had_data = false;
            s_play.prebuffering = true;
            FireDrainedCb();
        }
        idle_ms += 50;
        if (output_on && idle_ms >= s_play.cfg.idle_power_off_ms) {
            mhal::audio::EnableOutput(false);
            output_on = false;
            ESP_LOGI(TAG, "playback idle %ums, speaker off",
                     (unsigned)idle_ms);
        }
    }
}

}  // namespace

bool EnsurePlayback(const PlaybackConfig& cfg) {
    if (s_play.rb) return true;
    s_play.cfg = cfg;
    // 偶数队列长度：BYTEBUF 任意连续段边界保持 16bit 对齐
    s_play.cfg.queue_bytes &= ~(size_t)1;
    s_play.rb = xRingbufferCreateWithCaps(s_play.cfg.queue_bytes,
                                          RINGBUF_TYPE_BYTEBUF,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_play.rb) return false;
    // 优先级对齐旧固件 audio_output 任务（4）
    if (xTaskCreate(PlaybackTask, "audio_playback", 4096, nullptr, 4,
                    &s_play.task) != pdPASS) {
        vRingbufferDeleteWithCaps(s_play.rb);
        s_play.rb = nullptr;
        return false;
    }
    return true;
}

uint32_t PlaybackGen() { return s_play.gen.load(); }

size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms,
                    uint32_t expected_gen) {
    if (!s_play.rb || samples == 0) return 0;
    if (s_play.flushing) return samples;  // 打断中：静默丢弃

    s_play.feeding++;
    // 代次校验：feeding++ 与入队之间若已发生 FlushPlayback（gen 已 ++），本帧
    // 属于被打断的上一轮，不入队、按已消费丢弃。补齐了"喂入方过了自身 discard
    // 检查、尚未 feeding++"窗口里 flushing 已被清回、这帧被当新一轮播出的竞态。
    if (s_play.gen.load() != expected_gen) {
        s_play.feeding--;
        return samples;
    }
    const auto* src = (const uint8_t*)pcm;
    size_t remaining = samples * sizeof(int16_t);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (remaining > 0 && !s_play.flushing) {
        size_t chunk = remaining < kPlayerChunk ? remaining : kPlayerChunk;
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (now < deadline) ? (deadline - now) : 0;
        if (xRingbufferSend(s_play.rb, src, chunk, wait) != pdTRUE) {
            if (wait == 0) break;  // 总 deadline 用尽：放弃剩余，返回值告知
            continue;              // 按剩余 deadline 再等
        }
        src += chunk;
        remaining -= chunk;
    }
    s_play.feeding--;
    return samples - remaining / sizeof(int16_t);
}

// 三参兼容重载：期望代次取当前值，即不做代次校验（保留既有调用方语义）。
size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms) {
    return FeedPlayback(pcm, samples, timeout_ms, s_play.gen.load());
}

void FlushPlayback() {
    if (!s_play.rb) return;
    // 一进来即自增代次：晚于此刻读代次的喂入方（FeedPlayback 带 expected_gen
    // 重载）会看到不等而丢弃本轮迟到帧，见 FeedPlayback。
    s_play.gen.fetch_add(1);
    s_play.flushing = true;
    // 清空队列（播放任务同时在 flushing 下丢弃，二者并发消费无害）
    size_t got = 0;
    void* item;
    while ((item = xRingbufferReceiveUpTo(s_play.rb, &got, 0, kPlayerChunk)) !=
           nullptr) {
        vRingbufferReturnItem(s_play.rb, item);
    }
    // 喂入竞态收口：置 flushing 前已阻塞在 xRingbufferSend 的喂入方，会因
    // 上面的排空腾出空间而先完成一次 send 才看到 flushing 退出。等它退出后
    // 再排空一次，防止这 ≤1 块残帧（≤128ms）在 flushing 清零后被播出。
    {
        const int kFeederExitTimeoutMs = 200;
        int waited = 0;
        while (s_play.feeding.load() > 0 && waited < kFeederExitTimeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        while ((item = xRingbufferReceiveUpTo(s_play.rb, &got, 0,
                                              kPlayerChunk)) != nullptr) {
            vRingbufferReturnItem(s_play.rb, item);
        }
    }
    // 等正在写的残帧（≤128ms）落地，保证返回后不再出声。加超时兜底：
    // WritePcm 底层 i2s_channel_write 是 portMAX_DELAY，BT 模组掉时钟时会
    // 卡死——超时后放弃等待（此时 I2S 无时钟本就不出声），避免 FlushPlayback
    // 的调用方（持 TTS api_lock 的 barge-in 路径）被无限拖死。300ms > 2x 单块
    // 128ms，正常路径永不触发。
    {
        const int kWriteDrainTimeoutMs = 300;
        int waited = 0;
        while (s_play.writing && waited < kWriteDrainTimeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        if (s_play.writing) {
            ESP_LOGW(TAG, "FlushPlayback: write stuck (I2S no clock?), abandoning wait");
        }
    }
    s_play.had_data = false;
    // 打断清掉了 had_data，播放任务不会再走"排空→重新武装预积累"分支，
    // 在此补上：下一场播报同样先积累 prestart_ms，避免打断后首帧欠载。
    s_play.prebuffering = true;
    s_play.flushing = false;
    // 打断即视为一次排空（挂起的排空回调不再有意义，直接触发释放等待方）
    FireDrainedCb();
}

void OnPlaybackDrained(std::function<void()> cb) {
    if (!cb) {  // 注销：FireDrainedCb 持锁执行，取得锁即无在途回调
        std::lock_guard<std::mutex> lock(s_play.drained_mutex);
        s_play.drained_cb = nullptr;
        return;
    }
    if (!s_play.rb || IsPlaybackIdle()) {
        cb();
        return;
    }
    bool fire_now = false;
    {
        std::lock_guard<std::mutex> lock(s_play.drained_mutex);
        s_play.drained_cb = std::move(cb);
        // 注册与排空竞态：注册后已经空了就立即补触发
        fire_now = IsPlaybackIdle();
    }
    if (fire_now) FireDrainedCb();
}

bool IsPlaybackIdle() {
    if (!s_play.rb) return true;
    return PlaybackFilled() == 0 && !s_play.writing;
}

}  // namespace mhal::audio_pipeline
