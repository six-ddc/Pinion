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
#define TTS_FEED_TIMEOUT_MS 10000  // 抖动队列满时对 WS 任务的最大背压时长
#define TTS_RX_MAX_BYTES (1024 * 1024)  // 单条 WS 消息重组上限（防御异常长度）

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
    SemaphoreHandle_t cbs_lock;  // cbs 由 speak_begin 改写、WS/播放任务读
    volc_tts_callbacks_t cbs;
    char session_id[37];
    volatile bool session_active;
    volatile bool pending_finish;  // SessionFinished 已到，待播放队列排空
    volatile bool discard_audio;   // 打断后丢弃迟到的下行音频
    volatile bool flush_pending;   // 出错路径记账：待收尾路径清空播放队列
    volatile bool audio_started;
    uint32_t playback_gen;   // 本会话开始时捕获的播放代次（打断残音竞态收口）
    size_t dropped_samples;  // 本会话因 Feed 超时丢弃的样本数（诊断）
    // WS 消息重组
    uint8_t* rx;
    size_t rx_cap;
    size_t rx_expected;
};

static TtsState* s_tts = nullptr;

// cbs 是多个函数指针的结构体，跨任务整体拷贝非原子：读写都过 cbs_lock，
// 防止遗留连接事件撞上新会话改写 cbs 时读到半新半旧的指针。
static volc_tts_callbacks_t tts_cbs_snapshot(TtsState* s) {
    xSemaphoreTake(s->cbs_lock, portMAX_DELAY);
    volc_tts_callbacks_t c = s->cbs;
    xSemaphoreGive(s->cbs_lock);
    return c;
}

static void tts_emit_error(TtsState* s, int code, const char* msg,
                           size_t msg_len) {
    char buf[160] = {0};
    if (msg && msg_len) {
        size_t n = msg_len < sizeof(buf) - 1 ? msg_len : sizeof(buf) - 1;
        memcpy(buf, msg, n);
    }
    ESP_LOGE(TAG, "error %d: %s", code, buf);
    s->session_active = false;
    s->pending_finish = false;
    // 出错即打断，但本函数跑在 WS 事件任务上：FlushPlayback 内含最多数百 ms
    // 忙等（等在途喂入退出 + 等在写残帧落地），在此调用会冻结 WS 收发（含
    // ping/pong）。故只置标志——discard_audio 令迟到的下行帧不再入队，flush_pending
    // 记账缓冲余音待清；真正 FlushPlayback 挪到持 api_lock 的收尾路径执行：
    // barge-in 的 volc_tts_stop()，或下一次 volc_tts_speak_begin() 的清理。
    // 依赖：出错后必有一条收尾路径被触达——UI 打断/STOP/新会话会调 stop，
    // 否则下一轮播报的 speak_begin 会先清队列再开播。
    s->discard_audio = true;
    s->flush_pending = true;
    volc_tts_callbacks_t c = tts_cbs_snapshot(s);
    if (c.on_error) c.on_error(code, buf, c.ctx);
    xEventGroupSetBits(s->eg, BIT_FAILED | BIT_DONE);
}

static void tts_enqueue_audio(TtsState* s, const uint8_t* pcm, size_t len) {
    if (len < 2 || s->discard_audio) return;
    if (!s->audio_started) {
        s->audio_started = true;
        volc_tts_callbacks_t c = tts_cbs_snapshot(s);
        if (c.on_audio_start) c.on_audio_start(c.ctx);
    }
    // 队列满则阻塞 WS 任务（TCP 背压让服务端放缓）；打断时管线立即丢弃。
    // 带本会话代次：喂入与入队之间若发生打断（FlushPlayback），管线按已消费丢弃。
    size_t want = len / 2;
    size_t fed = mhal::audio_pipeline::FeedPlayback((const int16_t*)pcm, want, TTS_FEED_TIMEOUT_MS,
                                                    s->playback_gen);
    if (fed < want && !s->discard_audio) {
        // 背压 10s 仍塞不进：正常只在播放端停摆（I2S 掉时钟）时走到。
        // 打断（discard_audio）导致的部分喂入是预期路径，不计。
        s->dropped_samples += want - fed;
        ESP_LOGW(TAG, "feed timeout: dropped %u samples (session total %u)",
                 (unsigned)(want - fed), (unsigned)s->dropped_samples);
    }
}

// SessionFinished 后由播放管线在队列排空时触发（打断路径上被
// pending_finish=false 短路）
static void tts_on_drained(TtsState* s) {
    if (!s->pending_finish) return;
    s->pending_finish = false;
    ESP_LOGI(TAG, "playback drained");
    volc_tts_callbacks_t c = tts_cbs_snapshot(s);
    if (c.on_finished) c.on_finished(c.ctx);
    xEventGroupSetBits(s->eg, BIT_DONE);
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
            // 服务端音频帧先于本事件到达（同一 socket 顺序保证），此刻
            // 队列里已是完整音频；排空（或已空）即整场播完。
            mhal::audio_pipeline::OnPlaybackDrained(
                [s]() { tts_on_drained(s); });
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
                if ((size_t)data->payload_len > TTS_RX_MAX_BYTES) {
                    ESP_LOGE(TAG, "rx message too large: %d bytes",
                             data->payload_len);
                    s->rx_expected = 0;  // 该消息的后续分片一并丢弃
                    tts_emit_error(s, -ESP_ERR_NO_MEM, "rx too large", 12);
                    break;
                }
                if ((size_t)data->payload_len > s->rx_cap) {
                    uint8_t* grown =
                        (uint8_t*)realloc(s->rx, data->payload_len);
                    if (!grown) {
                        s->rx_expected = 0;
                        tts_emit_error(s, -ESP_ERR_NO_MEM, "rx alloc", 8);
                        break;
                    }
                    s->rx = grown;
                    s->rx_cap = data->payload_len;
                }
                s->rx_expected = data->payload_len;
            }
            if (!s->rx || s->rx_expected == 0 ||
                (size_t)(data->payload_offset + data->data_len) > s->rx_cap) {
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
    s->cbs_lock = xSemaphoreCreateMutex();
    if (!s->eg || !s->api_lock || !s->cbs_lock) {
        if (s->eg) vEventGroupDelete(s->eg);
        if (s->api_lock) vSemaphoreDelete(s->api_lock);
        if (s->cbs_lock) vSemaphoreDelete(s->cbs_lock);
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
    } else if (!mhal::audio_pipeline::EnsurePlayback()) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = tts_ensure_connection(s);
    }

    if (err == ESP_OK) {
        // 承接上一场出错路径延迟下来的打断：清掉可能残留的缓冲余音
        // （tts_emit_error 只置 flush_pending，不在 WS 上下文 flush）。此处持
        // api_lock，是收尾路径之一。
        if (s->flush_pending) {
            mhal::audio_pipeline::FlushPlayback();
            s->flush_pending = false;
        }
        xSemaphoreTake(s->cbs_lock, portMAX_DELAY);
        s->cbs = *cbs;
        xSemaphoreGive(s->cbs_lock);
        s->discard_audio = false;
        s->audio_started = false;
        s->pending_finish = false;
        s->dropped_samples = 0;
        // 捕获当前播放代次：本会话喂入的每帧都带上它（见 tts_enqueue_audio）。
        // 必须在上面可能的 FlushPlayback（会 ++gen）之后取。
        s->playback_gen = mhal::audio_pipeline::PlaybackGen();
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
    s->discard_audio = true;    // 迟到的下行音频直接丢
    s->pending_finish = false;  // 短路排空回调，不再触发 on_finished
    mhal::audio_pipeline::FlushPlayback();  // 收尾路径：连同出错延迟的清空一起做
    s->flush_pending = false;
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
    // 注销可能挂着的排空回调（其捕获了 s）：返回即保证无在途回调，
    // 之后 free(s) 安全。
    mhal::audio_pipeline::OnPlaybackDrained(nullptr);
    xSemaphoreTake(s->api_lock, portMAX_DELAY);
    tts_teardown_connection(s);
    s_tts = nullptr;
    xSemaphoreGive(s->api_lock);
    vSemaphoreDelete(s->api_lock);
    vSemaphoreDelete(s->cbs_lock);
    vEventGroupDelete(s->eg);
    free(s->rx);
    free(s);
    ESP_LOGI(TAG, "shutdown");
}
