// media_decoder_atb_aac — MediaDecoder 的 sim 端 AAC(ADTS) 实现：macOS 原生 AudioToolbox
//（AudioConverter）。仅宿主模拟器使用；真机走 esp_audio_codec。
//
// 为什么不用 helix-aac：那是为 32 位嵌入式写的定点库，在 64 位 arm64 宿主上有 UB
//（实测同一流非确定性崩溃/卡死），而 sim 永远跑在 macOS 上——系统解码器 64 位正确、
// 无 vendor、无许可负担。语义对齐 media_decoder_esp.cc 的 AAC 路径：任意分块积累、
// 坏字节跳过重同步、连续垃圾超限判流不可解、at_eof 丢尾计数。
//
// 结构：自己解析 ADTS 帧头（syncword/采样率/声道/帧长——正好复用 ts_demux_test 里
// 验过的帧长链算法），逐帧经 AudioConverterFillComplexBuffer 解成 16-bit 交错 PCM；
// 流参数（采样率/声道）变化时重建 converter。
#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "esp_log.h"

#include "media_decoder.h"

namespace media {
namespace {

const char* TAG = "media_atb";

constexpr size_t kMaxGarbageBytes = 64 * 1024;  // 同 media_decoder_esp
// ADTS 采样率索引表（ISO 14496-3）
constexpr int kAdtsRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                16000, 12000, 11025, 8000,  0,     0,     0,     0};

struct AdtsHeader {
    int frame_len = 0;    // 含头总长
    int header_len = 0;   // 7 或 9（带 CRC）
    int sample_rate = 0;
    int channels = 0;
};

// 解析 buf 处的 ADTS 头；不合法返回 false。调用方保证 buf 至少 7 字节。
bool ParseAdts(const uint8_t* b, size_t avail, AdtsHeader* h) {
    if (avail < 7 || b[0] != 0xFF || (b[1] & 0xF0) != 0xF0) return false;
    const int rate = kAdtsRates[(b[2] >> 2) & 0x0F];
    const int ch = ((b[2] & 0x01) << 2) | (b[3] >> 6);
    const int len = ((b[3] & 0x03) << 11) | (b[4] << 3) | (b[5] >> 5);
    if (rate == 0 || ch == 0 || len < 7) return false;
    h->frame_len = len;
    h->header_len = (b[1] & 0x01) ? 7 : 9;  // protection_absent=1 → 无 CRC → 7
    h->sample_rate = rate;
    h->channels = ch;
    return true;
}

class AtbAacDecoder : public MediaDecoder {
 public:
    ~AtbAacDecoder() override { DestroyConverter(); }

    bool Feed(const uint8_t* data, size_t len, bool at_eof, const FrameFn& on_frame) override {
        if (data != nullptr && len > 0) win_.insert(win_.end(), data, data + len);

        size_t off = 0;
        for (;;) {
            // 同步字扫描（坏字节逐位跳过，计入垃圾额度）
            AdtsHeader h;
            bool synced = false;
            while (win_.size() - off >= 7) {
                if (ParseAdts(win_.data() + off, win_.size() - off, &h)) {
                    synced = true;
                    break;
                }
                off++;
                if (++garbage_bytes_ > kMaxGarbageBytes) {
                    ESP_LOGE(TAG, "stream undecodable: %uB continuous garbage", (unsigned)garbage_bytes_);
                    return false;
                }
            }
            if (!synced || win_.size() - off < (size_t)h.frame_len) break;  // 不足整帧，等更多输入

            if (!EnsureConverter(h.sample_rate, h.channels)) return false;
            const uint8_t* payload = win_.data() + off + h.header_len;
            const size_t payload_len = (size_t)(h.frame_len - h.header_len);
            bool decode_ok = false;
            if (!DecodeOnePacket(payload, payload_len, h, on_frame, &decode_ok)) return true;  // 上层要求中止
            if (decode_ok) garbage_bytes_ = 0;
            // 坏帧：converter 报错也照样消费该帧字节（ADTS 帧长链自带边界），继续下一帧
            off += (size_t)h.frame_len;
        }

        win_.erase(win_.begin(), win_.begin() + off);
        if (at_eof) {
            discarded_tail_ += win_.size();
            win_.clear();
        }
        return true;
    }

    void Reset() override {
        if (conv_ != nullptr) AudioConverterReset(conv_);
        win_.clear();
        garbage_bytes_ = 0;
    }

    size_t discarded_tail() const override { return discarded_tail_; }

 private:
    // converter 输入回调：一次交出恰好一个 AAC packet；无货时返回哨兵错误终止本次 Fill。
    struct FillCtx {
        const uint8_t* pkt = nullptr;
        size_t len = 0;
        AudioStreamPacketDescription desc = {};
        bool consumed = false;
    };
    static constexpr OSStatus kNoMoreData = 'nomo';

    static OSStatus FillCb(AudioConverterRef, UInt32* num_pkts, AudioBufferList* buf,
                           AudioStreamPacketDescription** descs, void* ud) {
        auto* ctx = static_cast<FillCtx*>(ud);
        if (ctx->consumed) {
            *num_pkts = 0;
            return kNoMoreData;
        }
        ctx->consumed = true;
        buf->mNumberBuffers = 1;
        buf->mBuffers[0].mData = (void*)ctx->pkt;
        buf->mBuffers[0].mDataByteSize = (UInt32)ctx->len;
        buf->mBuffers[0].mNumberChannels = 0;
        ctx->desc.mStartOffset = 0;
        ctx->desc.mDataByteSize = (UInt32)ctx->len;
        ctx->desc.mVariableFramesInPacket = 0;
        if (descs != nullptr) *descs = &ctx->desc;
        *num_pkts = 1;
        return noErr;
    }

    bool EnsureConverter(int rate, int channels) {
        if (conv_ != nullptr && rate == rate_ && channels == ch_) return true;
        DestroyConverter();
        AudioStreamBasicDescription in = {};
        in.mFormatID = kAudioFormatMPEG4AAC;
        in.mSampleRate = rate;
        in.mChannelsPerFrame = (UInt32)channels;
        in.mFramesPerPacket = 1024;
        AudioStreamBasicDescription out = {};
        out.mFormatID = kAudioFormatLinearPCM;
        out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        out.mSampleRate = rate;
        out.mChannelsPerFrame = (UInt32)channels;
        out.mBitsPerChannel = 16;
        out.mBytesPerFrame = (UInt32)(2 * channels);
        out.mBytesPerPacket = out.mBytesPerFrame;
        out.mFramesPerPacket = 1;
        const OSStatus st = AudioConverterNew(&in, &out, &conv_);
        if (st != noErr || conv_ == nullptr) {
            ESP_LOGE(TAG, "AudioConverterNew failed: %d (rate=%d ch=%d)", (int)st, rate, channels);
            conv_ = nullptr;
            return false;
        }
        rate_ = rate;
        ch_ = channels;
        pcm_.resize((size_t)2048 * channels);  // 1024 帧样本 ×2 余量（SBR 双倍时也够）
        return true;
    }

    void DestroyConverter() {
        if (conv_ != nullptr) {
            AudioConverterDispose(conv_);
            conv_ = nullptr;
        }
    }

    // 解一个 AAC packet。返回 false = 上层要求中止（on_frame 返回 false）；
    // *decode_ok 表示本帧是否成功产出 PCM（失败按坏帧跳过，不算致命）。
    bool DecodeOnePacket(const uint8_t* payload, size_t len, const AdtsHeader& h, const FrameFn& on_frame,
                         bool* decode_ok) {
        *decode_ok = false;
        FillCtx ctx;
        ctx.pkt = payload;
        ctx.len = len;
        UInt32 out_frames = 2048;  // 每声道样本上限
        AudioBufferList out_list;
        out_list.mNumberBuffers = 1;
        out_list.mBuffers[0].mNumberChannels = (UInt32)h.channels;
        out_list.mBuffers[0].mDataByteSize = (UInt32)(pcm_.size() * sizeof(int16_t));
        out_list.mBuffers[0].mData = pcm_.data();
        const OSStatus st = AudioConverterFillComplexBuffer(conv_, FillCb, &ctx, &out_frames, &out_list, nullptr);
        if ((st != noErr && st != kNoMoreData) || out_frames == 0) {
            if (st != noErr && st != kNoMoreData) {
                ESP_LOGW(TAG, "decode frame failed: %d", (int)st);
            }
            return true;  // 坏帧跳过
        }
        *decode_ok = true;
        return on_frame(pcm_.data(), (int)out_frames, h.channels, h.sample_rate);
    }

    AudioConverterRef conv_ = nullptr;
    int rate_ = 0;
    int ch_ = 0;
    std::vector<int16_t> pcm_;
    std::vector<uint8_t> win_;
    size_t discarded_tail_ = 0;
    size_t garbage_bytes_ = 0;
};

}  // namespace

std::unique_ptr<MediaDecoder> CreateSimAacDecoder() {
    return std::unique_ptr<MediaDecoder>(new AtbAacDecoder());
}

}  // namespace media
