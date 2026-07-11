// 设备端自测走带：管线采集 3 秒 → ASR 全流式转写 → 文本喂 TTS → 播放。
// 由后续接线阶段在网络就绪后调用（符号经 CMake -u 强制保留在固件中）。
#include "volc_speech_selftest.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "metalio_hal/audio_pipeline.h"
#include "volc_asr.h"
#include "volc_tts.h"

static const char* TAG = "volc_selftest";

#define RECORD_SECONDS 3

struct SelftestCtx {
    SemaphoreHandle_t done;
    char final_text[512];
    int error_code;
};

static void st_asr_delta(const char* text, void* /*ctx*/) {
    ESP_LOGI(TAG, "asr_delta: %s", text);
}

static void st_asr_final(const char* text, void* arg) {
    auto* c = static_cast<SelftestCtx*>(arg);
    strlcpy(c->final_text, text, sizeof(c->final_text));
    ESP_LOGI(TAG, "asr_final: %s", text);
}

static void st_asr_error(int code, const char* msg, void* arg) {
    auto* c = static_cast<SelftestCtx*>(arg);
    c->error_code = code;
    ESP_LOGE(TAG, "asr_error %d: %s", code, msg);
}

static void st_tts_audio_start(void* /*ctx*/) {
    ESP_LOGI(TAG, "tts: first audio, speaking...");
}

static void st_tts_finished(void* arg) {
    auto* c = static_cast<SelftestCtx*>(arg);
    xSemaphoreGive(c->done);
}

static void st_tts_error(int code, const char* msg, void* arg) {
    auto* c = static_cast<SelftestCtx*>(arg);
    c->error_code = code;
    ESP_LOGE(TAG, "tts_error %d: %s", code, msg);
    xSemaphoreGive(c->done);
}

void volc_speech_selftest(void) {
    ESP_LOGI(TAG, "=== volc_speech selftest: record %ds -> ASR -> TTS ===",
             RECORD_SECONDS);
    static SelftestCtx ctx;
    memset(ctx.final_text, 0, sizeof(ctx.final_text));
    ctx.error_code = 0;
    if (!ctx.done) ctx.done = xSemaphoreCreateBinary();

    // —— ASR：管线采集 3 秒推流 ——
    volc_asr_callbacks_t asr_cbs = {};
    asr_cbs.on_delta = st_asr_delta;
    asr_cbs.on_final = st_asr_final;
    asr_cbs.on_error = st_asr_error;
    asr_cbs.ctx = &ctx;

    esp_err_t err = volc_asr_start(&asr_cbs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "asr start failed: %s (network up? keys ok?)",
                 esp_err_to_name(err));
        return;
    }

    mhal::audio_pipeline::CaptureConfig cap_cfg;  // 默认 20ms 帧 + 能量 VAD
    mhal::audio_pipeline::CaptureCallbacks cap_cbs;
    cap_cbs.on_frame = [](const int16_t* pcm, size_t samples) {
        volc_asr_feed(pcm, samples);
    };
    cap_cbs.on_vad = [](bool speaking) {
        ESP_LOGI(TAG, "vad: %s", speaking ? "speech" : "silence");
    };
    if (!mhal::audio_pipeline::StartCapture(cap_cfg, cap_cbs)) {
        ESP_LOGE(TAG, "capture start failed");
        volc_asr_abort();
        return;
    }
    ESP_LOGI(TAG, "recording... speak now");
    vTaskDelay(pdMS_TO_TICKS(RECORD_SECONDS * 1000));
    mhal::audio_pipeline::StopCapture();

    err = volc_asr_stop(10000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "asr stop/final failed: %s", esp_err_to_name(err));
    }

    const char* text = ctx.final_text[0] ? ctx.final_text
                                         : "自检没有听清，你好，我是小派。";
    ESP_LOGI(TAG, "speaking back: %s", text);

    // —— TTS：合成并经播放管线放音 ——
    volc_tts_callbacks_t tts_cbs = {};
    tts_cbs.on_audio_start = st_tts_audio_start;
    tts_cbs.on_finished = st_tts_finished;
    tts_cbs.on_error = st_tts_error;
    tts_cbs.ctx = &ctx;

    err = volc_tts_speak_begin(&tts_cbs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tts begin failed: %s", esp_err_to_name(err));
        return;
    }
    volc_tts_feed_text(text);
    volc_tts_speak_end();
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "tts timeout, stopping");
        volc_tts_stop();
    }
    ESP_LOGI(TAG, "=== selftest done ===");
}
