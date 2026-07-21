// media_decoder — 解码器抽象：pump 的 decoder 线程只认这个接口，不关心具体编解码库。
// 工厂按端各自提供（分端范式同 media_http_stream）：
//   真机: media_decoder_esp.cc      — espressif/esp_audio_codec 的 esp_audio_simple_dec（官方闭源 .a，MP3+AAC）
//   sim : media_decoder_minimp3.cc  — MP3 用 minimp3（滑窗语义在 impl 内部）；AAC 由 helix-aac impl 提供
// 约定：
//   - Feed 由 decoder 线程独占调用（非并发）。实现自持输入缓冲（滑窗/内部积累），接受任意
//     大小分块；所有大块状态**堆分配**——设备端解码线程栈紧张（minimp3 曾因 28KB 栈上
//     scratch 建不出线程，教训保留）。
//   - on_frame 对每个解出的 PCM 帧回调：interleaved int16、frames（每声道样本数）、
//     channels、hz。返回 false 要求尽快中止（stop 信号），Feed 随即返回 true。
//   - at_eof=true：放开内部保留水位榨干尾部；无法成帧的残字节丢弃并计入 discarded_tail。
//   - Feed 返回 false = 不可恢复解码错误（调用方停泵转 Error）；字节级垃圾/坏帧应内部
//     跳过重同步，不算不可恢复。
//   - Reset()：换曲复位（解码器状态 + 输入缓冲）；discarded_tail 诊断计数跨曲累计不清。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace media {

// 曲目编解码类型：reader 按源推断（.m3u8 → AacAdts，其余 MP3），经 Pump::track_codec
// 随 epoch 握手传给 decoder 线程。
enum class MediaCodec { Mp3, AacAdts };

class MediaDecoder {
 public:
    using FrameFn = std::function<bool(const int16_t* pcm, int frames, int channels, int hz)>;

    virtual bool Feed(const uint8_t* data, size_t len, bool at_eof, const FrameFn& on_frame) = 0;
    virtual void Reset() = 0;
    virtual size_t discarded_tail() const = 0;  // 累计丢弃的无法成帧尾字节（诊断日志用）
    virtual ~MediaDecoder() = default;
};

// 按 codec 创建解码器；不支持的 codec 返回 nullptr（调用方停泵报错，勿裸解引用）。
std::unique_ptr<MediaDecoder> CreateMediaDecoder(MediaCodec codec);

}  // namespace media
