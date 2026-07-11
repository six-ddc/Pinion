// 火山引擎 v3 二进制 WebSocket 协议：帧编解码 + gzip。
//
// 字节级对齐已验证可用的参考实现 ai-chat-esp32/service/src/asr.ts 与
// service/src/volcengine/protocols.ts：
//   帧 = 4B header | [seq int32 BE] | [error uint32 BE] | [event int32 BE]
//        | [sessionId len+bytes] | payload len(uint32 BE)+bytes
//   header[0] = version<<4 | headerSize(4B 单位)
//   header[1] = msgType<<4 | flags
//   header[2] = serialization<<4 | compression
//   header[3] = 保留 0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// message type（header[1] 高 4 位）
enum {
    VOLC_MSG_FULL_CLIENT       = 0x1,
    VOLC_MSG_AUDIO_ONLY_CLIENT = 0x2,
    VOLC_MSG_FULL_SERVER       = 0x9,
    VOLC_MSG_AUDIO_ONLY_SERVER = 0xB,
    VOLC_MSG_FRONTEND_SERVER   = 0xC,
    VOLC_MSG_ERROR             = 0xF,
};

// flags（header[1] 低 4 位）
enum {
    VOLC_FLAG_NO_SEQ     = 0x0,
    VOLC_FLAG_POS_SEQ    = 0x1,  // 携带正序号
    VOLC_FLAG_LAST       = 0x2,  // 末包（服务端 ASR final）
    VOLC_FLAG_NEG_SEQ    = 0x3,  // 末包 + 负序号（客户端 ASR 最后一段音频）
    VOLC_FLAG_WITH_EVENT = 0x4,  // 携带 event（TTS 双向流式）
};

// serialization / compression（header[2] 高/低 4 位）
enum {
    VOLC_SER_RAW   = 0x0,
    VOLC_SER_JSON  = 0x1,
    VOLC_COMP_NONE = 0x0,
    VOLC_COMP_GZIP = 0x1,
};

// TTS 双向流式 event 类型（仅列本组件用到的）
enum {
    VOLC_EVT_START_CONNECTION    = 1,
    VOLC_EVT_FINISH_CONNECTION   = 2,
    VOLC_EVT_CONNECTION_STARTED  = 50,
    VOLC_EVT_CONNECTION_FAILED   = 51,
    VOLC_EVT_CONNECTION_FINISHED = 52,
    VOLC_EVT_START_SESSION       = 100,
    VOLC_EVT_CANCEL_SESSION      = 101,
    VOLC_EVT_FINISH_SESSION      = 102,
    VOLC_EVT_SESSION_STARTED     = 150,
    VOLC_EVT_SESSION_CANCELED    = 151,
    VOLC_EVT_SESSION_FINISHED    = 152,
    VOLC_EVT_SESSION_FAILED      = 153,
    VOLC_EVT_USAGE_RESPONSE      = 154,
    VOLC_EVT_TASK_REQUEST        = 200,
    VOLC_EVT_TTS_SENTENCE_START  = 350,
    VOLC_EVT_TTS_SENTENCE_END    = 351,
    VOLC_EVT_TTS_RESPONSE        = 352,
};

typedef struct {
    uint8_t msg_type;
    uint8_t flags;
    uint8_t serialization;
    uint8_t compression;
    bool has_seq;
    int32_t sequence;
    bool is_last;        // flags bit1：服务端 ASR 末帧
    bool has_event;
    int32_t event;
    uint32_t error_code; // msg_type == VOLC_MSG_ERROR 时有效
    char session_id[64];
    const uint8_t* payload;  // 指向入参 data 内部，不拷贝
    uint32_t payload_len;
} volc_frame_t;

// 解析服务端帧。字段读取顺序对齐 protocols.ts::unmarshalMessage：
// seq（flags 为 POS/NEG_SEQ）→ error code（Error 帧）→ event/sessionId/
// connectId（flags 为 WITH_EVENT）→ payload。成功返回 true。
bool volc_frame_parse(const uint8_t* data, size_t len, volc_frame_t* out);

// —— 构帧（均返回 malloc 缓冲，调用方 free；失败返回 NULL）——

// ASR full client request：JSON 载荷内部先 gzip。
uint8_t* volc_build_asr_full_request(int32_t seq, const char* json,
                                     size_t* out_len);
// ASR audio-only：PCM 段内部先 gzip；is_last 时按参考实现发 -seq。
uint8_t* volc_build_asr_audio(int32_t seq, bool is_last, const uint8_t* pcm,
                              size_t pcm_len, size_t* out_len);
// TTS 事件帧（full client request + WITH_EVENT，JSON 不压缩）。
// session_id 传 NULL 表示连接级事件（StartConnection/FinishConnection）。
uint8_t* volc_build_tts_event(int32_t event, const char* session_id,
                              const char* json, size_t* out_len);

// —— gzip（malloc 缓冲，调用方 free；失败返回 NULL）——
uint8_t* volc_gzip(const uint8_t* in, size_t in_len, size_t* out_len);
uint8_t* volc_gunzip(const uint8_t* in, size_t in_len, size_t* out_len);

// 36 字符 UUIDv4 + '\0'
void volc_gen_uuid(char out[37]);

#ifdef __cplusplus
}
#endif
