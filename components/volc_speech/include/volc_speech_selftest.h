// 设备端自测：录 3 秒 mic → ASR 转写 → 把文本喂 TTS → 扬声器播放。
// 前置条件：网络已连通（mhal::network）、volc_keys.h 已配置。
// 阻塞执行（数十秒量级），仅供接线/联调阶段调用，勿在 UI 任务里跑。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void volc_speech_selftest(void);

#ifdef __cplusplus
}
#endif
