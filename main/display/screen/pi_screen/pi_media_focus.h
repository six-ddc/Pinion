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

// 排队意图取值：>=0 = PlayIndex 目标；负值哨兵 = 等效控制动作（-1 保留为"无意图"）。
// resume/next/prev 与起播同罪：都经 StartPump 的 FlushPlayback，回合中立即执行同样掐 TTS。
#define PI_MEDIA_QUEUE_RESUME (-2)
#define PI_MEDIA_QUEUE_NEXT (-3)
#define PI_MEDIA_QUEUE_PREV (-4)

// LLM 回合中触发的起播/换曲意图（media 工具 mode:'play' / mode:'control'，worker 线程
// 调用）：回合中立即执行会 FlushPlayback 把正在/即将播报的 TTS 掐成整段无声，故 play
// 调用方只 StagePlaylist(items, -1) 暂存列表，把起播索引（或上面的动作哨兵）排到这里；
// Resume 检查通过（播报已排空）时由本模块代为执行。后到意图覆盖先到；执行时若 media
// 已在 Playing（等待期间用户手动开播），play/resume 意图作废尊重用户选择，next/prev
// 仍执行（换曲对"已在播"依然成立）。
void pi_media_focus_queue_play(int index);

// 取走排队意图（返回排队值，-1 = 无）。用户在内置播放器（迷你条/Now-Playing 页）按播放
// 键时用：意图在场则立即播点名那首，而不是落进 Toggle 的 Stopped 分支从 index 0 起播。
int pi_media_focus_take_queued_play(void);

// 作废排队中的起播意图（用户明确 stop/pause 时调用——"叫停"之后音乐在回合结束
// 反而响起是最糟的意外）。无意图时 no-op。
void pi_media_focus_clear_queued_play(void);

#ifdef __cplusplus
}
#endif
