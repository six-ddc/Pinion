#pragma once

// ---------------------------------------------------------------------------
// pi_media_focus —— TTS/ASR ↔ 音乐的焦点仲裁（Stage D）。
//
// 策略：语音优先，音乐自动让路，事后恢复。
//   Suspend 触发点：ASR 开始聆听（barge-in/按住说话）、TTS 首帧音频即将出声。
//   Resume 触发点（去抖 ~500ms 后才真正 Resume；期间若又发生 Suspend 则本次
//   Resume 检查作废，避免同一会话内多段 TTS/多次 ASR 造成"暂停-恢复"抖动）：
//     ASR 结束聆听、TTS 播报排空/出错结束、agent 回合彻底结束（兜底：覆盖本轮
//     从未真正触发 TTS 音频——如纯工具调用回复、或 TTS 被用户关闭——的情况）。
//
// 全部方法均为 no-op-safe：media 处于 Stopped/Error，或本来就不是被本模块
// Suspend 挂起时，Resume 请求在 MediaController 内部即被判定为无操作，不产生
// 副作用/日志噪音。C 链接：pi_agent_task.c（TTS 回调）与 pi_screen.cc（ASR/回合
// 事件）都要调用，前者是纯 C 编译单元。
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// TTS 音频即将出声（首帧播放，一次 run 内只在 volc_tts 的 on_audio_start 触发一次）。
// 由 pi_agent_task 的 TTS 回调调用（该回调跑在 volc_tts 内部任务上下文，非阻塞）。
void pi_media_focus_tts_audio_start(void);

// TTS 播报排空（on_finished）或出错结束（on_error）：安排一次去抖 Resume 检查。
void pi_media_focus_tts_ended(void);

// agent 回合彻底结束（UI_DONE / UI_ERROR）：兜底安排一次去抖 Resume 检查，覆盖本轮
// 从未触发 TTS 音频的情况（纯工具调用回复 / TTS 被关闭）。
void pi_media_focus_turn_ended(void);

// ASR 开始聆听（barge-in / 按住说话）：立即 Suspend。
void pi_media_focus_asr_start(void);

// ASR 结束聆听（Finish/Cancel）：安排一次去抖 Resume 检查。
void pi_media_focus_asr_ended(void);

#ifdef __cplusplus
}
#endif
