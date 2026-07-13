#include "volc_tts.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "metalio_hal/audio_pipeline.h"
#include "volc_proto.h"

#if __has_include("volc_keys.h")
#include "volc_keys.h"
#else
#error "缺少 volc_keys.h：复制 include/volc_keys.h.example 为 include/volc_keys.h 并填入密钥"
#endif

static const char* TAG = "volc_tts";

// 端点/资源/音色与已验证的参考部署一致（ai-chat-esp32 service/.env）
#define TTS_URL "wss://openspeech.bytedance.com/api/v3/tts/bidirection"
#define TTS_RESOURCE_ID "seed-tts-2.0"
#define TTS_VOICE "zh_female_xiaohe_uranus_bigtts"
#define TTS_SAMPLE_RATE 16000  // mhal::audio 板载 codec 固定 16kHz，免重采样
#define TTS_CONNECT_TIMEOUT_MS 10000
#define TTS_SEND_TIMEOUT_MS 5000
#define TTS_FEED_TIMEOUT_MS 10000  // 抖动队列满时对搬运任务的最大背压时长
// 下行音频中间缓冲：WS 接收任务把服务端音频帧非阻塞入队到这里，独立搬运任务
// 再阻塞喂给播放管线。绝不能在 WS 接收任务里为播放阻塞——那会冻结这条连接的
// 收/发/keepalive，长消息尾部被服务端当慢消费者掐断（见组件根因分析）。
//
// 火山下发裸 PCM，以约 2.5x 实时速率倾泻，而播放只能 1x 实时消费，差额全压在
// 本缓冲：峰值积压 ≈ 19.2KB × 语音秒数。512KB 只够约 27s 语音就溢出丢帧（表现
// 为播到十几秒后持续卡顿）。4MB ≈ 208s 语音，覆盖任何正常回复。PSRAM。
// 根治之道是让火山改发压缩格式(opus/mp3)+端上解码把带宽降 10 倍（另议）。
#define TTS_AUDIO_RB_BYTES (4 * 1024 * 1024)
#define TTS_AUDIO_CHUNK 4096  // 单次搬运块上限（128ms），与播放管线对齐

#define BIT_WS_CONNECTED BIT0
#define BIT_CONN_STARTED BIT1
#define BIT_SESS_STARTED BIT2
#define BIT_SESS_FINISHED BIT3
#define BIT_SESS_CANCELED BIT4
#define BIT_FAILED BIT5
#define BIT_DONE BIT6           // 本次播报彻底结束（播完/打断/失败）
#define BIT_CONN_FINISHED BIT7  // 服务端 ConnectionFinished（连接级干净收尾）

struct TtsState {
    esp_websocket_client_handle_t ws;
    char* headers;  // 含密钥，仅传给 WS 客户端，不打日志
    bool conn_started;
    EventGroupHandle_t eg;
    SemaphoreHandle_t api_lock;
    volc_tts_callbacks_t cbs;
    char session_id[37];
    volatile bool session_active;
    volatile bool pending_finish;  // SessionFinished 已到，待播放队列排空
    volatile bool discard_audio;   // 打断后丢弃迟到的下行音频
    volatile bool audio_started;
    // 下行音频解耦：WS 接收任务非阻塞入队 → 搬运任务阻塞喂播放管线
    RingbufHandle_t audio_rb;
    TaskHandle_t pump_task;
    volatile bool pump_run;             // 搬运任务运行标志（shutdown 置 false）
    volatile bool pump_alive;           // 搬运任务是否在运行（shutdown join 用）
    volatile bool pump_finish_pending;  // SessionFinished 已到，待 audio_rb 排空后挂排空回调
    uint8_t audio_carry;                // 16bit 对齐用的半字节余数
    volatile bool audio_carry_valid;
    size_t audio_rb_free_empty;         // 空 audio_rb 的空闲字节（判空基线）
    // WS 消息重组
    uint8_t* rx;
    size_t rx_cap;
    size_t rx_expected;
};

static TtsState* s_tts = nullptr;

static void tts_emit_error(TtsState* s, int code, const char* msg,
                           size_t msg_len) {
    char buf[160] = {0};
    if (msg && msg_len) {
        size_t n = msg_len < sizeof(buf) - 1 ? msg_len : sizeof(buf) - 1;
        memcpy(buf, msg, n);
    }
    ESP_LOGE(TAG, "error %d: %s", code, buf);
    if (s->cbs.on_error) s->cbs.on_error(code, buf, s->cbs.ctx);
    s->session_active = false;
    s->pending_finish = false;
    s->discard_audio = true;         // 会话失败：丢弃迟到/残留音频
    s->pump_finish_pending = false;  // 取消待触发的排空
    xEventGroupSetBits(s->eg, BIT_FAILED | BIT_DONE);
}

// 非阻塞入队到中间缓冲；满则短超时兜底，绝不长阻塞 WS 接收任务
static void tts_audio_rb_send(TtsState* s, const uint8_t* buf, size_t n) {
    if (n == 0) return;
    if (xRingbufferSend(s->audio_rb, buf, n, 0) != pdTRUE) {
        // 满（极端超长消息，搬运/播放严重滞后）：短超时兜底，仍不长阻塞 WS 任务
        if (xRingbufferSend(s->audio_rb, buf, n, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "audio_rb full, dropping %u B", (unsigned)n);
        }
    }
}

// 本函数运行在 WS 接收任务上下文（esp_websocket 无独立 event task，handler
// 同步派发）。绝不能在这里为播放阻塞——只做非阻塞入队，立刻让 WS 任务回去读
// socket + 发 keepalive；实际的播放背压由 tts_audio_pump_task 承担。
static void tts_enqueue_audio(TtsState* s, const uint8_t* pcm, size_t len) {
    if (len < 1 || s->discard_audio || !s->audio_rb) return;
    if (!s->audio_started) {
        s->audio_started = true;
        if (s->cbs.on_audio_start) s->cbs.on_audio_start(s->cbs.ctx);
    }
    // 中间缓冲是字节流：若某帧字节数为奇数，直接拼接会让后续 16bit 样本永久
    // 错位（听感为失真/一顿一顿）。用 1 字节 carry 保证送入的字节流始终对齐。
    const uint8_t* src = pcm;
    size_t n = len;
    if (s->audio_carry_valid) {
        uint8_t pair[2] = {s->audio_carry, src[0]};
        tts_audio_rb_send(s, pair, 2);
        src += 1;
        n -= 1;
        s->audio_carry_valid = false;
    }
    size_t even = n & ~(size_t)1;
    if (even) tts_audio_rb_send(s, src, even);
    if (n & 1) {
        s->audio_carry = src[even];
        s->audio_carry_valid = true;
    }
}

// SessionFinished 后由播放管线在队列排空时触发（打断路径上被
// pending_finish=false 短路）
static void tts_on_drained(TtsState* s) {
    if (!s->pending_finish) return;
    s->pending_finish = false;
    ESP_LOGI(TAG, "playback drained");
    if (s->cbs.on_finished) s->cbs.on_finished(s->cbs.ctx);
    xEventGroupSetBits(s->eg, BIT_DONE);
}

// 等搬运任务把中间缓冲丢弃排空（barge-in / 会话切换）。audio_rb 是 BYTEBUF，
// ESP-IDF 只允许它有一个读者，故这里绝不能自己 Receive（会与搬运任务并发读、
// 命中 ringbuf 的 configASSERT 崩溃）——只做只读的空闲查询，靠 discard_audio
// 让搬运任务把残帧丢掉。调用前须已置 discard_audio=true。
static void tts_wait_audio_rb_empty(TtsState* s) {
    if (!s->audio_rb) return;
    s->audio_carry_valid = false;  // 半字节余数一并丢弃
    // 搬运任务在 discard 下不走 FeedPlayback（无阻塞），排空极快；~600ms 上限兜底
    for (int i = 0; i < 60; i++) {
        if (xRingbufferGetCurFreeSize(s->audio_rb) >= s->audio_rb_free_empty) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "audio_rb not drained in time");
}

// 下行音频搬运任务：从中间缓冲取音频、阻塞喂播放管线（背压落在本任务，与 WS
// 接收任务彻底解耦）。audio_rb 排空且上游 SessionFinished 已到时，挂
// OnPlaybackDrained——等 s_play.rb 也排空后才触发 on_finished（两级都空 = 播完）。
static void tts_audio_pump_task(void* arg) {
    auto* s = static_cast<TtsState*>(arg);
    TickType_t dbg_last = xTaskGetTickCount();
    uint32_t dbg_moved = 0, dbg_block_ms = 0, dbg_recv = 0;
    while (s->pump_run) {
        size_t got = 0;
        auto* item = (uint8_t*)xRingbufferReceiveUpTo(
            s->audio_rb, &got, pdMS_TO_TICKS(50), TTS_AUDIO_CHUNK);
        if (item) {
            dbg_recv++;
            if (!s->discard_audio && got >= 2) {
                TickType_t t0 = xTaskGetTickCount();
                mhal::audio_pipeline::FeedPlayback((const int16_t*)item, got / 2,
                                                   TTS_FEED_TIMEOUT_MS);
                dbg_block_ms += (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
                dbg_moved += got;
            }
            vRingbufferReturnItem(s->audio_rb, item);
        } else if (s->pump_finish_pending) {
            // 中间缓冲已排空
            s->pump_finish_pending = false;
            mhal::audio_pipeline::OnPlaybackDrained([s]() { tts_on_drained(s); });
        }
        // 诊断：每秒打印中间缓冲水位 / 本秒搬运量 / 下游背压(FeedPlayback)阻塞时长
        TickType_t now = xTaskGetTickCount();
        if ((now - dbg_last) * portTICK_PERIOD_MS >= 1000) {
            size_t used =
                TTS_AUDIO_RB_BYTES - xRingbufferGetCurFreeSize(s->audio_rb);
            if (dbg_moved > 0 || used > 512) {
                ESP_LOGI(TAG,
                         "pump: audio_rb=%uKB moved=%uB/s feedblock=%ums/s recv=%u",
                         (unsigned)(used / 1024), (unsigned)dbg_moved,
                         (unsigned)dbg_block_ms, (unsigned)dbg_recv);
            }
            dbg_moved = dbg_block_ms = dbg_recv = 0;
            dbg_last = now;
        }
    }
    s->pump_alive = false;
    vTaskDelete(nullptr);
}

static void tts_handle_frame(TtsState* s, const uint8_t* data, size_t len) {
    volc_frame_t f;
    if (!volc_frame_parse(data, len, &f)) {
        ESP_LOGW(TAG, "unparsable frame (%u bytes)", (unsigned)len);
        return;
    }

    if (f.msg_type == VOLC_MSG_ERROR) {
        tts_emit_error(s, (int)f.error_code, (const char*)f.payload,
                       f.payload_len);
        return;
    }
    if (f.msg_type == VOLC_MSG_AUDIO_ONLY_SERVER) {
        tts_enqueue_audio(s, f.payload, f.payload_len);
        return;
    }
    if (f.msg_type != VOLC_MSG_FULL_SERVER || !f.has_event) return;

    switch (f.event) {
        case VOLC_EVT_CONNECTION_STARTED:
            xEventGroupSetBits(s->eg, BIT_CONN_STARTED);
            break;
        case VOLC_EVT_CONNECTION_FAILED:
            tts_emit_error(s, -ESP_FAIL, (const char*)f.payload, f.payload_len);
            break;
        case VOLC_EVT_SESSION_STARTED:
            xEventGroupSetBits(s->eg, BIT_SESS_STARTED);
            break;
        case VOLC_EVT_SESSION_FINISHED:
            ESP_LOGI(TAG, "session finished");
            s->session_active = false;
            s->pending_finish = true;
            xEventGroupSetBits(s->eg, BIT_SESS_FINISHED);
            // 音频帧先于本事件到达（同一 socket 顺序），此刻 audio_rb 里才是
            // 完整音频。交给搬运任务在 audio_rb 排空后再挂 OnPlaybackDrained，
            // 避免中间缓冲还没搬完就过早触发 on_finished。
            s->pump_finish_pending = true;
            break;
        case VOLC_EVT_SESSION_CANCELED:
            s->session_active = false;
            xEventGroupSetBits(s->eg, BIT_SESS_CANCELED | BIT_DONE);
            break;
        case VOLC_EVT_SESSION_FAILED:
            tts_emit_error(s, -ESP_FAIL, (const char*)f.payload, f.payload_len);
            break;
        case VOLC_EVT_CONNECTION_FINISHED:
            ESP_LOGI(TAG, "connection finished");
            xEventGroupSetBits(s->eg, BIT_CONN_FINISHED);
            break;
        default:  // TTSSentenceStart/End、UsageResponse 等：忽略
            break;
    }
}

static void tts_ws_event(void* arg, esp_event_base_t /*base*/, int32_t event_id,
                         void* event_data) {
    auto* s = static_cast<TtsState*>(arg);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupSetBits(s->eg, BIT_WS_CONNECTED);
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (data->op_code != 0x0 && data->op_code != 0x1 &&
                data->op_code != 0x2) {
                break;
            }
            if (data->payload_len == 0) break;
            if (data->payload_offset == 0) {
                if ((size_t)data->payload_len > s->rx_cap) {
                    uint8_t* grown =
                        (uint8_t*)realloc(s->rx, data->payload_len);
                    if (!grown) {
                        tts_emit_error(s, -ESP_ERR_NO_MEM, "rx alloc", 8);
                        break;
                    }
                    s->rx = grown;
                    s->rx_cap = data->payload_len;
                }
                s->rx_expected = data->payload_len;
            }
            if (!s->rx || (size_t)(data->payload_offset + data->data_len) >
                              s->rx_cap) {
                break;
            }
            memcpy(s->rx + data->payload_offset, data->data_ptr,
                   data->data_len);
            if ((size_t)(data->payload_offset + data->data_len) >=
                s->rx_expected) {
                tts_handle_frame(s, s->rx, s->rx_expected);
            }
            break;
        }
        case WEBSOCKET_EVENT_ERROR:
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            s->conn_started = false;
            if (s->session_active) {
                tts_emit_error(s, -ESP_ERR_INVALID_STATE, "connection lost",
                               15);
            }
            break;
        default:
            break;
    }
}

static esp_err_t tts_send_event(TtsState* s, int event, const char* session_id,
                                const char* json);

static void tts_teardown_connection(TtsState* s) {
    if (!s->ws) return;
    // 协议级干净收尾：连接仍活时先发 FinishConnection，等服务端 ConnectionFinished
    // 回收连接槽。否则服务端累积「设备以为关了、服务端以为还活着」的僵尸连接，
    // 占满 app-key 的 TTS bidirection 并发额度 → 之后新连接一律被秒关（永久失效）。
    if (s->conn_started && esp_websocket_client_is_connected(s->ws)) {
        xEventGroupClearBits(s->eg, BIT_CONN_FINISHED);
        if (tts_send_event(s, VOLC_EVT_FINISH_CONNECTION, nullptr, "{}") ==
            ESP_OK) {
            xEventGroupWaitBits(s->eg, BIT_CONN_FINISHED, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(1500));
        }
    }
    esp_websocket_client_close(s->ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(s->ws);
    s->ws = nullptr;
    s->conn_started = false;
    free(s->headers);
    s->headers = nullptr;
}

// 建立 WSS + StartConnection 握手（已就绪则直接复用）
static esp_err_t tts_ensure_connection(TtsState* s) {
    if (s->ws && s->conn_started && esp_websocket_client_is_connected(s->ws)) {
        return ESP_OK;
    }
    tts_teardown_connection(s);

    char connect_id[37];
    volc_gen_uuid(connect_id);
    if (asprintf(&s->headers,
                 "X-Api-App-Key: " VOLC_APP_KEY "\r\n"
                 "X-Api-Access-Key: " VOLC_ACCESS_KEY "\r\n"
                 "X-Api-Resource-Id: " TTS_RESOURCE_ID "\r\n"
                 "X-Api-Connect-Id: %s\r\n",
                 connect_id) < 0) {
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t cfg = {};
    cfg.uri = TTS_URL;
    cfg.headers = s->headers;
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.network_timeout_ms = TTS_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;  // 连接由本组件按会话边界管理
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    s->ws = esp_websocket_client_init(&cfg);
    if (!s->ws) return ESP_FAIL;
    esp_websocket_register_events(s->ws, WEBSOCKET_EVENT_ANY, tts_ws_event, s);

    xEventGroupClearBits(s->eg, BIT_WS_CONNECTED | BIT_CONN_STARTED);
    esp_err_t err = esp_websocket_client_start(s->ws);
    if (err != ESP_OK) return err;

    EventBits_t bits =
        xEventGroupWaitBits(s->eg, BIT_WS_CONNECTED, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(TTS_CONNECT_TIMEOUT_MS));
    if (!(bits & BIT_WS_CONNECTED)) return ESP_ERR_TIMEOUT;

    size_t frame_len = 0;
    uint8_t* frame =
        volc_build_tts_event(VOLC_EVT_START_CONNECTION, nullptr, "{}",
                             &frame_len);
    if (!frame) return ESP_ERR_NO_MEM;
    int sent = esp_websocket_client_send_bin(s->ws, (const char*)frame,
                                             (int)frame_len,
                                             pdMS_TO_TICKS(TTS_SEND_TIMEOUT_MS));
    free(frame);
    if (sent != (int)frame_len) return ESP_FAIL;

    bits = xEventGroupWaitBits(s->eg, BIT_CONN_STARTED | BIT_FAILED, pdFALSE,
                               pdFALSE, pdMS_TO_TICKS(TTS_CONNECT_TIMEOUT_MS));
    if (!(bits & BIT_CONN_STARTED)) {
        return (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
    s->conn_started = true;
    ESP_LOGI(TAG, "connection established");
    return ESP_OK;
}

// 组装 StartSession / TaskRequest 载荷（模板对齐 tts.ts
// createRequestTemplate；text 为 NULL 时即 StartSession）
static char* tts_build_request_json(int event, const char* text) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return nullptr;
    cJSON* user = cJSON_AddObjectToObject(root, "user");
    if (user) cJSON_AddStringToObject(user, "uid", "metalio_claw6");
    cJSON_AddNumberToObject(root, "event", event);
    cJSON* rp = cJSON_AddObjectToObject(root, "req_params");
    if (rp) {
        cJSON_AddStringToObject(rp, "speaker", TTS_VOICE);
        if (text) cJSON_AddStringToObject(rp, "text", text);
        cJSON* ap = cJSON_AddObjectToObject(rp, "audio_params");
        if (ap) {
            cJSON_AddStringToObject(ap, "format", "pcm");
            cJSON_AddNumberToObject(ap, "sample_rate", TTS_SAMPLE_RATE);
            cJSON_AddBoolToObject(ap, "enable_timestamp", false);
        }
        cJSON_AddStringToObject(rp, "additions",
                                "{\"disable_markdown_filter\":false}");
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t tts_send_event(TtsState* s, int event, const char* session_id,
                                const char* json) {
    size_t frame_len = 0;
    uint8_t* frame = volc_build_tts_event(event, session_id, json, &frame_len);
    if (!frame) return ESP_ERR_NO_MEM;
    int sent = esp_websocket_client_send_bin(s->ws, (const char*)frame,
                                             (int)frame_len,
                                             pdMS_TO_TICKS(TTS_SEND_TIMEOUT_MS));
    free(frame);
    return sent == (int)frame_len ? ESP_OK : ESP_FAIL;
}

static TtsState* tts_get_state(void) {
    if (s_tts) return s_tts;
    auto* s = (TtsState*)calloc(1, sizeof(TtsState));
    if (!s) return nullptr;
    s->eg = xEventGroupCreate();
    s->api_lock = xSemaphoreCreateMutex();
    s->audio_rb = xRingbufferCreateWithCaps(TTS_AUDIO_RB_BYTES,
                                            RINGBUF_TYPE_BYTEBUF,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->eg || !s->api_lock || !s->audio_rb) {
        if (s->eg) vEventGroupDelete(s->eg);
        if (s->api_lock) vSemaphoreDelete(s->api_lock);
        if (s->audio_rb) vRingbufferDeleteWithCaps(s->audio_rb);
        free(s);
        return nullptr;
    }
    // 记录空 audio_rb 的空闲字节，作为 tts_wait_audio_rb_empty 的判空基线
    s->audio_rb_free_empty = xRingbufferGetCurFreeSize(s->audio_rb);
    s->pump_run = true;
    s->pump_alive = true;  // 先于建任务置位，避免 shutdown 在任务首行前误判已退出
    // 优先级对齐播放任务（4）；栈 3KB：只做 ringbuf 搬运 + FeedPlayback
    if (xTaskCreate(tts_audio_pump_task, "tts_audio", 3072, s, 4,
                    &s->pump_task) != pdPASS) {
        vEventGroupDelete(s->eg);
        vSemaphoreDelete(s->api_lock);
        vRingbufferDeleteWithCaps(s->audio_rb);
        free(s);
        return nullptr;
    }
    ESP_LOGI(TAG, "tts audio_rb ready: %u KB (PSRAM)",
             (unsigned)(TTS_AUDIO_RB_BYTES / 1024));
    s_tts = s;
    return s;
}

esp_err_t volc_tts_speak_begin(const volc_tts_callbacks_t* cbs) {
    if (!cbs) return ESP_ERR_INVALID_ARG;
    TtsState* s = tts_get_state();
    if (!s) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s->api_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s->session_active || s->pending_finish) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!mhal::audio_pipeline::EnsurePlayback()) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = tts_ensure_connection(s);
    }

    if (err == ESP_OK) {
        s->cbs = *cbs;
        s->discard_audio = true;  // 让搬运任务丢弃上一场残帧
        s->pump_finish_pending = false;
        tts_wait_audio_rb_empty(s);  // 等中间缓冲排空（含清 carry），从干净起播
        s->audio_started = false;
        s->pending_finish = false;
        s->discard_audio = false;
        volc_gen_uuid(s->session_id);
        xEventGroupClearBits(s->eg, BIT_SESS_STARTED | BIT_SESS_FINISHED |
                                        BIT_SESS_CANCELED | BIT_FAILED |
                                        BIT_DONE);
        char* json = tts_build_request_json(VOLC_EVT_START_SESSION, nullptr);
        if (json) {
            err = tts_send_event(s, VOLC_EVT_START_SESSION, s->session_id,
                                 json);
            cJSON_free(json);
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(
            s->eg, BIT_SESS_STARTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(TTS_CONNECT_TIMEOUT_MS));
        if (bits & BIT_SESS_STARTED) {
            s->session_active = true;
            ESP_LOGI(TAG, "session started");
        } else {
            err = (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
        }
    }

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // 半途失败的连接不复用，下次干净重建
        tts_teardown_connection(s);
        ESP_LOGE(TAG, "speak_begin failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s->api_lock);
    return err;
}

esp_err_t volc_tts_feed_text(const char* text_utf8) {
    TtsState* s = s_tts;
    if (!s || !s->session_active) return ESP_ERR_INVALID_STATE;
    if (!text_utf8 || !text_utf8[0]) return ESP_OK;
    // 纯空白片段跳过（对齐参考实现的 trim 检查）
    const char* p = text_utf8;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return ESP_OK;

    xSemaphoreTake(s->api_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s->session_active) {
        char* json = tts_build_request_json(VOLC_EVT_TASK_REQUEST, text_utf8);
        if (json) {
            err = tts_send_event(s, VOLC_EVT_TASK_REQUEST, s->session_id, json);
            cJSON_free(json);
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s->api_lock);
    return err;
}

esp_err_t volc_tts_speak_end(void) {
    TtsState* s = s_tts;
    if (!s || !s->session_active) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s->api_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s->session_active) {
        err = tts_send_event(s, VOLC_EVT_FINISH_SESSION, s->session_id, "{}");
    }
    xSemaphoreGive(s->api_lock);
    return err;
}

esp_err_t volc_tts_wait_done(uint32_t timeout_ms) {
    TtsState* s = s_tts;
    if (!s) return ESP_ERR_INVALID_STATE;
    if (!s->session_active && !s->pending_finish) return ESP_OK;
    EventBits_t bits = xEventGroupWaitBits(s->eg, BIT_DONE, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_DONE)) return ESP_ERR_TIMEOUT;
    return (bits & BIT_FAILED) ? ESP_FAIL : ESP_OK;
}

void volc_tts_stop(void) {
    TtsState* s = s_tts;
    if (!s) return;
    xSemaphoreTake(s->api_lock, portMAX_DELAY);
    s->discard_audio = true;    // 迟到音频直接丢；搬运任务据此把 audio_rb 残帧丢弃排空
    s->pending_finish = false;  // 短路排空回调，不再触发 on_finished
    s->pump_finish_pending = false;  // 取消待触发的排空
    mhal::audio_pipeline::FlushPlayback();  // 立即静音（清播放队列；audio_rb 由搬运任务丢弃）
    if (s->session_active) {
        s->session_active = false;
        if (tts_send_event(s, VOLC_EVT_CANCEL_SESSION, s->session_id, "{}") ==
            ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(
                s->eg,
                BIT_SESS_CANCELED | BIT_SESS_FINISHED | BIT_FAILED, pdFALSE,
                pdFALSE, pdMS_TO_TICKS(2000));
            if (!(bits & (BIT_SESS_CANCELED | BIT_SESS_FINISHED))) {
                tts_teardown_connection(s);  // 取消失败：连接不再可信
            }
        } else {
            tts_teardown_connection(s);
        }
        ESP_LOGI(TAG, "session stopped (barge-in)");
    }
    xEventGroupSetBits(s->eg, BIT_DONE);
    xSemaphoreGive(s->api_lock);
}

bool volc_tts_is_speaking(void) {
    TtsState* s = s_tts;
    return s && (s->session_active || s->pending_finish);
}

void volc_tts_shutdown(void) {
    TtsState* s = s_tts;
    if (!s) return;
    volc_tts_stop();
    xSemaphoreTake(s->api_lock, portMAX_DELAY);
    tts_teardown_connection(s);
    s_tts = nullptr;
    xSemaphoreGive(s->api_lock);
    // 停搬运任务并等它退出——它持有 s 指针，必须先于 free 结束。stop 已置
    // discard_audio + FlushPlayback，搬运任务不会再长阻塞在 FeedPlayback。
    s->pump_run = false;
    vTaskDelay(pdMS_TO_TICKS(60));  // 确保搬运任务已启动并观察到 pump_run
    for (int i = 0; i < 100 && s->pump_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    vSemaphoreDelete(s->api_lock);
    vEventGroupDelete(s->eg);
    vRingbufferDeleteWithCaps(s->audio_rb);
    free(s->rx);
    free(s);
    ESP_LOGI(TAG, "shutdown");
}
