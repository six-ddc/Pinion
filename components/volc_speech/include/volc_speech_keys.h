// volc_speech_keys.h — 火山引擎语音密钥的运行期注入口。
//
// 固件里不打包密钥：App Key / Access Key 由用户在 Web 后台的配置页填写、存 NVS，
// 开机时 main.cc 从 device_config 读出并调 volc_speech_set_keys() 注入本组件。
// ASR 与 TTS 在每次建 WSS 连接时读取（握手头 X-Api-App-Key / X-Api-Access-Key）。
//
// 生命周期：只在开机注入一次（改配置走后台保存 → 设备重启），因此内部用固定缓冲
// 存储、读取端拿到的 const char* 始终有效。未注入时 volc_asr_start /
// volc_tts_speak_begin 直接失败并 LOGE，不会拿空密钥去握手。
//
// 红线：key 值绝不出现在日志里。

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 注入密钥。任一参数为 NULL/空串 → 该项视为未配置。超长按 511 字节截断。
void volc_speech_set_keys(const char* app_key, const char* access_key);

// 两个密钥都非空 → true。
bool volc_speech_keys_ready(void);

// 供 ASR/TTS 拼握手头；未配置时返回空串（非 NULL）。
const char* volc_speech_app_key(void);
const char* volc_speech_access_key(void);

#ifdef __cplusplus
}
#endif
