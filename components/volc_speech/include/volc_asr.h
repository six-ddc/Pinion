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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 识别中间结果（对齐参考协议 asr_delta 语义）
    void (*on_delta)(const char* text, void* ctx);
    // 最终结果（对齐 asr_final；一次会话恰好触发一次，除非出错/中止）
    void (*on_final)(const char* text, void* ctx);
    // 错误：code 为火山错误码（本地错误为负的 esp_err_t）
    void (*on_error)(int code, const char* msg, void* ctx);
    void* ctx;
} volc_asr_callbacks_t;

// 建连 + 发送 full client request，阻塞至就绪（约数百 ms，TLS 握手）。
// 需网络已连通、volc_keys.h 已配置。同一时刻仅允许一个会话。
esp_err_t volc_asr_start(const volc_asr_callbacks_t* cbs);

// 推送 mic PCM（16kHz 16bit mono）。内部缓冲，满 200ms 即上传一段。
esp_err_t volc_asr_feed(const int16_t* pcm, size_t samples);

// 结束音频：把残余缓冲作为末段（负序号）上传，等待服务端 final。
// final_timeout_ms 内收到 final 返回 ESP_OK（on_final 已触发），超时返回
// ESP_ERR_TIMEOUT。无论结果如何，返回后会话已释放。传 0 表示不等待。
esp_err_t volc_asr_stop(uint32_t final_timeout_ms);

// 立即中止并释放会话，不触发 on_final。
void volc_asr_abort(void);

bool volc_asr_is_active(void);

#ifdef __cplusplus
}
#endif
