#include "metalio_hal/audio_pipeline.h"

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
        // ReadPcm 底层是 I2S slave 的 portMAX_DELAY 阻塞读，可能跨越停止点
        // 才返回。若此间已 StopCapture（并随后 volc_asr_stop 注销了会话），
        // 绝不能再把这最后一帧喂给已释放的消费者——立即退出。
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
    // 正常情况下采集任务在一帧内（≤frame_ms）就看到 stop_requested 退出。
    // 但 ReadPcm 底层是 I2S slave 的 portMAX_DELAY 阻塞读：BT 模组掉时钟
    // （未上电/PowerCycle/复位）时会长时间无法返回。join 加超时，避免把调用
    // 线程（pi_voice 语音控制任务）一起拖死——超时后放弃 join，任务自行退出
    // 且已被上面的 stop 守卫拦住不会再触碰消费者。最坏是下一轮 StartCapture
    // 因 running 未清而暂时失败，而非整条语音链死锁。
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
    std::mutex drained_mutex;
    std::function<void()> drained_cb;
};

PlaybackState s_play;

size_t PlaybackFilled() {
    return s_play.cfg.queue_bytes - xRingbufferGetCurFreeSize(s_play.rb);
}

void FireDrainedCb() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_play.drained_mutex);
        cb = std::move(s_play.drained_cb);
        s_play.drained_cb = nullptr;
    }
    if (cb) cb();
}

void PlaybackTask(void*) {
    bool prebuffering = true;
    bool output_on = false;
    uint32_t idle_ms = 0;
    // 诊断
    TickType_t dbg_last = xTaskGetTickCount();
    uint32_t dbg_played = 0, dbg_underrun = 0, dbg_prebuf = 0, dbg_prebuf_ms = 0;

    while (true) {
        // 诊断：每秒打印播放队列水位 / 本秒播放量 / 欠载(排空)次数 / 预缓冲停顿
        TickType_t dbg_now = xTaskGetTickCount();
        if ((dbg_now - dbg_last) * portTICK_PERIOD_MS >= 1000) {
            if (dbg_played > 0 || dbg_underrun > 0) {
                ESP_LOGI(TAG,
                         "play: rb=%uB played=%uB/s underrun=%u prebuf=%u(%ums)",
                         (unsigned)PlaybackFilled(), (unsigned)dbg_played,
                         (unsigned)dbg_underrun, (unsigned)dbg_prebuf,
                         (unsigned)dbg_prebuf_ms);
            }
            dbg_played = dbg_underrun = dbg_prebuf = dbg_prebuf_ms = 0;
            dbg_last = dbg_now;
        }

        // 低水位预启动：空转后首批数据到达时，先积累 prestart_ms 再开播，
        // 避免流式下行初期欠载卡顿；积累超时（3x）则不再等。
        if (prebuffering && PlaybackFilled() > 0 && !s_play.flushing) {
            dbg_prebuf++;
            const size_t prestart_bytes =
                (size_t)s_play.cfg.prestart_ms * SampleRate() * 2 / 1000;
            for (uint32_t waited = 0;
                 PlaybackFilled() < prestart_bytes &&
                 waited < s_play.cfg.prestart_ms * 3 && !s_play.flushing;
                 waited += 10) {
                vTaskDelay(pdMS_TO_TICKS(10));
                dbg_prebuf_ms += 10;
            }
            prebuffering = false;
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
                dbg_played += got;
            }
            vRingbufferReturnItem(s_play.rb, item);
            continue;
        }

        // 队列排空
        if (s_play.had_data) {
            s_play.had_data = false;
            prebuffering = true;
            dbg_underrun++;
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

size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms) {
    if (!s_play.rb || samples == 0) return 0;
    if (s_play.flushing) return samples;  // 打断中：静默丢弃

    const auto* src = (const uint8_t*)pcm;
    size_t remaining = samples * sizeof(int16_t);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (remaining > 0 && !s_play.flushing) {
        size_t chunk = remaining < kPlayerChunk ? remaining : kPlayerChunk;
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (now < deadline) ? (deadline - now) : 0;
        if (xRingbufferSend(s_play.rb, src, chunk, wait) != pdTRUE) {
            if (wait == 0) break;
            continue;  // 队列碎片放不下整块，缩块重试
        }
        src += chunk;
        remaining -= chunk;
    }
    return samples - remaining / sizeof(int16_t);
}

void FlushPlayback() {
    if (!s_play.rb) return;
    s_play.flushing = true;
    // 清空队列（播放任务同时在 flushing 下丢弃，二者并发消费无害）
    size_t got = 0;
    void* item;
    while ((item = xRingbufferReceiveUpTo(s_play.rb, &got, 0, kPlayerChunk)) !=
           nullptr) {
        vRingbufferReturnItem(s_play.rb, item);
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
    s_play.flushing = false;
    // 打断即视为一次排空（挂起的排空回调不再有意义，直接触发释放等待方）
    FireDrainedCb();
}

void OnPlaybackDrained(std::function<void()> cb) {
    if (!s_play.rb || IsPlaybackIdle()) {
        if (cb) cb();
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
