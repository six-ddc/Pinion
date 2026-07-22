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
    std::call_once(flag, [] {
        // 两层都要注册：esp_audio_dec_register_default 注册编解码器本体（MP3/AAC…，
        // 受 CONFIG_AUDIO_DECODER_*_SUPPORT 控制）；simple 版只注册容器解析器
        //（WAV/M4A/TS/OGG）。漏掉前者 open(TYPE_AAC) 直接 NOT_SUPPORT（真机实测 -7）。
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    });
}

class EspDecoder : public MediaDecoder {
 public:
    explicit EspDecoder(MediaCodec codec) : codec_(codec) { out_.resize(kOutBufInit); }
    ~EspDecoder() override { Close(); }

    bool Open() {
        RegisterDefaultDecodersOnce();
        esp_audio_simple_dec_cfg_t cfg = {};
        if (codec_ == MediaCodec::AacAdts) {
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            // dec_cfg 用成员而非栈局部：官方例程里该结构在整个解码期间存活（simp_dec_all_t
            // 函数级作用域），按闭源库可能持有指针的最保守假设对齐生命周期。
            aac_cfg_.aac_plus_enable = true;  // 电台可能是 HE-AAC，一并支持；ADTS 头自带参数
            cfg.dec_cfg = &aac_cfg_;
            cfg.cfg_size = sizeof(aac_cfg_);
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
        if (data != nullptr && len > 0) {
            win_.insert(win_.end(), data, data + len);
            got_data_ = true;
        }
        if (win_.empty()) {
            // win_ 空且非 EOS：无事可做。win_ 空且 EOS 但本曲从未喂过数据：解码器内部
            // 无缓存，同样无需 flush。只有"喂过数据 + 现在到 EOS"才需要跑 flush 榨尾帧。
            if (!at_eof || !got_data_) return true;
            return FlushTail(on_frame);
        }

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
            raw.consumed = 0;
            esp_audio_err_t err = esp_audio_simple_dec_process(dec_, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (!GrowOutBuf(frame.needed_size)) return false;
                continue;
            }
            // 库**不自行推进输入**：官方例程每轮 process 后手动 raw.len -= consumed（注释
            // "In case that input data contain multiple frames"）。漏推进 = 同一窗口反复
            // 解码——真机实测嘟嘟循环音 + 单次 Feed 内跑出 76s 假产出把播放队列灌爆。
            const uint32_t consumed = raw.consumed;
            raw.buffer += consumed;
            raw.len -= consumed;
            if (err == ESP_AUDIO_ERR_DATA_LACK) break;  // 余量不足一帧，留给下次
            if (err != ESP_AUDIO_ERR_OK) {
                // 数据错误：解析器完全没消费时手动跳 1 字节重同步；已消费 consumed>0 时
                // 说明库自己推进过边界，不再额外 +1（否则会吃掉下一有效帧的首字节）。
                // 连续垃圾超限判不可解流（典型：非音频内容被当作码流喂入）。
                if (raw.len == 0) break;
                if (consumed == 0) {
                    raw.buffer += 1;
                    raw.len -= 1;
                }
                if (++garbage_bytes_ > kMaxGarbageBytes) {
                    ESP_LOGE(TAG, "stream undecodable: %uB continuous garbage (err=%d)",
                             (unsigned)garbage_bytes_, (int)err);
                    return false;
                }
                continue;
            }
            if (frame.decoded_size > 0) {
                garbage_bytes_ = 0;
                if (!EmitFrame(frame, on_frame)) aborted = true;  // 上层要求中止（stop）
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
        info_logged_ = false;
        got_data_ = false;
    }

    size_t discarded_tail() const override { return discarded_tail_; }

 private:
    void Close() {
        if (dec_ != nullptr) {
            esp_audio_simple_dec_close(dec_);
            dec_ = nullptr;
        }
    }

    // BUFF_NOT_ENOUGH 扩容重试：needed_size 必须严格大于当前容量（否则是异常值，扩了也
    // 白扩、会无限 continue 自旋），且不超 256KB 上限；两者违反都判不可恢复错误。
    bool GrowOutBuf(uint32_t needed_size) {
        constexpr size_t kMaxOutBuf = 256 * 1024;
        if (needed_size <= out_.size() || needed_size > kMaxOutBuf) {
            ESP_LOGE(TAG, "BUFF_NOT_ENOUGH resize rejected: needed=%u cur=%u cap=%u", (unsigned)needed_size,
                     (unsigned)out_.size(), (unsigned)kMaxOutBuf);
            return false;
        }
        out_.resize(needed_size);  // 扩容重试，输入位置不动
        return true;
    }

    // 把 frame（已确认 decoded_size>0）交给上层；失败返回 false（流参数异常，不可恢复）。
    bool EmitFrame(const esp_audio_simple_dec_out_t& frame, const FrameFn& on_frame) {
        esp_audio_simple_dec_info_t info = {};
        if (esp_audio_simple_dec_get_info(dec_, &info) != ESP_AUDIO_ERR_OK || info.channel == 0 ||
            info.sample_rate == 0 || info.bits_per_sample != 16) {
            ESP_LOGE(TAG, "bad stream info: rate=%u ch=%u bits=%u", (unsigned)info.sample_rate,
                     (unsigned)info.channel, (unsigned)info.bits_per_sample);
            return false;
        }
        if (!info_logged_) {
            info_logged_ = true;  // 每曲首帧打一次流参数（真机排查解码配置用）
            ESP_LOGI(TAG, "stream: rate=%u ch=%u bits=%u frame=%uB dec_out=%uB", (unsigned)info.sample_rate,
                     (unsigned)info.channel, (unsigned)info.bits_per_sample, (unsigned)info.frame_size,
                     (unsigned)frame.decoded_size);
        }
        const int frames = (int)(frame.decoded_size / (2u * info.channel));
        return on_frame((const int16_t*)out_.data(), frames, (int)info.channel, (int)info.sample_rate);
    }

    // win_ 已空时的 EOS flush：库可能内部缓存了尚未吐出的尾帧（如容器解析器的
    // look-ahead），靠 len=0 + eos=true 反复 process 直到无更多输出才算榨干。
    bool FlushTail(const FrameFn& on_frame) {
        for (;;) {
            esp_audio_simple_dec_raw_t raw = {};
            raw.eos = true;
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = out_.data();
            frame.len = (uint32_t)out_.size();
            esp_audio_err_t err = esp_audio_simple_dec_process(dec_, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (!GrowOutBuf(frame.needed_size)) return false;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) break;  // 榨干完毕
            garbage_bytes_ = 0;
            if (!EmitFrame(frame, on_frame)) break;  // 上层要求中止：停止 flush 即可
        }
        return true;
    }

    MediaCodec codec_;
    esp_aac_dec_cfg_t aac_cfg_ = {};  // 生命周期须覆盖整个解码期（见 Open 注释）
    bool info_logged_ = false;
    bool got_data_ = false;      // 本曲是否喂过任何字节（决定 EOS 时是否需要 flush）
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
