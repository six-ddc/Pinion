#include "volc_asr.h"

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
#include "freertos/task.h"

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
// 采集→上行任务的有界 PCM 队列：~2s 音频余量（网络抖动缓冲）。
#define ASR_UPLINK_QUEUE_BYTES (ASR_SAMPLE_RATE * 2 * 2000 / 1000)
#define ASR_UPLINK_STACK 6144  // 栈：gzip(volc_build_asr_audio) + TLS 写，对齐采集任务
#define ASR_UPLINK_PRIO 5      // I/O 型，低于采集(8)、高于空闲
// 等上行任务退出的超时：需 > 单次 asr_send 的 TLS 写超时（任务最坏卡在一次
// send 里，超时返回后才看 abort/finish 标志退出）。
#define ASR_UPLINK_EXIT_TIMEOUT_MS 7000
#define ASR_UPLINK_POLL_MS 100  // 队列空时的轮询周期（及时响应 finish/abort）

#define BIT_CONNECTED BIT0
#define BIT_FINAL BIT1
#define BIT_ERROR BIT2
#define BIT_UPLINK_EXIT BIT3  // 上行任务已退出

struct AsrSession {
    esp_websocket_client_handle_t ws;
    volc_asr_callbacks_t cbs;
    EventGroupHandle_t eg;
    char* headers;            // 含密钥，仅传给 WS 客户端，不打日志
    // 上行解耦：feed 只把 PCM 塞入有界队列（非阻塞），专用上行任务取出做
    // 200ms 分段 + gzip + TLS 写。seq/pending/finish_sent 均只由上行任务读写。
    RingbufHandle_t pcm_rb;
    TaskHandle_t uplink_task;
    volatile bool finish_requested;  // stop 请求：搬完队列 + 末段上传后退出
    volatile bool abort_requested;   // teardown 请求：立即退出，不再发送
    size_t dropped_bytes;            // 队列满丢弃的 PCM 字节（诊断）
    int32_t seq;
    volatile bool finish_sent;
    volatile bool failed;
    // 200ms 分段聚合（上行任务私有）
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

// 提取 result.text（对齐 asr.ts extractText：result.text 优先，退回顶层 text），
// 并从 result.utterances 累加已定稿（definite=true）分句的文本字节数写入
// *committed_out——即"不会再改"的前缀长度。无 utterances 数组时置为
// VOLC_ASR_COMMITTED_UNKNOWN（见 volc_asr.h），由 UI 退回默认高亮策略。
static char* asr_extract(const uint8_t* payload, size_t len, size_t* committed_out) {
    *committed_out = VOLC_ASR_COMMITTED_UNKNOWN;
    cJSON* root = cJSON_ParseWithLength((const char*)payload, len);
    if (!root) return nullptr;
    char* out = nullptr;
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* text = result ? cJSON_GetObjectItem(result, "text")
                         : cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        out = strdup(text->valuestring);
    }
    cJSON* utterances = result ? cJSON_GetObjectItem(result, "utterances") : nullptr;
    if (cJSON_IsArray(utterances)) {
        size_t committed = 0;
        cJSON* utt = nullptr;
        cJSON_ArrayForEach(utt, utterances) {
            cJSON* def = cJSON_GetObjectItem(utt, "definite");
            cJSON* utext = cJSON_GetObjectItem(utt, "text");
            if (cJSON_IsTrue(def) && cJSON_IsString(utext) && utext->valuestring) {
                committed += strlen(utext->valuestring);
            }
        }
        *committed_out = committed;
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
            size_t committed = VOLC_ASR_COMMITTED_UNKNOWN;
            char* text = asr_extract(payload, payload_len, &committed);
            if (text && text[0] &&
                (!s->last_text || strcmp(text, s->last_text) != 0)) {
                free(s->last_text);
                s->last_text = text;
                text = nullptr;
                if (!f.is_last && s->cbs.on_delta) {
                    s->cbs.on_delta(s->last_text, committed, s->cbs.ctx);
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

// 把当前 pending 段（gzip 后）上传。last=true 为末段（负序号）。仅上行任务调用。
static void asr_flush_segment(AsrSession* s, bool last) {
    size_t frame_len = 0;
    uint8_t* frame = volc_build_asr_audio(s->seq, last, s->pending, s->pending_len, &frame_len);
    s->pending_len = 0;
    if (!frame) {
        asr_emit_error(s, -ESP_ERR_NO_MEM, "asr frame alloc");
        return;
    }
    esp_err_t err = asr_send(s, frame, frame_len);
    free(frame);
    if (err != ESP_OK) {
        // teardown 期间的写失败是预期噪声，不当业务错误上报。
        if (!s->abort_requested)
            asr_emit_error(s, -ESP_FAIL, "asr uplink send failed");
        return;
    }
    s->seq++;
}

// 上行任务：从有界队列取 PCM，聚成 200ms 段上传。把 gzip+TLS 写从采集任务
// （on_frame → volc_asr_feed）解耦，网络阻塞只在此任务侧积压，绝不回压 I2S RX。
static void asr_uplink_task(void* arg) {
    auto* s = static_cast<AsrSession*>(arg);
    while (!s->abort_requested && !s->failed) {
        size_t got = 0;
        auto* chunk = (uint8_t*)xRingbufferReceiveUpTo(
            s->pcm_rb, &got, pdMS_TO_TICKS(ASR_UPLINK_POLL_MS), ASR_SEGMENT_BYTES - s->pending_len);
        if (chunk) {
            memcpy(s->pending + s->pending_len, chunk, got);
            s->pending_len += got;
            vRingbufferReturnItem(s->pcm_rb, chunk);
            if (s->pending_len == ASR_SEGMENT_BYTES)
                asr_flush_segment(s, false);
            continue;  // 尽快继续搬运
        }
        if (s->finish_requested)
            break;  // 队列已空且收到收尾请求
    }
    // 正常收尾：残余 pending（可为空）作为末段上传。abort/failed 时跳过。
    if (s->finish_requested && !s->abort_requested && !s->failed) {
        s->finish_sent = true;
        asr_flush_segment(s, true);
    }
    xEventGroupSetBits(s->eg, BIT_UPLINK_EXIT);
    vTaskDelete(nullptr);
}

static void asr_destroy_locked_out(AsrSession* s) {
    // 必须在非 WS 任务、非上行任务上下文调用。
    // 先停上行任务再拆连接：任务退出前仍在用 s->ws / pending / eg，不能提前
    // close/destroy/free（UAF）。置 abort 后它当前那次 asr_send 最坏 5s（TLS
    // 写超时）返回、再看 abort 退出，故等待超时取 > 单次 send 超时。正常 stop
    // 路径此时任务已退出（BIT_UPLINK_EXIT 已置），等待立即返回。
    s->abort_requested = true;
    if (s->uplink_task) {
        xEventGroupWaitBits(s->eg, BIT_UPLINK_EXIT, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(ASR_UPLINK_EXIT_TIMEOUT_MS));
    }
    // 会话结论至此已定，teardown 期间的 WS 事件都是噪声（服务端 final 后
    // 主动断连，close 帧写失败会触发 ERROR），先注销回调防止污染上层标志。
    if (s->ws) {
        esp_websocket_unregister_events(s->ws, WEBSOCKET_EVENT_ANY, asr_ws_event);
        esp_websocket_client_close(s->ws, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(s->ws);
    }
    vEventGroupDelete(s->eg);
    if (s->pcm_rb)
        vRingbufferDeleteWithCaps(s->pcm_rb);
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

    char request_id[37];
    volc_gen_uuid(request_id);
    if (!s->eg || asprintf(&s->headers,
                           "X-Api-Resource-Id: " ASR_RESOURCE_ID "\r\n"
                           "X-Api-Request-Id: %s\r\n"
                           "X-Api-Access-Key: " VOLC_ACCESS_KEY "\r\n"
                           "X-Api-App-Key: " VOLC_APP_KEY "\r\n",
                           request_id) < 0) {
        if (s->eg)
            vEventGroupDelete(s->eg);
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

    // 有界 PCM 队列（PSRAM）+ 专用上行任务：把 gzip+TLS 写从采集任务解耦。
    s->pcm_rb = xRingbufferCreateWithCaps(ASR_UPLINK_QUEUE_BYTES, RINGBUF_TYPE_BYTEBUF,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->pcm_rb || xTaskCreate(asr_uplink_task, "asr_uplink", ASR_UPLINK_STACK, s,
                                  ASR_UPLINK_PRIO, &s->uplink_task) != pdPASS) {
        ESP_LOGE(TAG, "asr uplink queue/task create failed");
        asr_destroy_locked_out(s);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "session started");
    s_asr = s;
    return ESP_OK;
}

esp_err_t volc_asr_feed(const int16_t* pcm, size_t samples) {
    AsrSession* s = s_asr;
    if (!s) return ESP_ERR_INVALID_STATE;
    if (!pcm || samples == 0) return ESP_OK;
    if (s->finish_sent || s->finish_requested || s->failed) {
        return ESP_ERR_INVALID_STATE;
    }
    // 非阻塞入队，绝不回压调用方（采集任务，I2S RX 仅 ~90ms 余量，阻塞即溢出）。
    // gzip+TLS 写全在上行任务侧；网络阻塞只在此队列积压。满则丢本帧并计数——
    // 宁可丢新帧也不让采集侧停摆。
    size_t bytes = samples * sizeof(int16_t);
    if (xRingbufferSend(s->pcm_rb, pcm, bytes, 0) != pdTRUE) {
        s->dropped_bytes += bytes;
        ESP_LOGW(TAG, "uplink queue full: dropped %u bytes (session total %u)", (unsigned)bytes,
                 (unsigned)s->dropped_bytes);
    }
    return ESP_OK;
}

esp_err_t volc_asr_stop(uint32_t final_timeout_ms) {
    AsrSession* s = s_asr;
    if (!s) return ESP_ERR_INVALID_STATE;

    // 请求上行任务把队列剩余 PCM 搬完、残余作为末段（负序号）上传后退出。
    // 依赖不变量“先 StopCapture 再本函数”（见 volc_asr.h）：此刻不再有并发
    // feed 往队列塞，任务可独占排空。
    s->finish_requested = true;
    xEventGroupWaitBits(s->eg, BIT_UPLINK_EXIT, pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(ASR_UPLINK_EXIT_TIMEOUT_MS));
    esp_err_t err = s->failed ? ESP_FAIL : ESP_OK;

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
