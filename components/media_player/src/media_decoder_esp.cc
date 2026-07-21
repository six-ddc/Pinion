// media_decoder_esp — MediaDecoder 的真机实现：espressif/esp_audio_codec 官方解码库
//（esp_audio_simple_dec，独立 API 不依赖 GMF，闭源 .a 按芯片发布）。MP3 与 AAC(ADTS)
// 都走它；minimp3 不进设备固件（仅 sim 用）。
//
// process 契约（官方 simple_decoder_test 范式）：process 自行推进 raw.buffer/len；
// BUFF_NOT_ENOUGH 按 needed_size 扩输出缓冲重试；输入未耗尽期间不得改写其内容。
// 我们自持一段 pending 缓冲积累任意大小分块（解析器自己处理帧边界与垃圾重同步，
// 无 minimp3 式保留水位需求）。
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_log.h"
#include "impl/esp_aac_dec.h"

#include "media_decoder.h"

namespace media {
namespace {

const char* TAG = "media_dec";

// 输出 PCM 缓冲初值：MP3 满帧 1152 样本 × 2ch × 2B = 4.6KB、AAC-LC 1024×2×2 = 4KB，
// 8KB 起步都够；HE-AAC 双倍帧长由 BUFF_NOT_ENOUGH 路径按 needed_size 扩容兜底。
constexpr size_t kOutBufInit = 8192;
// 连续垃圾字节容忍上限：process 持续报错但毫无消费时逐字节跳过重同步，超过此累计量
// 判定流不可解（如把 m3u8 文本喂给了 MP3 解码器），返回不可恢复错误停泵。
constexpr size_t kMaxGarbageBytes = 64 * 1024;

void RegisterDefaultDecodersOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] { esp_audio_simple_dec_register_default(); });
}

class EspDecoder : public MediaDecoder {
 public:
    explicit EspDecoder(MediaCodec codec) : codec_(codec) { out_.resize(kOutBufInit); }
    ~EspDecoder() override { Close(); }

    bool Open() {
        RegisterDefaultDecodersOnce();
        esp_audio_simple_dec_cfg_t cfg = {};
        esp_aac_dec_cfg_t aac_cfg = {};
        if (codec_ == MediaCodec::AacAdts) {
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            aac_cfg.aac_plus_enable = true;  // 电台可能是 HE-AAC，一并支持；ADTS 头自带参数
            cfg.dec_cfg = &aac_cfg;
            cfg.cfg_size = sizeof(aac_cfg);
        } else {
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
        esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &dec_);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "simple_dec open failed: %d (codec=%d)", (int)err, (int)codec_);
            dec_ = nullptr;
            return false;
        }
        return true;
    }

    bool Feed(const uint8_t* data, size_t len, bool at_eof, const FrameFn& on_frame) override {
        if (dec_ == nullptr) return false;
        if (data != nullptr && len > 0) win_.insert(win_.end(), data, data + len);
        if (win_.empty()) return true;

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = win_.data();
        raw.len = (uint32_t)win_.size();
        raw.eos = at_eof;
        bool aborted = false;
        while (raw.len > 0 && !aborted) {
            const uint32_t len_before = raw.len;
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = out_.data();
            frame.len = (uint32_t)out_.size();
            esp_audio_err_t err = esp_audio_simple_dec_process(dec_, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                out_.resize(frame.needed_size);  // 扩容重试，输入位置不动
                continue;
            }
            if (err == ESP_AUDIO_ERR_DATA_LACK) break;  // 余量不足一帧，留给下次
            if (err != ESP_AUDIO_ERR_OK) {
                // 数据错误：解析器没有消费任何字节时手动跳 1 字节重同步；连续垃圾超限
                // 判不可解流（典型：非音频内容被当作码流喂入）。
                if (raw.len == 0) break;
                raw.buffer += 1;
                raw.len -= 1;
                if (++garbage_bytes_ > kMaxGarbageBytes) {
                    ESP_LOGE(TAG, "stream undecodable: %uB continuous garbage (err=%d)",
                             (unsigned)garbage_bytes_, (int)err);
                    return false;
                }
                continue;
            }
            if (frame.decoded_size > 0) {
                garbage_bytes_ = 0;
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(dec_, &info) != ESP_AUDIO_ERR_OK || info.channel == 0 ||
                    info.sample_rate == 0 || info.bits_per_sample != 16) {
                    ESP_LOGE(TAG, "bad stream info: rate=%u ch=%u bits=%u", (unsigned)info.sample_rate,
                             (unsigned)info.channel, (unsigned)info.bits_per_sample);
                    return false;
                }
                const int frames = (int)(frame.decoded_size / (2u * info.channel));
                if (!on_frame((const int16_t*)out_.data(), frames, (int)info.channel, (int)info.sample_rate)) {
                    aborted = true;  // 上层要求中止（stop）：丢弃剩余输入即可
                }
            } else if (raw.len == len_before) {
                break;  // 无消费亦无输出：防御性防自旋（正常路径不应到达）
            }
        }

        // 保留未消费尾部（process 已把 raw.buffer/len 推进到未消费处）。
        const size_t leftover = aborted ? 0 : raw.len;
        if (leftover > 0 && !at_eof) {
            std::memmove(win_.data(), raw.buffer, leftover);
            win_.resize(leftover);
        } else {
            if (at_eof) discarded_tail_ += leftover;
            win_.clear();
        }
        return true;
    }

    void Reset() override {
        if (dec_ != nullptr) esp_audio_simple_dec_reset(dec_);
        win_.clear();
        garbage_bytes_ = 0;
    }

    size_t discarded_tail() const override { return discarded_tail_; }

 private:
    void Close() {
        if (dec_ != nullptr) {
            esp_audio_simple_dec_close(dec_);
            dec_ = nullptr;
        }
    }

    MediaCodec codec_;
    esp_audio_simple_dec_handle_t dec_ = nullptr;
    std::vector<uint8_t> win_;   // pending 输入（未消费的压缩字节）
    std::vector<uint8_t> out_;   // 解码输出 PCM 缓冲（按 needed_size 扩容）
    size_t discarded_tail_ = 0;
    size_t garbage_bytes_ = 0;   // 连续无法解码且零消费的字节计数
};

}  // namespace

std::unique_ptr<MediaDecoder> CreateMediaDecoder(MediaCodec codec) {
    auto dec = std::unique_ptr<EspDecoder>(new (std::nothrow) EspDecoder(codec));
    if (dec == nullptr || !dec->Open()) return nullptr;
    return dec;
}

}  // namespace media
