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
    // 下面三个 VAD 阈值以帧平均幅度（int16）为量纲，标定在 bt_audio_codec
    // 采集侧的 >>kRxSampleShift 电平上（见 bt_audio_codec.cc）：换 codec 或
    // 改那个移位量会整体改变电平，需同步重标这三个阈值。
    int vad_enter_threshold = 1000;  // 进入语音阈值
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
    // 抖动队列（PSRAM），2MB ≈ 64s @16k/16bit/mono。定容原则：装得下"上游
    // 产出超前实时播放"的典型积压量，让有界应答（如整段 TTS 回复）尽快全部
    // 收进本地——喂入方长期阻塞会把上游 socket 压成 TCP zero-window，触发对端
    // slow-consumer 断连。队列满的背压 + Feed 超时只是超长应答的安全网。
    //
    // 权衡与已知限制（本次仅抬阈值，未根治）：
    // - 超过 64s 的单条应答仍会灌满队列→背压→zero-window→断连；根治需应用层
    //   流控或断连续播，属后续工作，不在此队列尺寸内解决。
    // - 队列在首次 EnsurePlayback 时一次性从 PSRAM 分配、常驻不释放（播放任务
    //   常驻，无销毁路径），即便空闲也占着 2MB PSRAM。
    // - PSRAM 预算：与 LVGL 720×720×24bit 缓冲共享。单帧缓冲 720×720×3 ≈
    //   1.48MB，双缓冲 ≈ 3MB，叠加本队列 2MB 已占 PSRAM 主要份额——调大本值
    //   前需核对 PSRAM 余量，避免挤垮显示缓冲。
    size_t queue_bytes = 2 * 1024 * 1024;
    uint32_t prestart_ms = 100;  // 队列积累到该时长（或 300ms 超时）才开播
    uint32_t idle_power_off_ms = 15000;  // 队列空闲该时长后关扬声器通路
};

// 幂等：首次调用创建队列与播放任务。之后的 cfg 被忽略。
bool EnsurePlayback(const PlaybackConfig& cfg = {});
// 流式喂入 PCM，队列满则阻塞至 timeout_ms（背压点）。返回实际接受的样本数。
size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms);
// 播放代次：每次 FlushPlayback 进入即自增。配合下面带 expected_gen 的重载，
// 关闭打断残音竞态——喂入方过了自身的 discard 检查、尚未入队时，若打断线程
// 跑完 FlushPlayback 把 flushing 清回，这帧本会进已清空队列被当新一轮播出。
uint32_t PlaybackGen();
// 带期望代次的喂入：内部 feeding++ 之后、入队之前校验代次仍等于 expected_gen，
// 不等（其间发生过 FlushPlayback）则不入队、按已消费丢弃（返回 samples）。
// expected_gen 由喂入方在会话开始 / 每次打断后用 PlaybackGen() 捕获。
size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms, uint32_t expected_gen);
// 打断：清空队列，正在写 I2S 的残帧（≤128ms）播完即静音。
void FlushPlayback();
// 一次性回调：播放队列由非空转为排空时触发（当前已空则立即触发）。
// 播放任务上下文，禁止阻塞、禁止在回调内再注册。传 nullptr 注销未触发的
// 回调——返回即保证不再有在途回调（会等正在执行的回调结束）。
void OnPlaybackDrained(std::function<void()> cb);
bool IsPlaybackIdle();  // 队列空且无在写帧

}  // namespace mhal::audio_pipeline
