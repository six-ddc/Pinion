// media_resampler — 把任意采样率/声道的解码 PCM 归一到板载 codec 的 16kHz mono。
// 处理链（每流独立状态，可 Reset 复用于换曲）：
//   1) stereo→mono 降混：(L+R)/2（单声道直通）。
//   2) 前置抗混叠低通：4 级级联 biquad（8 阶 Butterworth，RBJ 系数，截止 ~6.6kHz）：在
//      **输入率**下压掉将要落到 16k 奈奎斯特(8kHz)以上的频段，避免降采样折叠出混叠噪声。
//      单只 biquad 12dB/oct 对 48k→16k 这种 3:1 降采样远远不够（9kHz 折回 7kHz 只衰 6dB，
//      听感是金属味毛刺）；8 阶把折回带内的残留压到 -18dB 以下（10kHz 以上 -29dB 以下）。
//      输入率本身已足够低（截止逼近其奈奎斯特）时自动旁路。
//   3) 32-bit 定点相位累加线性插值重采样到 16kHz：ratio = in_hz/out_hz（Q16），
//      逐输入样本推进、在 [prev,cur] 区间按 frac 线性插值产出输出样本。
//
// 设计取向：够用即可（线性插值 + biquad 级联），不是录音棚级 SRC——目标是音乐/电台
// 可听、音准正确（重采样比率精确 → 时长/音高不变），CPU 占用低（P4 上边下边播不吃满）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

class Resampler {
 public:
    Resampler() = default;

    // 处理一帧交错 PCM（frames 个采样帧，每帧 channels 个 int16）：降混→低通→重采样，
    // 产出的 16kHz mono 样本 **追加** 到 out（不清空 out，便于累计）。in_hz/channels 变化
    // （换曲或流内格式跳变）时自动重配滤波器与比率并接续（不清插值相位，避免咔哒）。
    // 返回本次追加的输出样本数。
    size_t Process(const int16_t* interleaved, int frames, int channels, int in_hz,
                   std::vector<int16_t>& out);

    // 复位全部流状态（滤波器记忆 + 插值相位 + 配置）。换曲/seek 前调用。
    void Reset();

 private:
    void Configure(int in_hz, int channels);
    // 对一个输入率下的 mono 样本跑级联低通，返回滤波后样本（旁路时原样返回）。
    int32_t LowPass(int32_t x);
    // 送入一个滤波后的 mono 输入样本，按相位累加产出 0..N 个输出样本追加到 out。
    void Interpolate(int32_t sample, std::vector<int16_t>& out);

    static constexpr int kOutHz = 16000;
    static constexpr uint32_t kOne = 1u << 16;  // Q16 定点的 1.0
    static constexpr int kLpStages = 4;         // 4 级 biquad = 8 阶 Butterworth

    int in_hz_ = 0;
    int channels_ = 0;
    uint32_t ratio_q16_ = kOne;  // in_hz/out_hz，Q16

    // 线性插值相位状态
    int32_t prev_ = 0;         // 上一个输入样本（区间左端）
    bool has_prev_ = false;    // 是否已有 prev_（首样本仅装载不产出）
    uint32_t frac_q16_ = 0;    // 下一个输出样本在当前区间内的相位（Q16）

    // 级联 biquad 低通（每级 Direct Form 1）状态与系数
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 已归一
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;          // 输入/输出历史
    };
    bool lp_enabled_ = false;
    Biquad lp_[kLpStages];
};

}  // namespace media
