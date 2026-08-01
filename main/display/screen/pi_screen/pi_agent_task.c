/* pi-c — MetalioClaw5 pi_screen App: agent thread + real API transport +
 * on_event -> pi_ui_evt_t bridge (blueprint §2/§3 WP-B, §4 event map).
 *
 * Production talks to the real API: transport = pi_esp32_transport() (TLS via
 * esp_crt_bundle, already wired by the port) and model/provider config is built
 * at runtime from NVS by device_config (Web 后台填的 API Key 注入内置模板，或用户
 * 自备的整份 models JSON) — 固件不打包密钥。nvs_models_json_read() below is a
 * one-function pi_fs_t shim that hands that in-memory string to the real
 * PI_FEATURE_MODELS_JSON loader (pi_models_load, full catalog, keeps
 * model.compat/thinking_level_map alive — unlike pi_models_json_load_first,
 * which drops compat) exactly as if it had come off a filesystem.
 * Keeping the full catalog matters here: the deepseek entries carry a compat
 * override (requiresReasoningContentOnAssistantMessages/thinkingFormat=deepseek)
 * that PI_FEATURE_COMPAT needs to talk to DeepSeek correctly, and that override
 * only survives via the catalog API.
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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "device_config.h"
#include "pi/pi.h"
#include "pi_card/pi_card_media.h"
#include "pi_card/pi_card_tools.h"
#include "stock/stock_tool.h"
#include "pi_esp32.h"
#include "pi_media_focus.h"
#include "volc_tts.h"

/* Offline-debug channel: flip to 1 locally (or define via the build, e.g.
 * sim's -DPI_SIM_MOCK=ON) to replay the two-turn Anthropic mock script
 * instead of hitting the real API (no network required). Off by default —
 * production always uses models.json + pi_esp32_transport(). */
#ifndef PI_AGENT_TASK_USE_MOCK
#define PI_AGENT_TASK_USE_MOCK 0
#endif

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

/* ---------- env + real model catalog (models.json, from NVS) ---------- */
static pi_env_t g_env;
static pi_agent_t *g_agent;
static bool g_env_ready = false;

#if !PI_AGENT_TASK_USE_MOCK
/* models.json 的内容来自 NVS：用户在 Web 后台填的 API Key 注入内置模型模板，或
 * （高级模式）直接是用户粘的一整份 models JSON——两种都由
 * device_config_build_models_json() 合成，固件里不含任何密钥。
 * nvs_models_json_read() 是个一函数 pi_fs_t 垫片，把这份内存里的串交给 pi-c 真正的
 * PI_FEATURE_MODELS_JSON 加载器（pi_models_load），就像它是从文件系统读来的一样。
 * 未配置时返回 NULL → pi_models_load 失败 → agent 不启动（UI 照常跑，引导页会提示
 * 扫码配置）。 */
static pi_models_catalog_t *g_catalog = NULL;
static const pi_model_t *g_model = NULL; /* borrowed from g_catalog; catalog never freed */

static char *nvs_models_json_read(void *ctx, const char *path, size_t *out_len,
                                  const pi_alloc_t *alloc) {
    (void)ctx;
    (void)path;
    char *json = device_config_build_models_json(); /* malloc'd; NULL = 未配置 */
    if (!json) {
        ESP_LOGE(TAG, "no LLM config in NVS (configure via the web admin)");
        return NULL;
    }
    size_t len = strlen(json);
    char *buf = (char *)pi_malloc(alloc, len + 1); /* pi_models_load frees this */
    if (buf) {
        memcpy(buf, json, len + 1);
        if (out_len) *out_len = len;
    }
    free(json);
    return buf;
}

static const pi_fs_t NVS_MODELS_FS = {.read_file = nvs_models_json_read};

/* Loads the whole catalog (not pi_models_json_load_first: that convenience
 * snapshot drops model.compat, which DeepSeek needs). Picks catalog entry 0
 * (第一个 provider 的第一个模型 = 用户在后台「模型」框填的那个，device_config
 * 合成时已把它排到第一位)。 */
static bool load_model_catalog(void) {
    int rc = pi_models_load(&g_env, "models.json" /* ignored by the NVS fs shim */, &g_catalog);
    if (rc != PI_OK || !g_catalog) {
        ESP_LOGE(TAG, "pi_models_load failed rc=%d", rc);
        return false;
    }
    g_model = pi_models_at(g_catalog, 0);
    if (!g_model) {
        ESP_LOGE(TAG, "model catalog has no models");
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
static SemaphoreHandle_t g_prompt_mutex; /* 保护 g_pending_prompt 指针 */
static SemaphoreHandle_t
    g_agent_mutex; /* 串行化 g_agent 的 abort 与 destroy/create（重建在 worker 线程） */
static char* g_pending_prompt = NULL;           /* 动态分配，消费后 free：消除旧 512B 截断 */
static volatile bool g_running = false;         /* true while pi_agent_prompt() is on the stack */
/* 「下次吐字时切掉上一轮尚未播完的 TTS」——由 pi_agent_task_inject 的 steer 分支置位，UI 线程
   在该轮首个 TEXT_DELTA 处取走（见 pi_agent_task_tts_take_cut 的调用点）。理由见 inject。 */
static volatile bool g_tts_cut_pending = false;
static volatile bool g_rebuild_pending = false; /* new_session 请求，由 worker 消费做重建 */
/* 会话代次：new_session（LVGL 线程）自增；worker 每轮开跑前快照到 g_active_gen，
 * 打进它 enqueue 的每个事件；DrainQueueTick 丢弃 gen 与当前不符的旧会话残余事件。
 * 32 位对齐读写在目标架构上原子，且只有 new_session 单线程写，volatile 够用。 */
static volatile uint32_t g_session_gen = 0;
static uint32_t g_active_gen = 0; /* 当前 run 所属代次，仅 worker 任务读写 */

/* ---------- TTS（火山 volc_tts，text_delta 流式喂入） ----------
 * 关键约束：LLM 的 SSE 读循环（pi_agent_prompt）与 on_event 同跑在 worker 栈
 * 上，而 volc_tts_speak_begin/feed_text 都是会阻塞数秒~数十秒的调用（首字 WSS
 * 握手、以及被音频实时播放背压节流的 WS send）。若在 on_event 里同步喂，读循环
 * 就被拖住不读 LLM socket，连接被长时间占用→ DeepSeek 侧空闲/写超时→
 * "transport error(connect/read failed)"，且文本越长越明显。
 *
 * 因此这里把所有会阻塞的 volc_tts 调用挪到独立的 tts_pump 任务：喂入方只把
 * 朗读文本追加进一段内存缓冲（g_tts_pending）并唤醒 pump（全程非阻塞），读
 * 循环得以全速读完 LLM 流并尽快释放连接；pump 按音频自己的节奏消费缓冲。
 *
 * 文本来源：由 UI 侧（pi_screen）驱动而非 on_event —— UI 侧持有 markdown 解析
 * 上下文，把回复剥成纯文本（去掉加粗/标题/URL 等 markdown）再经 pi_agent_task_tts_feed 喂进来，
 * 使朗读内容 == 屏幕显示内容。run_start/feed/run_end 由 UI 在 AGENT_START/文本
 * delta/DONE 时调；cancel 由打断/新会话/关开关/出错触发。
 *
 * 并发：g_tts_pending / g_tts_run_gen / g_tts_run_ended 由 g_tts_lock 保护
 *（worker 与 LVGL/VoiceTask 的 cancel 会并发）。g_tts_run_gen 是"代次"：每个新
 * run（AGENT_START）与每次 cancel 都 ++，pump 据此丢弃过期文本、放弃被打断的
 * 旧会话。g_tts_enabled 跨线程只读写 bool，无需锁。 */
#define PI_TTS_PENDING_MAX (64 * 1024) /* 累积上限：音频跟不上时丢多余文本而非无界增长 */

static volatile bool g_tts_enabled = true; /* 真值由 pi_screen 从 NVS 灌入 */

/* TTS 连接级退避。speak_begin 每次失败重试都是一次完整 TLS 握手（重 CPU 重堆），
 * agent 多轮工具调用下 failed 逐 run 复位会演成握手风暴——真机实测饿死解码线程
 * 与 LVGL（音频蹦帧、UI 冻结），并把内部堆耗到 0 触发 esp-hosted sdio assert 重启。
 * 故失败后指数退避（15s 起、10min 封顶），终身配额类错误（45000292）直接退避到
 * 重启为止。秒数存 int32（RV32 对齐读写原子，跨 WS 回调线程与 pump 任务安全）。 */
static volatile int32_t g_tts_backoff_until_s = 0;
static volatile int32_t g_tts_fail_streak = 0;
#define TTS_ERR_QUOTA_LIFETIME 45000292

static SemaphoreHandle_t g_tts_lock;   /* 保护下面这组共享态 */
static SemaphoreHandle_t g_tts_signal; /* binary：唤醒 pump */
static char *g_tts_pending = NULL;      /* 当前 run 尚未喂给 TTS 的文本 */
static size_t g_tts_pending_len = 0;
static size_t g_tts_pending_cap = 0;
static uint32_t g_tts_run_gen = 0;      /* 每个新 run 与每次 cancel 都 ++ */
static bool g_tts_run_ended = false;    /* 当前 gen 是否已 AGENT_END */
static bool g_tts_overflow_warned = false; /* 每 run 只告警一次溢出 */
/* pump 正拿着一段文本在喂（含 speak_begin 的 WSS 握手窗口——那段时间
 * volc_tts_is_speaking() 还是 false，靠它补上，见 pi_agent_task_tts_active）。 */
static volatile bool g_tts_pump_busy = false;

static void tts_cancel_run(void);

/* Stage D 焦点仲裁：首帧音频即将出声 → 立即 Suspend 音乐；本次播报排空/出错结束 →
 * 安排一次去抖 Resume 检查（见 pi_media_focus.h/.cc，回合级兜底另在 pi_screen 的
 * UI_DONE/UI_ERROR 里补）。两个回调都跑在 volc_tts 内部任务上下文，必须非阻塞——
 * pi_media_focus 的实现只是原子计数 + detach 一个短睡眠线程，满足这一点。 */
static void tts_on_audio_start(void *ctx) {
    (void)ctx;
    pi_media_focus_tts_audio_start();
}

static void tts_on_finished(void *ctx) {
    (void)ctx;
    pi_media_focus_tts_ended();
}

static void tts_on_error(int code, const char *msg, void *ctx) {
    (void)ctx;
    ESP_LOGW(TAG, "tts error %d: %s", code, msg);
    if (code == TTS_ERR_QUOTA_LIFETIME) {
        /* 终身配额耗尽：重试永远不会成功，本次开机不再发起任何 TTS 连接 */
        g_tts_backoff_until_s = INT32_MAX;
        ESP_LOGW(TAG, "tts lifetime quota exhausted, TTS disabled until reboot");
    }
    /* 出错即作废本 run（翻代次让 pump 放弃该会话）并异步 stop：volc 层的 flush 已
     * 挪到持 api_lock 的收尾路径（emit_error 不再在 WS 回调里 FlushPlayback），不
     * stop 的话已缓冲的几秒音频会一直播到下一次 speak_begin 才被清；且 pump 私有
     * 的 session_open 也要靠这里翻代次才会复位，否则后续 feed 会打到死会话上。 */
    tts_cancel_run();
    pi_media_focus_tts_ended(); /* 出错也是"本次播报结束"，别让音乐永久停在让路态 */
}

static const volc_tts_callbacks_t TTS_CBS = {.on_audio_start = tts_on_audio_start,
                                             .on_finished = tts_on_finished,
                                             .on_error = tts_on_error,
                                             .ctx = NULL};

/* volc_tts_stop() 打断在播会话时最坏阻塞 ~2s（等服务端 SessionCanceled），
 * 而它的调用点（STOP 按钮/状态栏开关/新会话）都在 LVGL 线程——丢进一次性
 * 小任务执行，扬声器静音本身在 stop 内部第一步就发生，感知不到延迟。
 *
 * 代次戳：cancel 派生的异步 stop 若在运行时已被更新的 run 顶掉（g_tts_run_gen
 * 变了），说明它是过期的——直接跳过，避免误停后开的新会话（新会话遗留的旧会话
 * 由 pump 的 speak_begin INVALID_STATE 重试清理）。没有这个守卫时，一次打断的
 * 异步 stop 可能延迟落在紧接着的新回复播报中途，把它掐断。 */
static void tts_stop_worker(void *arg) {
    uint32_t my_gen = (uint32_t)(uintptr_t)arg;
    bool stale = false;
    if (g_tts_lock) {
        xSemaphoreTake(g_tts_lock, portMAX_DELAY);
        stale = (g_tts_run_gen != my_gen);
        xSemaphoreGive(g_tts_lock);
    }
    if (!stale) volc_tts_stop();
    vTaskDelete(NULL);
}

static void tts_stop_async(uint32_t gen) {
    if (xTaskCreate(tts_stop_worker, "tts_stop", 4096, (void *)(uintptr_t)gen, 4, NULL) != pdPASS) {
        volc_tts_stop(); /* 退化为同步（内存紧张时）*/
    }
}

static inline void tts_signal(void) {
    if (g_tts_signal) xSemaphoreGive(g_tts_signal); /* binary：重复 give 合并成一次唤醒 */
}

/* 需持有 g_tts_lock。清空待发送文本（保留 cap 复用，不 free）。 */
static void tts_pending_reset_locked(void) {
    g_tts_pending_len = 0;
    g_tts_overflow_warned = false;
}

/* 新 run 开始 —— 作废旧缓冲、翻代次。非阻塞。由 UI 侧(pi_screen)在收到
 * UI_AGENT_START 时调用（TTS 文本生命周期改由 UI 驱动：UI 侧才有 markdown
 * 解析上下文，能把朗读文本剥成纯文本再喂进来）。 */
void pi_agent_task_tts_run_start(void) {
    if (!g_tts_lock) return;
    xSemaphoreTake(g_tts_lock, portMAX_DELAY);
    tts_pending_reset_locked();
    g_tts_run_gen++;
    g_tts_run_ended = false;
    xSemaphoreGive(g_tts_lock);
    tts_signal();
}

/* 追加一段（已剥离 markdown 的）朗读文本进缓冲并唤醒 pump —— 非阻塞。 */
void pi_agent_task_tts_feed(const char *plain_utf8) {
    if (!g_tts_enabled || !g_tts_lock || !plain_utf8 || !plain_utf8[0]) return;
    const char *delta = plain_utf8;
    size_t add = strlen(delta);
    xSemaphoreTake(g_tts_lock, portMAX_DELAY);
    if (g_tts_pending_len + add > PI_TTS_PENDING_MAX) {
        if (!g_tts_overflow_warned) {
            ESP_LOGW(TAG, "tts pending full (%d B), dropping further speech text this run",
                     (int)PI_TTS_PENDING_MAX);
            g_tts_overflow_warned = true;
        }
        xSemaphoreGive(g_tts_lock);
        return; /* 文本 UI 不受影响，仅本 run 后续不再喂 TTS */
    }
    if (g_tts_pending_len + add + 1 > g_tts_pending_cap) {
        size_t newcap = g_tts_pending_cap ? g_tts_pending_cap : 512;
        while (newcap < g_tts_pending_len + add + 1) newcap *= 2;
        char *grown = (char *)realloc(g_tts_pending, newcap);
        if (!grown) {
            xSemaphoreGive(g_tts_lock);
            ESP_LOGW(TAG, "tts pending realloc failed, dropping delta");
            return;
        }
        g_tts_pending = grown;
        g_tts_pending_cap = newcap;
    }
    memcpy(g_tts_pending + g_tts_pending_len, delta, add);
    g_tts_pending_len += add;
    g_tts_pending[g_tts_pending_len] = '\0';
    xSemaphoreGive(g_tts_lock);
    tts_signal();
}

/* 本 run 文本已完 —— pump 排空缓冲后 speak_end。非阻塞。由 UI 侧在 UI_DONE 调。 */
void pi_agent_task_tts_run_end(void) {
    if (!g_tts_lock) return;
    xSemaphoreTake(g_tts_lock, portMAX_DELAY);
    g_tts_run_ended = true;
    xSemaphoreGive(g_tts_lock);
    tts_signal();
}

/* 打断/新会话/关闭/出错：作废当前 run 未播文本（翻代次让 pump 放弃旧会话），
 * 并异步停播在播音频。可从任意任务调用，不阻塞调用线程。 */
static void tts_cancel_run(void) {
    uint32_t gen = 0;
    if (g_tts_lock) {
        xSemaphoreTake(g_tts_lock, portMAX_DELAY);
        tts_pending_reset_locked();
        gen = ++g_tts_run_gen;
        g_tts_run_ended = false;
        xSemaphoreGive(g_tts_lock);
    }
    tts_stop_async(gen); /* 代次戳：过期 stop 自动跳过，不误伤新会话 */
    tts_signal();
}

/* TTS pump 任务：唯一调用会阻塞的 volc_tts_speak_begin/feed_text/speak_end 的
 * 地方，与 SSE 读循环彻底解耦。session_open/failed/pump_gen 为本任务私有态。 */
static void tts_pump(void *arg) {
    (void)arg;
    uint32_t pump_gen = 0;
    bool session_open = false;
    bool failed = false;
    for (;;) {
        xSemaphoreTake(g_tts_signal, portMAX_DELAY);

        /* 快照代次/收尾标志，并整段夺走待发送缓冲（留空缓冲给 worker 继续写）*/
        xSemaphoreTake(g_tts_lock, portMAX_DELAY);
        uint32_t gen = g_tts_run_gen;
        char *chunk = NULL;
        if (g_tts_pending_len > 0) {
            chunk = g_tts_pending;
            g_tts_pending = NULL;
            g_tts_pending_cap = 0;
            g_tts_pending_len = 0;
        }
        g_tts_pump_busy = (chunk != NULL);
        xSemaphoreGive(g_tts_lock);

        /* 代次变化（新 run 或被 cancel）：放弃旧会话本地态；旧会话由 cancel 的
         * volc_tts_stop 负责收尾，这里不调 speak_end。 */
        if (gen != pump_gen) {
            pump_gen = gen;
            session_open = false;
            failed = false;
        }

        if (!g_tts_enabled) { /* 中途关闭：丢弃并回收 */
            free(chunk);
            continue;
        }

        if (chunk && !failed) {
            if (!session_open) {
                int32_t now_s = (int32_t)(esp_timer_get_time() / 1000000);
                if (now_s < g_tts_backoff_until_s) {
                    failed = true; /* 退避期内：本 run 静默，不发起 TLS 握手 */
                } else {
                    esp_err_t err = volc_tts_speak_begin(&TTS_CBS);
                    if (err == ESP_ERR_INVALID_STATE) { /* 上一场还在排空：打断后重试一次 */
                        volc_tts_stop();
                        err = volc_tts_speak_begin(&TTS_CBS);
                    }
                    if (err != ESP_OK) {
                        int32_t streak = g_tts_fail_streak + 1;
                        g_tts_fail_streak = streak;
                        int shift = streak - 1 > 5 ? 5 : streak - 1;
                        int32_t backoff = 15 << shift; /* 15s..480s */
                        if (g_tts_backoff_until_s != INT32_MAX)
                            g_tts_backoff_until_s = now_s + backoff;
                        ESP_LOGW(TAG, "tts speak_begin failed (%d), muted for this run, backoff %ds",
                                 (int)err, (int)backoff);
                        failed = true;
                    } else {
                        g_tts_fail_streak = 0;
                        session_open = true;
                    }
                }
            }
            if (session_open) volc_tts_feed_text(chunk); /* 阻塞点落在 pump，不碰读循环 */
        }
        free(chunk);
        g_tts_pump_busy = false;

        /* 收尾：本 run 已结束且缓冲排空、代次未变 → FinishSession，余音继续播完。
         * 在锁下复查（feed_text 阻塞期间可能又来了新 delta 或发生了 cancel）。 */
        xSemaphoreTake(g_tts_lock, portMAX_DELAY);
        bool drained = (gen == g_tts_run_gen) && g_tts_run_ended && g_tts_pending_len == 0;
        xSemaphoreGive(g_tts_lock);
        if (drained && session_open) {
            volc_tts_speak_end();
            session_open = false;
        }
    }
}

/* ---------- pi_card 声明式 UI 工具（ui_render / ui_update / ui_close） ----------
 * 工具名只能匹配 ^[a-zA-Z0-9_-]+$（OpenAI/DeepSeek 兼容 API 的 function.name 约束）——
 * 不能含点号，否则请求被拒 "invalid tools[N].function.name not match pattern"。故用下划线。
 * execute 在 worker 线程调 pi_card_tool_*（校验 + 入 pi_ui_queue，不碰 LVGL），
 * 秒回，绝不在 SSE 读循环上做重活。真正建控件在 pi_screen 的 DrainQueueTick。 */
static int card_tool_run(char *(*fn)(const cJSON *, bool *), const pi_alloc_t *alloc,
                         const cJSON *args, pi_tool_result_t *out) {
    bool is_err = false;
    char *res = fn(args, &is_err);
    out->output = pi_strdup(alloc, res ? res : "error");
    out->is_error = is_err;
    free(res);
    return PI_OK;
}
static int ui_render_exec(const pi_alloc_t *alloc, const char *id, const cJSON *args,
                          volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                          void *user, pi_tool_result_t *out) {
    (void)id; (void)abort_flag; (void)on_update; (void)update_user; (void)user;
    return card_tool_run(pi_card_tool_render, alloc, args, out);
}
static int ui_update_exec(const pi_alloc_t *alloc, const char *id, const cJSON *args,
                          volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                          void *user, pi_tool_result_t *out) {
    (void)id; (void)abort_flag; (void)on_update; (void)update_user; (void)user;
    return card_tool_run(pi_card_tool_update, alloc, args, out);
}
static int ui_close_exec(const pi_alloc_t *alloc, const char *id, const cJSON *args,
                         volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                         void *user, pi_tool_result_t *out) {
    (void)id; (void)abort_flag; (void)on_update; (void)update_user; (void)user;
    return card_tool_run(pi_card_tool_close, alloc, args, out);
}
/* stock 行情查询：worker 线程同步阻塞抓取（每次 HTTP 6s 超时）——模型本来就在等
 * tool 结果，且 TTS/ASR 在独立任务，阻塞只延迟 agent 循环自身。 */
static int stock_exec(const pi_alloc_t *alloc, const char *id, const cJSON *args,
                      volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                      void *user, pi_tool_result_t *out) {
    (void)id; (void)abort_flag; (void)on_update; (void)update_user; (void)user;
    return card_tool_run(pi_stock_tool_run, alloc, args, out);
}
/* media 播放查询/起播：worker 线程同步扫盘/建表后 StagePlaylist（几十 ms 内返回，
 * 解码/播放在 MediaController 后台线程）。绝不碰 LVGL。 */
static int media_exec(const pi_alloc_t *alloc, const char *id, const cJSON *args,
                      volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                      void *user, pi_tool_result_t *out) {
    (void)id; (void)abort_flag; (void)on_update; (void)update_user; (void)user;
    return card_tool_run(pi_media_tool_run, alloc, args, out);
}

/* 非 const：ui_render 项的 description 在 pi_agent_task_start 里由 pi_card_render_desc()
 * （动态生成，见 pi_card_tools.h）运行时回填，替掉这里的占位空串。 */
static pi_agent_tool_t TOOLS[] = {
    {
        .def = {.name = "ui_render",
                .description = "",
                .parameters_schema_json = PI_CARD_RENDER_SCHEMA},
        .execute = ui_render_exec,
    },
    {
        .def = {.name = "ui_update",
                .description = PI_CARD_UPDATE_DESC,
                .parameters_schema_json = PI_CARD_UPDATE_SCHEMA},
        .execute = ui_update_exec,
    },
    {
        .def = {.name = "ui_close",
                .description = PI_CARD_CLOSE_DESC,
                .parameters_schema_json = PI_CARD_CLOSE_SCHEMA},
        .execute = ui_close_exec,
    },
    {
        .def = {.name = "stock",
                .description = PI_STOCK_TOOL_DESC,
                .parameters_schema_json = PI_STOCK_TOOL_SCHEMA},
        .execute = stock_exec,
    },
    {
        .def = {.name = "media",
                .description = PI_MEDIA_TOOL_DESC,
                .parameters_schema_json = PI_MEDIA_TOOL_SCHEMA},
        .execute = media_exec,
    },
};

/* ---------- pi event -> pi_ui_evt_t (blueprint §4 mapping table) ---------- */

static void enqueue(pi_ui_kind_t kind, char *s1, char *s2, int i1, int i2) {
    pi_ui_evt_t evt;
    evt.kind = kind;
    evt.s1 = s1;
    evt.s2 = s2;
    evt.s3 = NULL; /* 本地事件不用 s3；卡片事件由 pi_card_host 侧自建 evt 设置 */
    evt.i1 = i1;
    evt.i2 = i2;
    evt.gen = g_active_gen;
    QueueHandle_t q = pi_ui_queue();
    if (xQueueSend(q, &evt, 0) != pdTRUE) {
        /* 大工具调用（如整卡 JSON）流式期间每个增量都可能撞满队列，逐条告警会以
         * ~20Hz 刷串口并加剧 CPU 饥饿（真机实测 30s 刷 1686 条），聚合到每秒一条。 */
        static int drop_count = 0;
        static int64_t drop_last_us = 0;
        drop_count++;
        int64_t now = esp_timer_get_time();
        if (now - drop_last_us >= 1000000) {
            ESP_LOGW(TAG, "pi_ui_queue full, dropped %d evts this sec (last kind=%d)", drop_count,
                     (int)kind);
            drop_count = 0;
            drop_last_us = now;
        }
        free(s1);
        free(s2);
    }
}

/* ---------- TEXT_DELTA 溢出缓冲（队列满时正文不丢）----------
 * UI 事件队列（深 32）满时，enqueue 对非 TEXT_DELTA 事件丢弃可接受，但 TEXT_DELTA
 * 一丢屏上就缺字。这里把入队失败的正文暂存到一个 malloc 增长的缓冲，下一次
 * TEXT_DELTA（或 AGENT_END 冲洗）时 prepend 合并回流，保持字序。只在 worker 任务
 * （on_event 同步跑在 worker 栈上）读写，无需锁；每轮 AGENT_START 清空以配合会话代次。 */
static char* g_text_overflow = NULL;
static size_t g_text_overflow_len = 0;

static void text_overflow_clear(void) {
    free(g_text_overflow);
    g_text_overflow = NULL;
    g_text_overflow_len = 0;
}

static void text_overflow_append(const char* s, size_t n) {
    if (!s || n == 0)
        return;
    char* grown = (char*)realloc(g_text_overflow, g_text_overflow_len + n + 1);
    if (!grown)
        return; /* OOM：这段确实丢了，但已尽力，不崩 */
    memcpy(grown + g_text_overflow_len, s, n);
    g_text_overflow_len += n;
    grown[g_text_overflow_len] = '\0';
    g_text_overflow = grown;
}

/* TEXT_DELTA 专用入队：先把溢出缓冲与本次 delta 合并成一个事件再入队，入队失败
 * 则整段（含合并进来的历史）退回溢出缓冲，绝不丢字。delta 传 NULL/空时仅冲洗
 * 残留溢出（AGENT_END 收尾用）。 */
static void enqueue_text_delta(const char* delta) {
    size_t ol = g_text_overflow_len;
    size_t dl = delta ? strlen(delta) : 0;
    if (ol == 0 && dl == 0)
        return;
    char* merged = (char*)malloc(ol + dl + 1);
    if (!merged) { /* 合并分配失败：溢出缓冲原样保留，把新 delta 也并进去，下次再试 */
        text_overflow_append(delta, dl);
        return;
    }
    if (ol)
        memcpy(merged, g_text_overflow, ol);
    if (dl)
        memcpy(merged + ol, delta, dl);
    merged[ol + dl] = '\0';
    text_overflow_clear(); /* 历史溢出已转移进 merged */
    pi_ui_evt_t evt;
    evt.kind = UI_TEXT_DELTA;
    evt.s1 = merged;
    evt.s2 = NULL;
    evt.s3 = NULL; /* 本地事件不用 s3（与 enqueue 一致），避免残留未初始化字段 */
    evt.i1 = 0;
    evt.i2 = 0; /* 诚实化：流式期间不发序号，输出量以 DONE 的 usage 为准 */
    evt.gen = g_active_gen;
    if (xQueueSend(pi_ui_queue(), &evt, 0) != pdTRUE) {
        text_overflow_append(merged, ol + dl); /* 队列仍满：退回溢出，下段再回流 */
        free(merged);
    }
}

static uint64_t now_ms(void) {
    const pi_sys_t *sys = pi_esp32_sys();
    return (sys && sys->now_ms) ? sys->now_ms(sys->ctx) : 0;
}

/* per-run cursors (single in-flight prompt at a time, reset at AGENT_START) */
static bool s_thinking_open = false;
static uint64_t s_tool_start_ms = 0;
/* 会话累计用量：每条 assistant 消息的 MESSAGE_END 都累加（含多工具回合的中间消息，
 * 旧实现只留最后一条、且被每轮覆盖——用户看到的 IN/OUT"每次发送就重置"即此）。
 * IN 累加的是**完整 prompt 量**（input+cache_read+cache_write）：pi-c 的 usage.input
 * 刨掉了 DeepSeek 上下文缓存命中部分（对算钱正确，对"消耗了多少 token"误导）。
 * 跨 run 存活，rebuild_agent（new_session）清零；写在 worker/事件线程，读在 LVGL
 * 线程（32 位对齐读写在目标架构原子，同 g_session_gen 的约定）。
 * s_last_ctx_tokens = 最后一条 assistant 消息的完整 prompt 量 = 当前上下文占用。 */
static uint32_t s_sess_in_tokens = 0;
static uint32_t s_sess_out_tokens = 0;
static uint32_t s_last_ctx_tokens = 0;

static void on_event(const pi_agent_event_t *ev, void *user) {
    (void)user;
    switch (ev->kind) {
    case PI_AG_EV_AGENT_START:
        s_thinking_open = false;
        /* 用量累计是会话级的，这里不清（只在 rebuild_agent 清零）。 */
        /* TTS run 生命周期由 UI 侧驱动（pi_screen 收到 UI_AGENT_START 时调
         * pi_agent_task_tts_run_start），这里只入 UI 队列。 */
        text_overflow_clear(); /* 新一轮：清掉上一轮/被打断轮残留的溢出正文 */
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
                enqueue_text_delta(ai->delta); /* 队列满不丢字：并溢出缓冲后入队 */
                /* TTS 不在此喂：UI 侧从 UI_TEXT_DELTA 剥出纯文本后调
                 * pi_agent_task_tts_feed（见 pi_screen DrainQueueTick）。 */
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
        /* 诊断：pi-c 层的失败（参数 JSON 修不出来 / schema 校验 / 重复键）不经过工具
         * execute，错误只回给 LLM、串口全盲——真机曾连败 5 次 ui_render 而日志零线索。
         * 这里对一切 is_error 的工具结果打头部片段（错误 echo 可达数 KB，截断防刷屏）。 */
        if (ev->tool_result && ev->tool_result->is_error) {
            char head[200];
            strlcpy(head, output, sizeof(head));
            ESP_LOGW(TAG, "tool %s FAILED (%d ms): %s", name, elapsed, head);
        }
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
            /* 诊断：真正的网络/HTTP/API 错误详情原样打串口（原来只 enqueue 到屏、串口看不到，
             * 只剩一个 pi_agent_prompt rc=-100）。真机排障靠这一条看清是 HTTP 4xx/传输失败/超时等。 */
            ESP_LOGE(TAG, "AGENT ERROR (stop_reason=ERROR): %s", msg);
            enqueue(UI_ERROR, strdup(msg), NULL, 0, 0);
            /* 出错停播由 UI 侧在 UI_ERROR 里调 pi_agent_task_tts_cancel。 */
        }
        /* 每条 assistant 消息都累计真实 usage（provider 上报，非估算）：IN 取完整
         * prompt 量（含缓存命中，见 s_sess_* 注释），OUT 取 output；同时记下本条的
         * 完整 prompt 量作为当前上下文占用（CTX 显示用）。 */
        if (ev->message && ev->message->role == PI_ROLE_ASSISTANT) {
            const pi_usage_t *u = &ev->message->usage;
            s_last_ctx_tokens = u->input + u->cache_read + u->cache_write;
            s_sess_in_tokens += s_last_ctx_tokens;
            s_sess_out_tokens += u->output;
        }
        break;

    case PI_AG_EV_AGENT_END:
        /* TTS 收尾由 UI 侧在 UI_DONE 里调 pi_agent_task_tts_run_end。 */
        enqueue_text_delta(NULL); /* 冲掉结尾残留溢出正文，避免收尾缺字 */
        enqueue(UI_DONE, NULL, NULL, (int)s_sess_in_tokens, (int)s_sess_out_tokens);
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
    g_env.fs = &NVS_MODELS_FS;               /* only consumer: pi_models_load below */
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
    cfg.system_prompt = pi_card_system_prompt(); /* function-static，深拷贝见 pi_agent.c:58-60 */
    cfg.tools = TOOLS;
    cfg.tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);
    cfg.on_event = on_event;
    cfg.max_turns = 8;
    return pi_agent_create(&cfg);
}

/* worker 任务上下文重建 agent：new_session 请求下由 worker 自己做。此刻
 * pi_agent_prompt() 一定已返回（下方 worker 循环里 destroy 只在 g_running=false 段
 * 调），满足 pi_agent_destroy 的"run 结束后才能销毁"契约（pi_agent.h / 蓝图 R9），
 * 无需 LVGL 线程自旋等待。销毁/重建窗口内 LVGL 线程仍可能调 pi_agent_task_abort
 * （屏幕卸载路径不受 UI 状态门控），g_agent_mutex 把 abort 与 destroy/create 串行化。 */
static void rebuild_agent(void) {
    xSemaphoreTake(g_agent_mutex, portMAX_DELAY);
    if (g_agent)
        pi_agent_destroy(g_agent);
#if PI_AGENT_TASK_USE_MOCK
    pi_mock_deinit(&g_mock);
    pi_mock_init(&g_mock, g_responses, 2, 24); /* resets mock.next -> replay from turn 1 */
    g_env.transport = pi_mock_transport(&g_mock);
#endif
    /* catalog 不重载：它在本次运行期内不变（改配置走 Web 后台保存 → 设备重启），
     * 所以新会话只需要一个新 agent。 */
    g_agent = create_agent();
    xSemaphoreGive(g_agent_mutex);
    if (!g_agent)
        ESP_LOGE(TAG, "rebuild_agent: pi_agent_create failed");
    /* 会话级用量累计随新会话清零。此刻旧 run 一定已返回（destroy 契约），不会再有
     * MESSAGE_END 竞争写入。 */
    s_sess_in_tokens = 0;
    s_sess_out_tokens = 0;
    s_last_ctx_tokens = 0;
}

/* 原子取出待处理 prompt 并置空；返回值所有权移交调用方（用完 free），无则 NULL。 */
static char* take_pending_prompt(void) {
    xSemaphoreTake(g_prompt_mutex, portMAX_DELAY);
    char* p = g_pending_prompt;
    g_pending_prompt = NULL;
    xSemaphoreGive(g_prompt_mutex);
    return p;
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(g_prompt_sem, portMAX_DELAY);
        /* 顺序保证：先处理 new_session 的重建，再消费 prompt，即使二者的 give 因
         * 二值信号量合并成一个 token 也不漏——本轮把两件事都办掉。 */
        if (g_rebuild_pending) {
            g_rebuild_pending = false;
            rebuild_agent();
        }
        char* prompt = take_pending_prompt();
        if (!prompt)
            continue;   /* 只是 new_session 的唤醒，无 prompt */
        if (!g_agent) { /* 重建失败：丢弃该 prompt，不崩 */
            free(prompt);
            continue;
        }
        g_active_gen = g_session_gen; /* 本 run 所属代次，其事件据此打标 */
        g_running = true;
        int rc = pi_agent_prompt(g_agent, prompt);
        if (rc != PI_OK) ESP_LOGW(TAG, "pi_agent_prompt rc=%d", rc);
        g_running = false;
        free(prompt);
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
    g_prompt_mutex = xSemaphoreCreateMutex();
    g_agent_mutex = xSemaphoreCreateMutex();
    g_tts_lock = xSemaphoreCreateMutex();
    g_tts_signal = xSemaphoreCreateBinary();
    ensure_env();
    /* ui_render 的描述是运行时动态生成的（含 DataHub::ListPaths() 拼出的活体路径清单），
     * 必须在 create_agent() 之前填好——pi_agent_create 浅拷贝 TOOLS[] 借用 description 指针
     * （pi-c pi_agent.c:67-68），此后 rebuild_agent 复用同一个 TOOLS[]，指针需常驻有效
     * （pi_card_render_desc() 内部缓存进 function-static std::string，满足这一点）。初始化
     * 顺序安全：main.cc PiScreen::Create() 注册全部路径同步先于 lv_screen_load -> LOAD 钩子
     * -> pi_agent_task_start。 */
    for (size_t i = 0; i < sizeof(TOOLS) / sizeof(TOOLS[0]); i++) {
        if (strcmp(TOOLS[i].def.name, "ui_render") == 0) {
            TOOLS[i].def.description = pi_card_render_desc();
        }
    }
    /* 8KB 预算守卫（编排者裁决补充）：DESC+system prompt 之和常驻占用上下文，两者都随
     * DataHub 路径 / CommandRegistry 命令的运行时注册增长——新加一条就可能悄悄越界。
     * sim 的 "budget" 命令是本地核验；这里在真机启动路径上补一道只告警不 fail 的信号，
     * 免得越界只能靠人工偶尔想起来去跑 sim 才发现。 */
    {
        /* 预算基线 9216->11264：弱模型鲁棒性批次给 system prompt 补了五个 few-shot 示例
         *（cells 控制/rows 表格/bind_rows 列表/toggle 显隐复合/overlay 确认复合）+ NEVER
         * 负面清单（真机实录：模型没见过的形态永远写不对，一维 rows 连拒两次；示例是最强
         * 的格式教学，~1.6KB 换首错率）。仍是软告警不 fail，越过新线即提示该重新审视示例/
         * 路径清单是否需要收敛。 */
        size_t sys_len = strlen(pi_card_system_prompt());
        size_t desc_len = strlen(pi_card_render_desc());
        if (sys_len + desc_len > 11264) {
            ESP_LOGW(TAG, "pi_card system_prompt(%u)+ui_render desc(%u)=%u bytes, over 11264 budget",
                     (unsigned)sys_len, (unsigned)desc_len, (unsigned)(sys_len + desc_len));
        }
    }
    g_agent = create_agent();
    if (!g_agent) {
        ESP_LOGE(TAG, "pi_agent_create failed");
        return;
    }
    if (xTaskCreatePinnedToCore(worker, "pi_agent", PI_AGENT_TASK_STACK, NULL, PI_AGENT_TASK_PRIO,
                                &g_worker_task, PI_AGENT_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(pi_agent) failed");
    }
    /* TTS pump：独立任务承接所有会阻塞的 volc_tts 调用，与 SSE 读循环解耦。栈 6KB
     * 覆盖 cJSON 构造 TTS 请求 + WS send + WSS 握手等待。常驻、跨会话复用。 */
    if (xTaskCreate(tts_pump, "tts_pump", 6144, NULL, PI_AGENT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(tts_pump) failed");
    }
}

void pi_agent_task_send_prompt(const char *preset) {
    if (!g_agent) {
        ESP_LOGW(TAG, "pi_agent_task_send_prompt: not started");
        return;
    }
    char* dup = strdup(preset ? preset : ""); /* 动态分配，彻底消除旧 512B 截断 */
    if (!dup) {
        ESP_LOGE(TAG, "pi_agent_task_send_prompt: strdup OOM");
        return;
    }
    xSemaphoreTake(g_prompt_mutex, portMAX_DELAY);
    free(g_pending_prompt); /* 覆盖尚未消费的旧 prompt（正常有 UI 侧 s_agent_busy 拦截）*/
    g_pending_prompt = dup;
    xSemaphoreGive(g_prompt_mutex);
    xSemaphoreGive(g_prompt_sem);
}

void pi_agent_task_abort(void) {
    tts_cancel_run(); /* STOP/打断：作废本 run 未播文本 + 异步停播 */
    xSemaphoreTake(
        g_agent_mutex,
        portMAX_DELAY); /* 防撞 worker 的 rebuild 窗口（destroy/create 很快，阻塞可忽略） */
    if (g_agent) pi_agent_abort(g_agent);
    xSemaphoreGive(g_agent_mutex);
}

void pi_agent_task_note(const char *text) {
    if (!text || !text[0]) return;
    xSemaphoreTake(g_agent_mutex, portMAX_DELAY); /* 防撞 worker 的 rebuild（destroy/create）窗口 */
    if (g_agent) {
        size_t n = 0;
        pi_agent_transcript(g_agent, &n);
        /* 会话里还没有任何消息（开机后从未对话/刚 new_session）：没有"对话中"可言，
         * 丢弃——避免用户首个问题被塞进一条无关旧备注。 */
        if (n > 0) pi_agent_steer(g_agent, text);
    }
    xSemaphoreGive(g_agent_mutex);
}

bool pi_agent_task_has_messages(void) {
    bool has = false;
    xSemaphoreTake(g_agent_mutex, portMAX_DELAY); /* 防撞 worker 的 rebuild 窗口 */
    if (g_agent) {
        size_t n = 0;
        pi_agent_transcript(g_agent, &n);
        has = n > 0;
    }
    xSemaphoreGive(g_agent_mutex);
    return has;
}

void pi_agent_task_inject(const char *text) {
    if (!g_agent || !text || !text[0]) return;
    if (g_running) {
        /* steer 是在**当前 run 内**再起一轮，不会产生新的 AGENT_START，于是 TTS run 也不会
           重开——新一轮的文本会直接 append 进同一个缓冲，老老实实排在上一轮尚未播完的音频
           后面（而 volc_tts_feed_text 的限速阀让"上一轮"能拖上好几分钟）。置个标志，等新一轮
           **真的吐出第一个字**时再切旧音频（消费点见 pi_screen 的 UI_TEXT_DELTA）：此刻就切
           只会换来几秒静音——文本还要等网络 RTT + 推理才来。 */
        g_tts_cut_pending = true;
        pi_agent_steer(g_agent, text); /* 运行中：插到下一轮之前 */
    } else {
        pi_agent_task_send_prompt(text); /* 空闲：起一轮（自带 AGENT_START → TTS run 会重开） */
    }
}

void pi_agent_task_tts_cancel(void) { tts_cancel_run(); }

bool pi_agent_task_tts_active(void) {
    if (!g_tts_enabled) return false;
    if (g_tts_pump_busy || volc_tts_is_speaking()) return true;
    bool pending = false;
    if (g_tts_lock) {
        xSemaphoreTake(g_tts_lock, portMAX_DELAY);
        pending = g_tts_pending_len > 0;
        xSemaphoreGive(g_tts_lock);
    }
    return pending;
}

bool pi_agent_task_tts_take_cut(void) {
    bool v = g_tts_cut_pending;
    g_tts_cut_pending = false;
    return v;
}

void pi_agent_task_tts_clear_cut(void) { g_tts_cut_pending = false; }

void pi_agent_task_new_session(void) {
    if (!g_agent) return;
    /* 非阻塞：只置标志 + 打断当前 run + 唤醒 worker；destroy+重建交给 worker 在它
     * 自己的循环里做（它天然满足"pi_agent_prompt 返回后才能 destroy"，不需要 LVGL
     * 线程自旋等待）。代次自增让旧 run 正在 unwind 的残余事件被 drain 丢弃。 */
    g_session_gen++; /* 仅本函数（LVGL 线程）写，单写者，++ 安全 */
    g_rebuild_pending = true;
    tts_cancel_run(); /* 作废本 run 未播文本 + 异步停播（翻代次让 pump 放弃旧会话） */
    xSemaphoreTake(g_agent_mutex, portMAX_DELAY); /* 上一次重建可能仍在途，防打到悬空指针 */
    if (g_agent)
        pi_agent_abort(g_agent);
    xSemaphoreGive(g_agent_mutex);
    xSemaphoreGive(g_prompt_sem);
}

uint32_t pi_agent_task_session_gen(void) { return g_session_gen; }

/* 当前正在执行的 run 所属代次（worker 线程侧的事件生产者据此给事件打标，与本任务自身
 * 入队事件用的 g_active_gen 完全一致——保证同一轮 run 的事件被 drain 一致过滤）。
 * 仅可从 worker 线程调用（pi_card 工具等在 run 内同步执行的路径）。 */
uint32_t pi_agent_task_run_gen(void) { return g_active_gen; }

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

uint32_t pi_agent_ctx_tokens(void) { return s_last_ctx_tokens; }

bool pi_agent_tts_enabled(void) { return g_tts_enabled; }

void pi_agent_tts_set_enabled(bool enable) {
    g_tts_enabled = enable;
    if (!enable) tts_cancel_run();
}

bool pi_agent_task_is_running(void) { return g_running; }
