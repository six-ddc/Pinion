#pragma once

#include <cstdint>

class AudioCodec;  // 完整定义见公共头 "audio_codec.h"

// 音频门面：板载走 I2S0 slave 全双工（16kHz，BCLK 12 / WS 10 / DOUT 9 /
// DIN 11），对端是 BT 音频模组的 codec。这是未来 ASR 的喂料/放音入口。
//
// 惰性初始化：首次调用本头任一函数才构造 codec 并使能 I2S 通道；
// 固件不用音频时零开销。音量持久化沿用 NVS "audio"/"output_volume"。
namespace mhal::audio {

// 底层 AudioCodec 实例（BTAudioCodecDuplex）。需要 vector 版
// InputData/OutputData 或采样率等元信息时直接用它。
AudioCodec* Codec();

void EnableInput(bool enable);   // mic 通路
void EnableOutput(bool enable);  // speaker 通路

// 读 mic PCM（16-bit 单声道），返回实际样本数；未 EnableInput 时返回 0。
int ReadPcm(int16_t* dst, int samples);
// 写 speaker PCM，返回实际写入样本数。
int WritePcm(const int16_t* src, int samples);

void SetVolume(int percent, bool persist = true);
int GetVolume();

}  // namespace mhal::audio
