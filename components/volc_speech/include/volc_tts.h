// 火山引擎双向流式语音合成（v3 bidirection，设备直连）。
//
// 会话模型（一次播报 = 一个 session；WSS 连接跨会话复用）：
//   volc_tts_speak_begin() → volc_tts_feed_text()×N（如 LLM text_delta）
//   → volc_tts_speak_end() → 音频继续流式下发并经 mhal::audio 播放
//   → SessionFinished 且播放队列排空后触发 on_finished
// 任意时刻可 volc_tts_stop() 打断（丢弃未播音频，CancelSession）。
//
// 回调在组件内部任务上下文触发，禁止耗时/阻塞操作。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 第一帧音频开始播放（可用于 UI 切"说话中"状态）
    void (*on_audio_start)(void* ctx);
    // 本次播报完整播完（含播放队列排空）
    void (*on_finished)(void* ctx);
    // 错误：code 为火山错误码（本地错误为负的 esp_err_t）
    void (*on_error)(int code, const char* msg, void* ctx);
    void* ctx;
} volc_tts_callbacks_t;

// 开始一次播报会话。必要时建立/复用 WSS 连接（首次约数百 ms），发送
// StartSession 并等待确认。上一会话未结束时返回 ESP_ERR_INVALID_STATE
//（先 volc_tts_stop() 或等 on_finished）。
esp_err_t volc_tts_speak_begin(const volc_tts_callbacks_t* cbs);

// 追加一段合成文本（UTF-8，可为流式增量片段，空白串被忽略）。
esp_err_t volc_tts_feed_text(const char* text_utf8);

// 文本输入完毕（FinishSession）。非阻塞；音频会继续到达并播放。
esp_err_t volc_tts_speak_end(void);

// 阻塞等待本次播报完全结束（on_finished/on_error/stop 之一发生）。
esp_err_t volc_tts_wait_done(uint32_t timeout_ms);

// 打断：立刻停止播放、丢弃缓冲音频、取消服务端会话。连接保留供复用。
void volc_tts_stop(void);

bool volc_tts_is_speaking(void);

// 关闭底层连接并释放全部资源（通常不需要；组件默认复用连接）。
void volc_tts_shutdown(void);

#ifdef __cplusplus
}
#endif
