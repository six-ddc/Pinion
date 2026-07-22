#include "media_resampler.h"

#include <cmath>

namespace media {

void Resampler::Reset() {
    in_hz_ = 0;
    channels_ = 0;
    ratio_q16_ = kOne;
    prev_ = 0;
    has_prev_ = false;
    frac_q16_ = 0;
    lp_enabled_ = false;
    for (int i = 0; i < kLpStages; i++) lp_[i] = Biquad{};
}

void Resampler::Configure(int in_hz, int channels) {
    in_hz_ = in_hz > 0 ? in_hz : kOutHz;
    channels_ = channels > 0 ? channels : 1;
    // 重采样比率（Q16）：输出一个样本，输入相位推进 in_hz/out_hz。
    ratio_q16_ = (uint32_t)(((uint64_t)in_hz_ << 16) / kOutHz);

    // 抗混叠：4 级 RBJ 低通 biquad 级联成 8 阶 Butterworth，截止 6.6kHz。
    // 每级 Q 为 8 阶 Butterworth 极点对的标准值；级按 Q 从低到高排，减小级间过冲。
    // 只在真下采样（in_hz_ > kOutHz）时开启——16kHz 直通（in_hz_ == kOutHz）与所有
    // 上采样场景（8k/11.025k/12k → 16k）没有折叠风险，压低通反而白削 6.6-8kHz 频段。
    // 截止逼近输入奈奎斯特时旁路（输入率已够低，降采样不会折叠），以 0.95 奈奎斯特
    // 为界避免系数病态。
    static const double kStageQ[kLpStages] = {0.50980, 0.60134, 0.89998, 2.56292};
    const double fc = 6600.0;
    const double fs = (double)in_hz_;
    if (in_hz_ <= kOutHz || fc >= fs * 0.5 * 0.95) {
        lp_enabled_ = false;
    } else {
        const double w0 = 2.0 * M_PI * fc / fs;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        for (int i = 0; i < kLpStages; i++) {
            const double alpha = sinw0 / (2.0 * kStageQ[i]);
            const double a0 = 1.0 + alpha;
            lp_[i].b0 = (float)(((1.0 - cosw0) / 2.0) / a0);
            lp_[i].b1 = (float)((1.0 - cosw0) / a0);
            lp_[i].b2 = (float)(((1.0 - cosw0) / 2.0) / a0);
            lp_[i].a1 = (float)((-2.0 * cosw0) / a0);
            lp_[i].a2 = (float)((1.0 - alpha) / a0);
        }
        lp_enabled_ = true;
    }
    // 换配置时清滤波器记忆，避免旧率的历史样本污染新率响应。
    for (int i = 0; i < kLpStages; i++) {
        lp_[i].x1 = lp_[i].x2 = lp_[i].y1 = lp_[i].y2 = 0;
    }
}

int32_t Resampler::LowPass(int32_t x) {
    if (!lp_enabled_) return x;
    // 级联 Direct Form 1：y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2，逐级串联。
    // 级间不截断（高 Q 级瞬态可能过冲 int16 范围，float 域无损通过），只在末端钳位。
    float v = (float)x;
    for (int i = 0; i < kLpStages; i++) {
        Biquad& s = lp_[i];
        const float y = s.b0 * v + s.b1 * s.x1 + s.b2 * s.x2 - s.a1 * s.y1 - s.a2 * s.y2;
        s.x2 = s.x1;
        s.x1 = v;
        s.y2 = s.y1;
        s.y1 = y;
        v = y;
    }
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int32_t)v;
}

void Resampler::Interpolate(int32_t sample, std::vector<int16_t>& out) {
    if (!has_prev_) {
        prev_ = sample;
        has_prev_ = true;
        frac_q16_ = 0;
        return;
    }
    // 当前区间 [prev_, sample] 宽度 = 1.0（一个输入样本间隔，Q16 = kOne）。
    // 产出所有相位落在本区间内的输出样本；相位每次推进 ratio（decimation 时可能一个
    // 区间零产出）。
    while (frac_q16_ < kOne) {
        int32_t o = prev_ + (int32_t)(((int64_t)(sample - prev_) * (int32_t)frac_q16_) >> 16);
        if (o > 32767) o = 32767;
        if (o < -32768) o = -32768;
        out.push_back((int16_t)o);
        frac_q16_ += ratio_q16_;
    }
    frac_q16_ -= kOne;  // 消费掉本输入样本代表的 1.0 相位
    prev_ = sample;
}

size_t Resampler::Process(const int16_t* interleaved, int frames, int channels, int in_hz,
                          std::vector<int16_t>& out) {
    if (interleaved == nullptr || frames <= 0 || channels <= 0) return 0;
    if (in_hz != in_hz_ || channels != channels_) Configure(in_hz, channels);

    const size_t before = out.size();
    for (int f = 0; f < frames; f++) {
        const int16_t* frame = interleaved + (size_t)f * channels;
        int32_t mono;
        if (channels == 1) {
            mono = frame[0];
        } else {
            // 降混：前两声道均值即可覆盖 stereo；多声道取前两路（够用）。
            mono = ((int32_t)frame[0] + (int32_t)frame[1]) / 2;
        }
        Interpolate(LowPass(mono), out);
    }
    return out.size() - before;
}

}  // namespace media
