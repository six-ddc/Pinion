#include "metalio_hal/audio.h"

#include <cstring>
#include <vector>

#include "audio_codec.h"
#include "bt_audio_codec.h"
#include "config.h"

namespace mhal::audio {

namespace {
// 惰性构造 + 一次性 Start()（使能 I2S 通道、从 NVS 恢复音量），与旧固件
// AudioService::Initialize 对 codec 的启动语义一致。
AudioCodec* Impl() {
    static BTAudioCodecDuplex codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                    AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_MIC_GPIO_WS,
                                    AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_DIN);
    static bool started = false;
    if (!started) {
        started = true;
        codec.Start();
    }
    return &codec;
}
}  // namespace

AudioCodec* Codec() { return Impl(); }

void EnableInput(bool enable) { Impl()->EnableInput(enable); }

void EnableOutput(bool enable) { Impl()->EnableOutput(enable); }

int ReadPcm(int16_t* dst, int samples) {
    std::vector<int16_t> buf(samples);
    if (!Impl()->InputData(buf)) {
        return 0;
    }
    std::memcpy(dst, buf.data(), samples * sizeof(int16_t));
    return samples;
}

int WritePcm(const int16_t* src, int samples) {
    std::vector<int16_t> buf(src, src + samples);
    Impl()->OutputData(buf);
    return samples;
}

void SetVolume(int percent, bool persist) {
    // AudioCodec::SetOutputVolume 总是持久化到 NVS "audio"；persist=false
    // 时先记住旧值语义不值得复刻，直接沿用旧行为（永远持久化）。
    (void)persist;
    Impl()->SetOutputVolume(percent);
}

int GetVolume() { return Impl()->output_volume(); }

}  // namespace mhal::audio
