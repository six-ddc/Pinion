// media_decoder_minimp3 — MediaDecoder 的 minimp3 实现（MP3）。sim 端使用；真机已切
// espressif/esp_audio_codec（media_decoder_esp.cc），本文件不进设备固件。
//
// 滑窗语义自 media_pump.cc 原 DecoderRun 原样搬入（Stage 1 重构，金标 WAV 逐字节回归）：
// minimp3 需要看到当前帧**及其后续字节**才肯出音（比特储备/下一帧同步头校验）；窗内不足
// kDecodeReserve 且未到输入末尾时先积累数据再解码，否则边界帧被判 samples==0 丢音
//（实测 8KB 分块解 sine440 丢 27 帧；保留 16KB 后与整段解码逐帧一致）。at_eof 解除约束
// 榨干剩余，无法成帧的尾字节丢弃计入 discarded_tail。
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "media_decoder.h"

// minimp3：仅在本编译单元展开实现（其余 TU 只用声明）。不用其 stdio 文件 API。
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

namespace media {
namespace {

// 解码窗保留水位（语义见文件头）。
constexpr size_t kDecodeReserve = 16384;

class Minimp3Decoder : public MediaDecoder {
 public:
    // 大块状态全部堆分配：原版 mp3dec_decode_frame 在栈上放 ~16KB scratch，逼得解码线程
    // 要 28KB 连续栈——低内存时 pthread 建不出来（真机实测），故走 _ex 变体 + 堆 scratch。
    Minimp3Decoder()
        : mp3_(new(std::nothrow) mp3dec_t),
          scratch_(new(std::nothrow) mp3dec_scratch_t),
          pcm_(new(std::nothrow) int16_t[MINIMP3_MAX_SAMPLES_PER_FRAME]) {
        win_.reserve(kDecodeReserve * 2);
        if (ok()) mp3dec_init(mp3_);
    }
    ~Minimp3Decoder() override {
        delete mp3_;
        delete scratch_;
        delete[] pcm_;
    }

    bool ok() const { return mp3_ != nullptr && scratch_ != nullptr && pcm_ != nullptr; }

    bool Feed(const uint8_t* data, size_t len, bool at_eof, const FrameFn& on_frame) override {
        if (!ok()) return false;
        if (data != nullptr && len > 0) win_.insert(win_.end(), data, data + len);

        // 榨干滑窗：逐帧解码。仅当窗内保有 kDecodeReserve 字节（保证 minimp3 有足够后瞻）
        // 或已到输入末尾时才解，否则先积累——规避边界帧被判 samples==0 丢音。
        while (!win_.empty() && (win_.size() >= kDecodeReserve || at_eof)) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame_ex(mp3_, scratch_, win_.data(), (int)win_.size(), pcm_, &info);
            if (info.frame_bytes == 0) break;  // 当前窗不足一帧，需更多输入
            win_.erase(win_.begin(), win_.begin() + info.frame_bytes);
            if (samples > 0) {
                if (!on_frame(pcm_, samples, info.channels, info.hz)) return true;  // 上层要求中止
            }
        }
        if (at_eof && !win_.empty()) {
            discarded_tail_ += win_.size();  // 曲末无法成帧的尾部垃圾
            win_.clear();
        }
        return true;
    }

    void Reset() override {
        if (ok()) mp3dec_init(mp3_);
        win_.clear();
    }

    size_t discarded_tail() const override { return discarded_tail_; }

 private:
    mp3dec_t* mp3_;
    mp3dec_scratch_t* scratch_;
    int16_t* pcm_;
    std::vector<uint8_t> win_;  // 滑窗：保留未消费的压缩字节
    size_t discarded_tail_ = 0;
};

}  // namespace

std::unique_ptr<MediaDecoder> CreateMediaDecoder(MediaCodec codec) {
    if (codec != MediaCodec::Mp3) return nullptr;  // AAC：真机走 esp 实现；sim 待 helix impl 接入
    auto dec = std::unique_ptr<Minimp3Decoder>(new Minimp3Decoder());
    if (!dec->ok()) return nullptr;
    return dec;
}

}  // namespace media
