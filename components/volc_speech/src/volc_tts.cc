#include "volc_tts.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "metalio_hal/audio.h"
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
#define TTS_RB_BYTES (64 * 1024)   // ~2s @16kHz/16bit 抖动缓冲（PSRAM）
#define TTS_PLAYER_CHUNK 4096      // 单次取出上限：128ms，保证打断延迟可控

#define BIT_WS_CONNECTED BIT0
#define BIT_CONN_STARTED BIT1
#define BIT_SESS_STARTED BIT2
#define BIT_SESS_FINISHED BIT3
#define BIT_SESS_CANCELED BIT4
#define BIT_FAILED BIT5
#define BIT_DONE BIT6  // 本次播报彻底结束（播完/打断/失败）

struct TtsState {
    esp_websocket_client_handle_t ws;
    char* headers;  // 含密钥，仅传给 WS 客户端，不打日志
    bool conn_started;
    EventGroupHandle_t eg;
    SemaphoreHandle_t api_lock;
    RingbufHandle_t rb;
    TaskHandle_t player;
    volatile bool player_exit;
    volc_tts_callbacks_t cbs;
    char session_id[37];
    volatile bool session_active;
    volatile bool pending_finish;  // SessionFinished 已到，待播放队列排空
    volatile bool discard_audio;   // 打断后丢弃后续音频
    volatile bool audio_started;
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
    xEventGroupSetBits(s->eg, BIT_FAILED | BIT_DONE);
}

static void tts_enqueue_audio(TtsState* s, const uint8_t* pcm, size_t len) {
    if (len == 0 || s->discard_audio) return;
    if (!s->audio_started) {
        s->audio_started = true;
        if (s->cbs.on_audio_start) s->cbs.on_audio_start(s->cbs.ctx);
    }
    // 队列满则阻塞 WS 任务（TCP 背压让服务端放缓），打断时立即放弃
    size_t off = 0;
    while (off < len && !s->discard_audio) {
        size_t chunk = len - off;
        if (xRingbufferSend(s->rb, pcm + off, chunk, pdMS_TO_TICKS(100)) ==
            pdTRUE) {
            off += chunk;
        } else if (chunk > 1024) {
            // 整包放不下就按队列剩余空间切小重试
            size_t free_bytes = xRingbufferGetCurFreeSize(s->rb);
            if (free_bytes >= 2) {
                size_t part = free_bytes & ~(size_t)1;  // 保持 16bit 对齐
                if (part > chunk) part = chunk;
                if (xRingbufferSend(s->rb, pcm + off, part, 0) == pdTRUE) {
                    off += part;
                }
            }
        }
    }
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
            s->pending_finish = true;  // 播放任务排空后触发 on_finished
            xEventGroupSetBits(s->eg, BIT_SESS_FINISHED);
            break;
        case VOLC_EVT_SESSION_CANCELED:
            s->session_active = false;
            xEventGroupSetBits(s->eg, BIT_SESS_CANCELED | BIT_DONE);
            break;
        case VOLC_EVT_SESSION_FAILED:
            tts_emit_error(s, -ESP_FAIL, (const char*)f.payload, f.payload_len);
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

static void tts_player_task(void* arg) {
    auto* s = static_cast<TtsState*>(arg);
    while (!s->player_exit) {
        size_t got = 0;
        uint8_t* item = (uint8_t*)xRingbufferReceiveUpTo(
            s->rb, &got, pdMS_TO_TICKS(50), TTS_PLAYER_CHUNK);
        if (item) {
            if (!s->discard_audio && got >= 2) {
                mhal::audio::WritePcm((const int16_t*)item, got / 2);
            }
            vRingbufferReturnItem(s->rb, item);
        } else if (s->pending_finish) {
            s->pending_finish = false;
            ESP_LOGI(TAG, "playback drained");
            if (s->cbs.on_finished) s->cbs.on_finished(s->cbs.ctx);
            xEventGroupSetBits(s->eg, BIT_DONE);
        }
    }
    s->player = nullptr;
    vTaskDelete(nullptr);
}

static void tts_teardown_connection(TtsState* s) {
    if (!s->ws) return;
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
    s->rb = xRingbufferCreateWithCaps(TTS_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->eg || !s->api_lock || !s->rb) {
        if (s->eg) vEventGroupDelete(s->eg);
        if (s->api_lock) vSemaphoreDelete(s->api_lock);
        if (s->rb) vRingbufferDeleteWithCaps(s->rb);
        free(s);
        return nullptr;
    }
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
    } else {
        err = tts_ensure_connection(s);
    }

    if (err == ESP_OK && !s->player) {
        s->player_exit = false;
        if (xTaskCreate(tts_player_task, "volc_tts_play", 4096, s, 5,
                        &s->player) != pdPASS) {
            s->player = nullptr;
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        s->cbs = *cbs;
        s->discard_audio = false;
        s->audio_started = false;
        s->pending_finish = false;
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
            mhal::audio::EnableOutput(true);
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
    s->discard_audio = true;  // 播放任务与入队路径立即丢弃
    s->pending_finish = false;
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
    if (s->player) {
        s->player_exit = true;
        for (int i = 0; i < 20 && s->player; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    tts_teardown_connection(s);
    s_tts = nullptr;
    xSemaphoreGive(s->api_lock);
    vSemaphoreDelete(s->api_lock);
    vEventGroupDelete(s->eg);
    vRingbufferDeleteWithCaps(s->rb);
    free(s->rx);
    free(s);
    ESP_LOGI(TAG, "shutdown");
}
