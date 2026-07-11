/* pi-c — MetalioClaw5 pi_screen App: agent thread + real API transport + calc
 * tool + on_event -> pi_ui_evt_t bridge (blueprint §2/§3 WP-B, §4 event map).
 *
 * Production talks to the real API: transport = pi_esp32_transport() (TLS via
 * esp_crt_bundle, already wired by the port) and model/provider config comes
 * from models.json — a verbatim copy of six-ddc/pi-c/models.json lives as a C
 * string literal in pi_models_data.h (PI_MODELS_JSON_TEXT), so no build-system
 * embed step or external symbol is needed. embedded_models_json_read() below
 * is a one-function pi_fs_t shim that hands that in-memory string to the
 * real PI_FEATURE_MODELS_JSON loader (pi_models_load, full catalog, keeps
 * model.compat/thinking_level_map alive — unlike pi_models_json_load_first,
 * which drops compat) exactly as if it had come off a filesystem.
 * Keeping the full catalog matters here: models.json's deepseek entries carry
 * a compat override (requiresReasoningContentOnAssistantMessages/
 * thinkingFormat=deepseek) that PI_FEATURE_COMPAT needs to talk to DeepSeek
 * correctly, and that override only survives via the catalog API.
 *
 * The two-turn mock script is kept in the tree as an opt-in offline-debug
 * channel (PI_AGENT_TASK_USE_MOCK, default 0) but contributes zero code to
 * the production path when left off — see the #if blocks below.
 *
 * Runs pi_agent_prompt() to completion on a dedicated FreeRTOS task (blueprint
 * §1 3b). on_event() fires synchronously on that task's stack while the loop
 * is running (pi_agent.h contract) — every pi_agent_event_t/pi_ai_event_t
 * pointer it exposes (message/ai->delta/tool_args/tool_result->output/
 * error_message) is borrowed and only valid for the callback's duration, so
 * everything this file keeps is strdup'ed (plain malloc, per pi_ui_bridge.h)
 * into a pi_ui_evt_t before it is pushed to pi_ui_queue() (blueprint R3).
 * SPDX-License-Identifier: MIT */
#include "pi_ui_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pi/pi.h"
#include "pi_esp32.h"
#include "pi_models_data.h"
#include "volc_tts.h"

/* Offline-debug channel: flip to 1 locally to replay the two-turn Anthropic
 * mock script instead of hitting the real API (no network required). Off by
 * default — production always uses models.json + pi_esp32_transport(). */
#define PI_AGENT_TASK_USE_MOCK 0

#if PI_AGENT_TASK_USE_MOCK
#include "pi_mock_paced.h"
#include "pi_scr_mock.h"
static pi_mock_t g_mock;
static const pi_mock_response_t g_responses[2] = {{200, PI_MOCK_TURN1, 0, 0},
                                                   {200, PI_MOCK_TURN2, 0, 0}};
static const pi_model_t MOCK_MODEL = {
    .id = "claude-sonnet-5",
    .api = PI_API_ANTHROPIC_MESSAGES,
    .base_url = "https://mock.local",
    .max_tokens = 1024,
    .reasoning = true,
};
#endif

static const char *TAG = "pi_agent_task";

/* ---------- env + real model catalog (models.json, embedded) ---------- */
static pi_env_t g_env;
static pi_agent_t *g_agent;
static bool g_env_ready = false;

#if !PI_AGENT_TASK_USE_MOCK
/* models.json's content lives in pi_models_data.h as a C string literal
 * (PI_MODELS_JSON_TEXT — verbatim copy of six-ddc/pi-c/models.json, no build-
 * system embed step / external symbol needed). embedded_models_json_read()
 * is a one-function pi_fs_t shim that hands that in-memory string to pi-c's
 * real PI_FEATURE_MODELS_JSON loader (pi_models_load) exactly as if it had
 * come off a filesystem. */
#define PI_MODELS_JSON_LEN (sizeof(PI_MODELS_JSON_TEXT) - 1) /* -1: exclude the literal's NUL */

static pi_models_catalog_t *g_catalog = NULL;
static const pi_model_t *g_model = NULL; /* borrowed from g_catalog; catalog never freed */

static char *embedded_models_json_read(void *ctx, const char *path, size_t *out_len,
                                       const pi_alloc_t *alloc) {
    (void)ctx;
    (void)path;
    char *buf = (char *)pi_malloc(alloc, PI_MODELS_JSON_LEN + 1); /* pi_models_load frees this */
    if (!buf) return NULL;
    memcpy(buf, PI_MODELS_JSON_TEXT, PI_MODELS_JSON_LEN);
    buf[PI_MODELS_JSON_LEN] = '\0';
    if (out_len) *out_len = PI_MODELS_JSON_LEN;
    return buf;
}

static const pi_fs_t EMBEDDED_MODELS_FS = {.read_file = embedded_models_json_read};

/* Loads the whole catalog (not pi_models_json_load_first: that convenience
 * snapshot drops model.compat, which DeepSeek needs). Picks catalog entry 0
 * (models.json's single "deepseek" provider, first model = deepseek-v4-pro). */
static bool load_model_catalog(void) {
    int rc = pi_models_load(&g_env, "models.json" /* ignored by the embedded fs shim */,
                            &g_catalog);
    if (rc != PI_OK || !g_catalog) {
        ESP_LOGE(TAG, "pi_models_load failed rc=%d", rc);
        return false;
    }
    g_model = pi_models_at(g_catalog, 0);
    if (!g_model) {
        ESP_LOGE(TAG, "models.json catalog has no models");
        return false;
    }
    ESP_LOGI(TAG, "model catalog loaded: provider=%s id=%s baseUrl=%s",
             g_model->provider_id ? g_model->provider_id : "?",
             g_model->id ? g_model->id : "?", g_model->base_url ? g_model->base_url : "?");
    return true;
}
#endif /* !PI_AGENT_TASK_USE_MOCK */

/* ---------- worker task + prompt hand-off ---------- */
#define PI_AGENT_TASK_STACK 8192 /* bytes, internal DIRAM (blueprint §1 3b / R4) */
#define PI_AGENT_TASK_PRIO 4
#define PI_AGENT_TASK_CORE 0

static TaskHandle_t g_worker_task;
static SemaphoreHandle_t g_prompt_sem;
static char g_pending_prompt[512];
static volatile bool g_running = false; /* true while pi_agent_prompt() is on the stack */

/* ---------- TTS（火山 volc_tts，text_delta 流式喂入） ----------
 * 会话状态（g_tts_session_open/g_tts_failed_this_run）只在 agent worker 任务
 * 上下文读写（on_event 同步跑在 worker 栈上）；g_tts_enabled 跨线程只读写
 * bool，无需锁。mock 走带下 speak_begin 无网络会失败一次并静默降级——文本
 * 事件流不受影响，mock 流程不破坏。 */
static volatile bool g_tts_enabled = true; /* 真值由 pi_screen 从 NVS 灌入 */
static bool g_tts_session_open = false;
static bool g_tts_failed_this_run = false;

static void tts_on_error(int code, const char *msg, void *ctx) {
    (void)ctx;
    ESP_LOGW(TAG, "tts error %d: %s", code, msg);
}

static const volc_tts_callbacks_t TTS_CBS = {.on_audio_start = NULL,
                                             .on_finished = NULL,
                                             .on_error = tts_on_error,
                                             .ctx = NULL};

/* volc_tts_stop() 打断在播会话时最坏阻塞 ~2s（等服务端 SessionCanceled），
 * 而它的调用点（STOP 按钮/状态栏开关/新会话）都在 LVGL 线程——丢进一次性
 * 小任务执行，扬声器静音本身在 stop 内部第一步就发生，感知不到延迟。 */
static void tts_stop_worker(void *arg) {
    (void)arg;
    volc_tts_stop();
    vTaskDelete(NULL);
}

static void tts_stop_async(void) {
    if (xTaskCreate(tts_stop_worker, "tts_stop", 4096, NULL, 4, NULL) != pdPASS) {
        volc_tts_stop(); /* 退化为同步（内存紧张时）*/
    }
}

/* worker 任务上下文：确保会话开启并追加一段文本 */
static void tts_feed_delta(const char *delta) {
    if (!g_tts_enabled || g_tts_failed_this_run || !delta || !delta[0]) return;
    if (!g_tts_session_open) {
        esp_err_t err = volc_tts_speak_begin(&TTS_CBS);
        if (err == ESP_ERR_INVALID_STATE) { /* 上一场还在排空：打断后重试一次 */
            volc_tts_stop();
            err = volc_tts_speak_begin(&TTS_CBS);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "tts speak_begin failed (%d), muted for this run", (int)err);
            g_tts_failed_this_run = true;
            return;
        }
        g_tts_session_open = true;
    }
    volc_tts_feed_text(delta);
}

/* ---------- calc tool (real execution: mul(37,89)=3293, host_chat/main.c parity) ---------- */
/* file-scope static initializer needs a compile-time constant: a `static const
 * char *` variable is NOT one in C, so this must be a macro, not a string
 * pointed to by a static (that produced "initializer element is not constant"
 * in the TOOLS[] initializer below). */
#define CALC_SCHEMA                                                                              \
    "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"number\"},"                          \
    "\"b\":{\"type\":\"number\"},\"op\":{\"type\":\"string\","                                   \
    "\"enum\":[\"add\",\"sub\",\"mul\",\"div\"]}},"                                               \
    "\"required\":[\"a\",\"b\",\"op\"]}"

static int calc_exec(const pi_alloc_t *alloc, const char *id, const cJSON *args,
                     volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                     void *user, pi_tool_result_t *out) {
    (void)id;
    (void)abort_flag;
    (void)on_update;
    (void)update_user;
    (void)user;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(args, "a");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(args, "b");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(args, "op");
    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b) || !cJSON_IsString(op)) {
        out->output = pi_strdup(alloc, "invalid arguments");
        out->is_error = true;
        return PI_OK;
    }
    double x = a->valuedouble, y = b->valuedouble, r = 0;
    const char *o = op->valuestring;
    if (strcmp(o, "add") == 0)
        r = x + y;
    else if (strcmp(o, "sub") == 0)
        r = x - y;
    else if (strcmp(o, "mul") == 0)
        r = x * y;
    else if (strcmp(o, "div") == 0)
        r = y != 0 ? x / y : 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", r);
    out->output = pi_strdup(alloc, buf);
    return PI_OK;
}

static const pi_agent_tool_t TOOLS[] = {
    {
        .def = {.name = "calc",
                .description = "Basic arithmetic on two numbers",
                .parameters_schema_json = CALC_SCHEMA},
        .execute = calc_exec,
    },
};

/* ---------- pi event -> pi_ui_evt_t (blueprint §4 mapping table) ---------- */

static void enqueue(pi_ui_kind_t kind, char *s1, char *s2, int i1, int i2) {
    pi_ui_evt_t evt;
    evt.kind = kind;
    evt.s1 = s1;
    evt.s2 = s2;
    evt.i1 = i1;
    evt.i2 = i2;
    QueueHandle_t q = pi_ui_queue();
    if (xQueueSend(q, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pi_ui_queue full, dropping evt kind=%d", (int)kind);
        free(s1);
        free(s2);
    }
}

static uint64_t now_ms(void) {
    const pi_sys_t *sys = pi_esp32_sys();
    return (sys && sys->now_ms) ? sys->now_ms(sys->ctx) : 0;
}

/* per-run cursors (single in-flight prompt at a time, reset at AGENT_START) */
static bool s_thinking_open = false;
static uint64_t s_tool_start_ms = 0;
static int s_text_delta_seq = 0;
/* latest assistant message's real usage (pi_usage_t), refreshed at every
 * MESSAGE_END so by AGENT_END it holds this run's final-turn numbers — the
 * AGENT_END event itself carries no message pointer (emit_simple zeroes it),
 * so this is the only way to get real input/output token counts onto DONE. */
static uint32_t s_last_usage_input = 0;
static uint32_t s_last_usage_output = 0;

static void on_event(const pi_agent_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
    case PI_AG_EV_AGENT_START:
        s_thinking_open = false;
        s_text_delta_seq = 0;
        s_last_usage_input = 0;
        s_last_usage_output = 0;
        g_tts_failed_this_run = false;
        enqueue(UI_AGENT_START, NULL, NULL, 0, 0);
        break;

    case PI_AG_EV_MESSAGE_UPDATE: {
        const pi_ai_event_t *ai = ev->ai;
        if (!ai) break;
        switch (ai->kind) {
        case PI_AI_EV_THINKING_DELTA:
            /* first delta opens the thinking indicator (blueprint §4 row 2); no
             * text is queued — FLOW renders a timer, not the thinking content */
            if (!s_thinking_open) {
                s_thinking_open = true;
                enqueue(UI_THINKING_START, NULL, NULL, 0, 0);
            }
            break;
        case PI_AI_EV_THINKING_END:
            if (s_thinking_open) {
                s_thinking_open = false;
                enqueue(UI_THINKING_END, NULL, NULL, 0, 0);
            }
            break;
        case PI_AI_EV_TOOLCALL_START: {
            const pi_block_t *b = (ai->partial && ai->block_index < ai->partial->block_count)
                                       ? &ai->partial->blocks[ai->block_index]
                                       : NULL;
            const char *name = (b && b->kind == PI_BLOCK_TOOL_CALL && b->u.tool_call.name)
                                    ? b->u.tool_call.name
                                    : "";
            enqueue(UI_TOOL_START, strdup(name), NULL, 0, 0);
            break;
        }
        case PI_AI_EV_TOOLCALL_DELTA:
#if PI_FEATURE_PARTIAL_JSON
            if (ai->partial_args) {
                char *json = cJSON_PrintUnformatted(ai->partial_args);
                if (json) enqueue(UI_TOOL_ARGS, json, NULL, 0, 0); /* cJSON_Malloc == malloc */
            }
#endif
            break;
        case PI_AI_EV_TEXT_DELTA:
            if (ai->delta) {
                enqueue(UI_TEXT_DELTA, strdup(ai->delta), NULL, 0, ++s_text_delta_seq);
                tts_feed_delta(ai->delta); /* 边出字边播（worker 栈上同步喂） */
            }
            break;
        default:
            break; /* TEXT_START/END, THINKING_START, TOOLCALL_END: no bridge mapping */
        }
        break;
    }

    case PI_AG_EV_TOOL_EXECUTION_START:
        /* folded into the UI_TOOL_START already queued at TOOLCALL_START above
         * (blueprint §4 row 5: "并入 TOOL_START 或单独" — this impl merges, since
         * pi_ui_kind_t has no distinct "running" kind); only the timestamp for
         * TOOL_END's elapsed_ms is taken here. */
        s_tool_start_ms = now_ms();
        break;

    case PI_AG_EV_TOOL_EXECUTION_END: {
        const char *name = ev->tool_name ? ev->tool_name : "";
        const char *output =
            (ev->tool_result && ev->tool_result->output) ? ev->tool_result->output : "";
        int elapsed = (int)(now_ms() - s_tool_start_ms);
        enqueue(UI_TOOL_END, strdup(name), strdup(output), elapsed, 0);
        break;
    }

    case PI_AG_EV_MESSAGE_END:
        /* real API errors land here too (bad network/TLS/HTTP non-200): the
         * providers absorb them into stop_reason=PI_STOP_ERROR with a
         * human-readable error_message (e.g. "HTTP 401: ..." or "transport
         * error (connect/read failed)") instead of pi_agent_prompt() ever
         * crashing — forward it verbatim so the UI can show it and the user
         * can just send another prompt to retry. */
        if (ev->message && ev->message->stop_reason == PI_STOP_ERROR) {
            const char *msg = ev->message->error_message ? ev->message->error_message : "error";
            enqueue(UI_ERROR, strdup(msg), NULL, 0, 0);
            if (g_tts_session_open) { /* 出错即打断播报，不给用户读错误还在放音 */
                g_tts_session_open = false;
                volc_tts_stop();
            }
        }
        /* capture real usage off every assistant message; the last one seen
         * before AGENT_END is this run's final turn (usage.input/.output are
         * the provider-reported token counts, not an estimate). */
        if (ev->message && ev->message->role == PI_ROLE_ASSISTANT) {
            s_last_usage_input = ev->message->usage.input;
            s_last_usage_output = ev->message->usage.output;
        }
        break;

    case PI_AG_EV_AGENT_END:
        if (g_tts_session_open) { /* 文本收尾：FinishSession，余下音频继续播完 */
            g_tts_session_open = false;
            volc_tts_speak_end();
        }
        enqueue(UI_DONE, NULL, NULL, (int)s_last_usage_input, (int)s_last_usage_output);
        break;

    default:
        break; /* TURN_START/TURN_END/TOOL_EXECUTION_UPDATE: no bridge mapping needed */
    }
}

/* ---------- env/agent lifecycle ---------- */

static void ensure_env(void) {
    if (g_env_ready) return;
    memset(&g_env, 0, sizeof(g_env));
    g_env.alloc = pi_esp32_alloc();
    g_env.sys = pi_esp32_sys();
#if PI_AGENT_TASK_USE_MOCK
    pi_mock_init(&g_mock, g_responses, 2, 24);
    g_env.transport = pi_mock_transport(&g_mock);
#else
    g_env.transport = pi_esp32_transport(); /* real HTTP, TLS via esp_crt_bundle */
    g_env.fs = &EMBEDDED_MODELS_FS;          /* only consumer: pi_models_load below */
#endif
    pi_env_init(&g_env);
    g_env_ready = true;
#if !PI_AGENT_TASK_USE_MOCK
    if (!load_model_catalog()) ESP_LOGE(TAG, "model catalog unavailable; agent will not start");
#endif
}

static pi_agent_t *create_agent(void) {
    pi_agent_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.env = &g_env;
#if PI_AGENT_TASK_USE_MOCK
    cfg.model = MOCK_MODEL;
    cfg.stream_opts.api_key = "mock";
#else
    if (!g_model) return NULL;
    cfg.model = *g_model; /* shallow copy; internal pointers borrowed from g_catalog
                           * (never freed — lives for the firmware's whole run) */
    cfg.stream_opts.api_key = pi_models_api_key(g_catalog, g_model);
#endif
    cfg.system_prompt = "You are pi.";
    cfg.tools = TOOLS;
    cfg.tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);
    cfg.on_event = on_event;
    cfg.max_turns = 8;
    return pi_agent_create(&cfg);
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(g_prompt_sem, portMAX_DELAY);
        g_running = true;
        int rc = pi_agent_prompt(g_agent, g_pending_prompt);
        if (rc != PI_OK) ESP_LOGW(TAG, "pi_agent_prompt rc=%d", rc);
        g_running = false;
    }
}

/* ---------- pi_ui_bridge.h API ---------- */

static QueueHandle_t s_queue = NULL;

QueueHandle_t pi_ui_queue(void) {
    if (s_queue == NULL) s_queue = xQueueCreate(32, sizeof(pi_ui_evt_t));
    return s_queue;
}

void pi_agent_task_start(void) {
    static bool started = false;
    if (started) return;
    started = true;

    pi_ui_queue();
    g_prompt_sem = xSemaphoreCreateBinary();
    ensure_env();
    g_agent = create_agent();
    if (!g_agent) {
        ESP_LOGE(TAG, "pi_agent_create failed");
        return;
    }
    if (xTaskCreatePinnedToCore(worker, "pi_agent", PI_AGENT_TASK_STACK, NULL, PI_AGENT_TASK_PRIO,
                                &g_worker_task, PI_AGENT_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(pi_agent) failed");
    }
}

void pi_agent_task_send_prompt(const char *preset) {
    if (!g_agent) {
        ESP_LOGW(TAG, "pi_agent_task_send_prompt: not started");
        return;
    }
    snprintf(g_pending_prompt, sizeof(g_pending_prompt), "%s", preset ? preset : "");
    xSemaphoreGive(g_prompt_sem);
}

void pi_agent_task_abort(void) {
    tts_stop_async(); /* STOP/打断：文字流与播报一起停 */
    if (g_agent) pi_agent_abort(g_agent);
}

void pi_agent_task_new_session(void) {
    if (!g_agent) return;
    tts_stop_async();
    pi_agent_abort(g_agent);
    /* pi_agent_destroy() is UB while a run is in progress (pi_agent.h contract,
     * blueprint R9): wait for worker()'s pi_agent_prompt() call to return
     * (bounded — best-effort; abort() above is what actually unwinds the run). */
    for (int waited_ms = 0; g_running && waited_ms < 2000; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (g_running) {
        ESP_LOGE(TAG, "pi_agent_task_new_session: worker still running, aborting rebuild");
        return;
    }
    pi_agent_destroy(g_agent);
#if PI_AGENT_TASK_USE_MOCK
    pi_mock_deinit(&g_mock);
    pi_mock_init(&g_mock, g_responses, 2, 24); /* resets mock.next -> replay from turn 1 */
    g_env.transport = pi_mock_transport(&g_mock);
#endif
    /* models.json/catalog is not reloaded: it's an immutable embedded blob for
     * the firmware's whole run, so a new session only needs a fresh agent. */
    g_agent = create_agent();
    if (!g_agent) ESP_LOGE(TAG, "pi_agent_task_new_session: pi_agent_create failed");
}

const char *pi_agent_model_name(void) {
#if PI_AGENT_TASK_USE_MOCK
    return MOCK_MODEL.name ? MOCK_MODEL.name : (MOCK_MODEL.id ? MOCK_MODEL.id : "?");
#else
    if (!g_model) return "?";
    return g_model->name ? g_model->name : (g_model->id ? g_model->id : "?");
#endif
}

uint32_t pi_agent_context_window(void) {
#if PI_AGENT_TASK_USE_MOCK
    return (uint32_t)MOCK_MODEL.context_window; /* 0: mock model doesn't set it */
#else
    return g_model ? (uint32_t)g_model->context_window : 0;
#endif
}

bool pi_agent_tts_enabled(void) { return g_tts_enabled; }

void pi_agent_tts_set_enabled(bool enable) {
    g_tts_enabled = enable;
    if (!enable) tts_stop_async();
}
