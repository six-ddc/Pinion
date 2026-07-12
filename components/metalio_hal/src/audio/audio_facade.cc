#include "metalio_hal/audio.h"

#include <cstring>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
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

void EnableOutput(bool enable) {
    Impl()->EnableOutput(enable);
    // PA 跟随播放开关（开机不常开）：闲置断电由播放任务触发，消除功放
    // 持续放大 BT codec 空闲底噪的滋滋声。
    IOExpander::getInstance().setLevel(IOExpander::Pin::PA, enable);
    if (enable) {
        vTaskDelay(pdMS_TO_TICKS(30));  // 功放上电稳定，避免吞掉首音
    }
}

int ReadPcm(int16_t* dst, int samples) {
    // codec 输入可能是交错多声道（通道0=mic，其余为回采参考，旧 AudioService
    // 的拆法）：按声道数整读一帧，再抽取 mic 声道还原成单声道。
    const int ch = Impl()->input_channels();
    std::vector<int16_t> buf(samples * ch);
    if (!Impl()->InputData(buf)) {
        return 0;
    }
    if (ch <= 1) {
        std::memcpy(dst, buf.data(), samples * sizeof(int16_t));
    } else {
        for (int i = 0; i < samples; i++) {
            dst[i] = buf[(size_t)i * ch];
        }
    }
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
