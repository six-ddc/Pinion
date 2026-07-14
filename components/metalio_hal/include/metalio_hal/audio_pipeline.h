#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// 音频管线：麦克风连续采集与扬声器流式播放的基建层（从旧 xiaozhi 固件
// AudioService 的任务/队列模型剥离，去掉 protocol/Application/wake_word/
// opus 耦合）。构建在 mhal::audio 裸 codec 之上，采样率跟随 codec 原生率
//（本板 16kHz/16bit/mono，见 SampleRate()）。
//
// 采集：独立任务拉流 → [可选能量 VAD] → 固定帧回调（frame_ms 可配）。
// 消费者（如火山 ASR）在回调里拿帧；回调运行在采集任务上下文，禁止阻塞。
// 处理器挂点与旧 AfeAudioProcessor 同形（Feed→OnOutput 帧回调），后续若
// 要接 esp-sr AFE，替换任务内部处理段即可，外部 API 不变。
//
// 播放：流式喂入 PCM → PSRAM 抖动队列 → 播放任务写 I2S。支持低水位预启动
//（防启动欠载）、打断清空（barge-in 立即静音）、排空一次性回调、闲置自动
// 关功放通路。播放中音量控制直接用 mhal::audio::SetVolume（codec 级实时
// 生效）。
namespace mhal::audio_pipeline {

// codec 原生采样率（本板 16000），采集回调帧与播放喂入均为该率。
int SampleRate();

// —— 采集侧 ——

struct CaptureConfig {
    int frame_ms = 20;            // 回调帧长，10–200ms
    bool enable_vad = true;       // 能量 VAD（启发式，非模型；见 .cc 说明）
    int vad_enter_threshold = 1000;  // 帧平均幅度（int16）进入语音阈值
    int vad_exit_threshold = 400;    // 退出语音阈值（迟滞）
    int vad_hangover_ms = 600;       // 低于阈值持续该时长才判静音
};

struct CaptureCallbacks {
    // 每帧 PCM（16kHz 16bit mono，samples = frame_ms*16）。采集任务上下文。
    std::function<void(const int16_t* pcm, size_t samples)> on_frame;
    // VAD 状态翻转（enable_vad 时）。采集任务上下文。
    std::function<void(bool speaking)> on_vad;
};

// 启动采集（使能 mic、丢 120ms 预热帧后开始回调）。已在采集时返回 false。
bool StartCapture(const CaptureConfig& cfg, CaptureCallbacks cbs);
// 停止采集并关闭 mic 通路。阻塞至采集任务退出（≤ 一帧时长）。
void StopCapture();
bool IsCapturing();
bool IsVoiceDetected();  // 最近 VAD 状态（未开 VAD 恒 false）

// —— 播放侧 ——

struct PlaybackConfig {
    // 抖动队列（PSRAM），~64s @16k/16bit。定容原则：装得下"上游产出超前
    // 实时播放"的典型积压量，让有界应答（如整段 TTS 回复）尽快全部收进
    // 本地——喂入方长期阻塞会把上游 socket 压成 TCP zero-window，触发对端
    // slow-consumer 断连。队列满的背压 + Feed 超时只是超长应答的安全网。
    size_t queue_bytes = 2 * 1024 * 1024;
    uint32_t prestart_ms = 100;  // 队列积累到该时长（或 300ms 超时）才开播
    uint32_t idle_power_off_ms = 15000;  // 队列空闲该时长后关扬声器通路
};

// 幂等：首次调用创建队列与播放任务。之后的 cfg 被忽略。
bool EnsurePlayback(const PlaybackConfig& cfg = {});
// 流式喂入 PCM，队列满则阻塞至 timeout_ms（背压点）。返回实际接受的样本数。
size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms);
// 打断：清空队列，正在写 I2S 的残帧（≤128ms）播完即静音。
void FlushPlayback();
// 一次性回调：播放队列由非空转为排空时触发（当前已空则立即触发）。
// 播放任务上下文，禁止阻塞、禁止在回调内再注册。传 nullptr 注销未触发的
// 回调——返回即保证不再有在途回调（会等正在执行的回调结束）。
void OnPlaybackDrained(std::function<void()> cb);
bool IsPlaybackIdle();  // 队列空且无在写帧

}  // namespace mhal::audio_pipeline
