// 火山引擎流式语音识别（SAUC bigmodel v3，设备直连）。
//
// 会话模型：一次识别 = 一条 WSS 连接。
//   volc_asr_start() → volc_asr_feed()×N → volc_asr_stop() → on_final
// 采用调用方推流（push）：feed 收 16kHz/16bit/mono PCM（与 mhal::audio
// ReadPcm 输出一致），组件内部聚成 200ms 段 gzip 后上传。
//
// 回调在 WebSocket 客户端任务上下文触发：禁止耗时/阻塞操作；text 指针仅在
// 回调内有效。text 为"截至目前的完整识别文本"（服务端全量下发，非增量）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// on_delta 的 committed_bytes 取此值表示"本帧无 definite 分句信息"（服务端未下发
// utterances，或 sim/selftest 无此概念）：调用方应退回自己的默认高亮策略。
#define VOLC_ASR_COMMITTED_UNKNOWN SIZE_MAX

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 识别中间结果（对齐参考协议 asr_delta 语义）。committed_bytes = text 中已
    // 定稿（服务端 utterances 里 definite=true 的分句）前缀的字节数，即"不会再改"
    // 的部分长度；[committed_bytes, strlen(text)) 是仍可能被回改的未定稿尾部。
    // 无 definite 信息时为 VOLC_ASR_COMMITTED_UNKNOWN。
    void (*on_delta)(const char* text, size_t committed_bytes, void* ctx);
    // 最终结果（对齐 asr_final；一次会话恰好触发一次，除非出错/中止）
    void (*on_final)(const char* text, void* ctx);
    // 错误：code 为火山错误码（本地错误为负的 esp_err_t）
    void (*on_error)(int code, const char* msg, void* ctx);
    void* ctx;
} volc_asr_callbacks_t;

// 建连 + 发送 full client request，阻塞至就绪（约数百 ms，TLS 握手）。
// 需网络已连通、volc_keys.h 已配置。同一时刻仅允许一个会话。
esp_err_t volc_asr_start(const volc_asr_callbacks_t* cbs);

// 推送 mic PCM（16kHz 16bit mono）。仅把 PCM 拷入内部有界队列即返回（非阻塞、
// 不做网络 I/O），由组件的上行任务取出聚成 200ms 段 gzip 后上传——采集任务
// 绝不会被网络阻塞回压。队列满（网络长时间阻塞）则丢弃本帧并计数告警。
//
// 并发不变量：volc_asr_feed 不得与 volc_asr_stop / volc_asr_abort 并发。调用方
// 必须先停止推流（如 mhal::audio_pipeline::StopCapture() 确保采集任务已退出、
// 不再有 feed 调用）再调 stop/abort。stop/abort 会让上行任务独占排空队列并
// 上传末段，并发的 feed 会与之竞争队列/收尾状态。
esp_err_t volc_asr_feed(const int16_t* pcm, size_t samples);

// 结束音频：请求上行任务把队列剩余 PCM 搬完、残余作为末段（负序号）上传，
// 再等待服务端 final。final_timeout_ms 内收到 final 返回 ESP_OK（on_final 已
// 触发），超时返回 ESP_ERR_TIMEOUT。无论结果如何，返回后会话已释放。传 0
// 表示不等 final（仍会等上行任务发完末段）。见上面的并发不变量。
esp_err_t volc_asr_stop(uint32_t final_timeout_ms);

// 立即中止并释放会话，不触发 on_final。
void volc_asr_abort(void);

bool volc_asr_is_active(void);

#ifdef __cplusplus
}
#endif
