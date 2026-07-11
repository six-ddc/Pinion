#include "volc_asr.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "volc_proto.h"

#if __has_include("volc_keys.h")
#include "volc_keys.h"
#else
#error "缺少 volc_keys.h：复制 include/volc_keys.h.example 为 include/volc_keys.h 并填入密钥"
#endif

static const char* TAG = "volc_asr";

// 端点/资源与已验证的参考部署一致（ai-chat-esp32 service/.env）
#define ASR_URL "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
#define ASR_RESOURCE_ID "volc.seedasr.sauc.duration"
#define ASR_SAMPLE_RATE 16000  // mhal::audio 板载 codec 固定 16kHz
#define ASR_SEGMENT_MS 200
#define ASR_SEGMENT_BYTES (ASR_SAMPLE_RATE * 2 * ASR_SEGMENT_MS / 1000)
#define ASR_CONNECT_TIMEOUT_MS 10000
#define ASR_SEND_TIMEOUT_MS 5000

#define BIT_CONNECTED BIT0
#define BIT_FINAL BIT1
#define BIT_ERROR BIT2

struct AsrSession {
    esp_websocket_client_handle_t ws;
    volc_asr_callbacks_t cbs;
    EventGroupHandle_t eg;
    SemaphoreHandle_t lock;   // 保护发送路径与 pending 缓冲
    char* headers;            // 含密钥，仅传给 WS 客户端，不打日志
    int32_t seq;
    bool finish_sent;
    bool failed;
    // 200ms 分段聚合
    uint8_t pending[ASR_SEGMENT_BYTES];
    size_t pending_len;
    // WS 消息重组（esp_websocket_client 按 buffer_size 分片投递）
    uint8_t* rx;
    size_t rx_cap;
    size_t rx_expected;
    // 最新识别文本（服务端全量下发）
    char* last_text;
};

static AsrSession* s_asr = nullptr;

static void asr_emit_error(AsrSession* s, int code, const char* msg) {
    if (s->failed) return;
    s->failed = true;
    ESP_LOGE(TAG, "error %d: %s", code, msg ? msg : "");
    if (s->cbs.on_error) s->cbs.on_error(code, msg ? msg : "", s->cbs.ctx);
    xEventGroupSetBits(s->eg, BIT_ERROR);
}

// 提取 result.text（对齐 asr.ts extractText：result.text 优先，退回顶层 text）
static char* asr_extract_text(const uint8_t* payload, size_t len) {
    cJSON* root = cJSON_ParseWithLength((const char*)payload, len);
    if (!root) return nullptr;
    char* out = nullptr;
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* text = result ? cJSON_GetObjectItem(result, "text")
                         : cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        out = strdup(text->valuestring);
    }
    cJSON_Delete(root);
    return out;
}

static void asr_handle_frame(AsrSession* s, const uint8_t* data, size_t len) {
    volc_frame_t f;
    if (!volc_frame_parse(data, len, &f)) {
        ESP_LOGW(TAG, "unparsable frame (%u bytes)", (unsigned)len);
        return;
    }

    uint8_t* inflated = nullptr;
    const uint8_t* payload = f.payload;
    size_t payload_len = f.payload_len;
    if (payload_len > 0 && f.compression == VOLC_COMP_GZIP) {
        inflated = volc_gunzip(payload, payload_len, &payload_len);
        if (!inflated) {
            ESP_LOGW(TAG, "gunzip failed, drop frame");
            return;
        }
        payload = inflated;
    }

    if (f.msg_type == VOLC_MSG_ERROR) {
        char msg[160] = {0};
        if (payload_len) {
            size_t n = payload_len < sizeof(msg) - 1 ? payload_len : sizeof(msg) - 1;
            memcpy(msg, payload, n);
        }
        asr_emit_error(s, (int)f.error_code, msg);
    } else if (f.msg_type == VOLC_MSG_FULL_SERVER) {
        if (payload_len > 0 && f.serialization == VOLC_SER_JSON) {
            char* text = asr_extract_text(payload, payload_len);
            if (text && text[0] &&
                (!s->last_text || strcmp(text, s->last_text) != 0)) {
                free(s->last_text);
                s->last_text = text;
                text = nullptr;
                if (!f.is_last && s->cbs.on_delta) {
                    s->cbs.on_delta(s->last_text, s->cbs.ctx);
                }
            }
            free(text);
        }
        if (f.is_last) {
            ESP_LOGI(TAG, "final (seq=%d)", (int)f.sequence);
            if (s->cbs.on_final) {
                s->cbs.on_final(s->last_text ? s->last_text : "", s->cbs.ctx);
            }
            xEventGroupSetBits(s->eg, BIT_FINAL);
        }
    }
    free(inflated);
}

static void asr_ws_event(void* arg, esp_event_base_t /*base*/, int32_t event_id,
                         void* event_data) {
    auto* s = static_cast<AsrSession*>(arg);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupSetBits(s->eg, BIT_CONNECTED);
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (data->op_code != 0x0 && data->op_code != 0x1 &&
                data->op_code != 0x2) {
                break;  // ping/pong/close 控制帧
            }
            if (data->payload_len == 0) break;
            if (data->payload_offset == 0) {
                if ((size_t)data->payload_len > s->rx_cap) {
                    uint8_t* grown =
                        (uint8_t*)realloc(s->rx, data->payload_len);
                    if (!grown) {
                        asr_emit_error(s, -ESP_ERR_NO_MEM, "rx alloc");
                        break;
                    }
                    s->rx = grown;
                    s->rx_cap = data->payload_len;
                }
                s->rx_expected = data->payload_len;
            }
            if (!s->rx || (size_t)(data->payload_offset + data->data_len) >
                              s->rx_cap) {
                break;  // 无起始分片的续片，丢弃
            }
            memcpy(s->rx + data->payload_offset, data->data_ptr,
                   data->data_len);
            if ((size_t)(data->payload_offset + data->data_len) >=
                s->rx_expected) {
                asr_handle_frame(s, s->rx, s->rx_expected);
            }
            break;
        }
        case WEBSOCKET_EVENT_ERROR:
            asr_emit_error(s, -ESP_FAIL, "websocket error");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            // 对齐参考实现：finish 已发出后的关闭视为正常收尾（final 兜底）
            if ((xEventGroupGetBits(s->eg) & (BIT_FINAL | BIT_ERROR)) == 0) {
                if (s->finish_sent) {
                    if (s->cbs.on_final) {
                        s->cbs.on_final(s->last_text ? s->last_text : "",
                                        s->cbs.ctx);
                    }
                    xEventGroupSetBits(s->eg, BIT_FINAL);
                } else {
                    asr_emit_error(s, -ESP_ERR_INVALID_STATE,
                                   "closed before finish");
                }
            }
            break;
        default:
            break;
    }
}

static esp_err_t asr_send(AsrSession* s, const uint8_t* frame, size_t len) {
    int sent = esp_websocket_client_send_bin(
        s->ws, (const char*)frame, (int)len, pdMS_TO_TICKS(ASR_SEND_TIMEOUT_MS));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

static void asr_destroy_locked_out(AsrSession* s) {
    // 必须在非 WS 任务上下文调用
    esp_websocket_client_close(s->ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(s->ws);
    vEventGroupDelete(s->eg);
    vSemaphoreDelete(s->lock);
    free(s->headers);
    free(s->rx);
    free(s->last_text);
    free(s);
}

esp_err_t volc_asr_start(const volc_asr_callbacks_t* cbs) {
    if (s_asr) return ESP_ERR_INVALID_STATE;
    if (!cbs) return ESP_ERR_INVALID_ARG;

    auto* s = (AsrSession*)calloc(1, sizeof(AsrSession));
    if (!s) return ESP_ERR_NO_MEM;
    s->cbs = *cbs;
    s->seq = 1;
    s->eg = xEventGroupCreate();
    s->lock = xSemaphoreCreateMutex();

    char request_id[37];
    volc_gen_uuid(request_id);
    if (!s->eg || !s->lock ||
        asprintf(&s->headers,
                 "X-Api-Resource-Id: " ASR_RESOURCE_ID "\r\n"
                 "X-Api-Request-Id: %s\r\n"
                 "X-Api-Access-Key: " VOLC_ACCESS_KEY "\r\n"
                 "X-Api-App-Key: " VOLC_APP_KEY "\r\n",
                 request_id) < 0) {
        if (s->eg) vEventGroupDelete(s->eg);
        if (s->lock) vSemaphoreDelete(s->lock);
        free(s);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t cfg = {};
    cfg.uri = ASR_URL;
    cfg.headers = s->headers;
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.network_timeout_ms = ASR_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;  // 会话态在服务端，掉线只能报错重来
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    s->ws = esp_websocket_client_init(&cfg);
    if (!s->ws) {
        vEventGroupDelete(s->eg);
        vSemaphoreDelete(s->lock);
        free(s->headers);
        free(s);
        return ESP_FAIL;
    }
    esp_websocket_register_events(s->ws, WEBSOCKET_EVENT_ANY, asr_ws_event, s);

    esp_err_t err = esp_websocket_client_start(s->ws);
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(
            s->eg, BIT_CONNECTED | BIT_ERROR, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(ASR_CONNECT_TIMEOUT_MS));
        if (!(bits & BIT_CONNECTED)) {
            err = (bits & BIT_ERROR) ? ESP_FAIL : ESP_ERR_TIMEOUT;
        }
    }

    if (err == ESP_OK) {
        // full client request：参数对齐 asr.ts buildFullClientRequest
        static const char* kRequestJson =
            "{\"user\":{\"uid\":\"metalio_claw6\"},"
            "\"audio\":{\"format\":\"pcm\",\"codec\":\"raw\",\"rate\":16000,"
            "\"bits\":16,\"channel\":1},"
            "\"request\":{\"model_name\":\"bigmodel\",\"enable_itn\":true,"
            "\"enable_punc\":true,\"enable_ddc\":true,"
            "\"show_utterances\":true,\"enable_nonstream\":false}}";
        size_t frame_len = 0;
        uint8_t* frame =
            volc_build_asr_full_request(s->seq, kRequestJson, &frame_len);
        if (frame) {
            err = asr_send(s, frame, frame_len);
            free(frame);
            if (err == ESP_OK) s->seq++;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        asr_destroy_locked_out(s);
        return err;
    }
    ESP_LOGI(TAG, "session started");
    s_asr = s;
    return ESP_OK;
}

esp_err_t volc_asr_feed(const int16_t* pcm, size_t samples) {
    AsrSession* s = s_asr;
    if (!s) return ESP_ERR_INVALID_STATE;
    if (!pcm || samples == 0) return ESP_OK;

    xSemaphoreTake(s->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s->finish_sent || s->failed) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        const uint8_t* src = (const uint8_t*)pcm;
        size_t remaining = samples * sizeof(int16_t);
        while (remaining > 0 && err == ESP_OK) {
            size_t space = ASR_SEGMENT_BYTES - s->pending_len;
            size_t take = remaining < space ? remaining : space;
            memcpy(s->pending + s->pending_len, src, take);
            s->pending_len += take;
            src += take;
            remaining -= take;

            if (s->pending_len == ASR_SEGMENT_BYTES) {
                size_t frame_len = 0;
                uint8_t* frame = volc_build_asr_audio(
                    s->seq, false, s->pending, s->pending_len, &frame_len);
                if (!frame) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    err = asr_send(s, frame, frame_len);
                    free(frame);
                    if (err == ESP_OK) s->seq++;
                }
                s->pending_len = 0;
            }
        }
    }
    xSemaphoreGive(s->lock);
    return err;
}

esp_err_t volc_asr_stop(uint32_t final_timeout_ms) {
    AsrSession* s = s_asr;
    if (!s) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s->finish_sent && !s->failed) {
        s->finish_sent = true;
        // 残余（可为空）作为末段：负序号，对齐参考实现 finish()
        size_t frame_len = 0;
        uint8_t* frame = volc_build_asr_audio(s->seq, true, s->pending,
                                              s->pending_len, &frame_len);
        s->pending_len = 0;
        if (frame) {
            err = asr_send(s, frame, frame_len);
            free(frame);
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s->lock);

    if (err == ESP_OK && final_timeout_ms > 0) {
        EventBits_t bits =
            xEventGroupWaitBits(s->eg, BIT_FINAL | BIT_ERROR, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(final_timeout_ms));
        if (bits & BIT_FINAL) {
            err = ESP_OK;
        } else if (bits & BIT_ERROR) {
            err = ESP_FAIL;
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }

    s_asr = nullptr;
    asr_destroy_locked_out(s);
    ESP_LOGI(TAG, "session closed (%s)", esp_err_to_name(err));
    return err;
}

void volc_asr_abort(void) {
    AsrSession* s = s_asr;
    if (!s) return;
    s_asr = nullptr;
    asr_destroy_locked_out(s);
    ESP_LOGI(TAG, "session aborted");
}

bool volc_asr_is_active(void) { return s_asr != nullptr; }
