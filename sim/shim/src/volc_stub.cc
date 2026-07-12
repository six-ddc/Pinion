// sim shim — volc_speech ASR/TTS stubs.
//
// ASR: a "session" is opened by pi_screen's VoiceTask exactly like on device;
// speech is injected by typing in the SDL window (sim_asr_type). on_delta
// fires with the full accumulated line (matching the server's full-text
// semantics); volc_asr_stop() delivers on_final synchronously.
//
// TTS: collects the streamed text deltas and prints the finished utterance to
// the console; PI_SIM_SAY=1 additionally speaks it via macOS `say`.
#include <signal.h>
#include <spawn.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "sim_hooks.h"
#include "volc_asr.h"
#include "volc_tts.h"

extern char** environ;

namespace {

int64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ---------------- ASR session ----------------

std::mutex g_asr_mu;
volc_asr_callbacks_t g_asr_cbs{};
bool g_asr_active = false;
std::string g_asr_text;
int64_t g_asr_last_input_ms = 0;  // 0 = nothing typed yet
bool g_asr_force_silence = false;

constexpr int64_t kVoiceHangoverMs = 1000;  // typing pause that reads as VAD silence

// ---------------- TTS session ----------------

std::mutex g_tts_mu;
bool g_tts_open = false;
std::string g_tts_text;
pid_t g_say_pid = -1;

void SayKillLocked() {
    if (g_say_pid > 0) {
        kill(g_say_pid, SIGTERM);
        g_say_pid = -1;
    }
}

void SaySpawnLocked(const std::string& text) {
    if (text.empty()) return;
    const char* argv[] = {"say", text.c_str(), nullptr};
    pid_t pid = -1;
    if (posix_spawnp(&pid, "say", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0) {
        g_say_pid = pid; /* reaped automatically: main() sets SIGCHLD to SIG_IGN */
    }
}

}  // namespace

// ---------------- sim hooks (driven from the SDL side) ----------------

extern "C" {

bool sim_asr_session_active(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    return g_asr_active;
}

void sim_asr_type(const char* utf8) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    if (!g_asr_active || utf8 == nullptr) return;
    g_asr_text += utf8;
    g_asr_last_input_ms = NowMs();
    g_asr_force_silence = false;
    if (g_asr_cbs.on_delta != nullptr) g_asr_cbs.on_delta(g_asr_text.c_str(), g_asr_cbs.ctx);
}

void sim_asr_backspace(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    if (!g_asr_active || g_asr_text.empty()) return;
    size_t n = g_asr_text.size();
    while (n > 0 && (static_cast<unsigned char>(g_asr_text[n - 1]) & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    g_asr_text.resize(n);
    g_asr_last_input_ms = NowMs();
    if (g_asr_cbs.on_delta != nullptr) g_asr_cbs.on_delta(g_asr_text.c_str(), g_asr_cbs.ctx);
}

void sim_asr_end_of_speech(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    if (!g_asr_active) return;
    g_asr_force_silence = true;
}

bool sim_asr_voice_detected(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    if (!g_asr_active || g_asr_force_silence || g_asr_last_input_ms == 0) return false;
    return NowMs() - g_asr_last_input_ms < kVoiceHangoverMs;
}

// ---------------- volc_asr ----------------

esp_err_t volc_asr_start(const volc_asr_callbacks_t* cbs) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    if (g_asr_active) return ESP_ERR_INVALID_STATE;
    g_asr_cbs = (cbs != nullptr) ? *cbs : volc_asr_callbacks_t{};
    g_asr_active = true;
    g_asr_text.clear();
    g_asr_last_input_ms = 0;
    g_asr_force_silence = false;
    fprintf(stderr, "[sim] ASR listening — type in the window; pause 1s (or Enter) to send\n");
    return ESP_OK;
}

esp_err_t volc_asr_feed(const int16_t* pcm, size_t samples) {
    (void)pcm;
    (void)samples;
    return ESP_OK;
}

esp_err_t volc_asr_stop(uint32_t final_timeout_ms) {
    (void)final_timeout_ms;
    volc_asr_callbacks_t cbs;
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_asr_mu);
        if (!g_asr_active) return ESP_OK;
        cbs = g_asr_cbs;
        text = g_asr_text;
        g_asr_active = false;
    }
    if (cbs.on_final != nullptr) cbs.on_final(text.c_str(), cbs.ctx);
    return ESP_OK;
}

void volc_asr_abort(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    g_asr_active = false;
    g_asr_text.clear();
}

bool volc_asr_is_active(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    return g_asr_active;
}

// ---------------- volc_tts ----------------

esp_err_t volc_tts_speak_begin(const volc_tts_callbacks_t* cbs) {
    (void)cbs; /* on_audio_start/on_finished are never fired by the stub */
    std::lock_guard<std::mutex> lk(g_tts_mu);
    if (g_tts_open) return ESP_ERR_INVALID_STATE;
    g_tts_open = true;
    g_tts_text.clear();
    return ESP_OK;
}

esp_err_t volc_tts_feed_text(const char* text_utf8) {
    std::lock_guard<std::mutex> lk(g_tts_mu);
    if (!g_tts_open) return ESP_ERR_INVALID_STATE;
    if (text_utf8 != nullptr) g_tts_text += text_utf8;
    return ESP_OK;
}

esp_err_t volc_tts_speak_end(void) {
    std::lock_guard<std::mutex> lk(g_tts_mu);
    if (!g_tts_open) return ESP_ERR_INVALID_STATE;
    g_tts_open = false;
    if (!g_tts_text.empty()) {
        fprintf(stderr, "[sim][TTS] %s\n", g_tts_text.c_str());
        const char* say = getenv("PI_SIM_SAY");
        if (say != nullptr && say[0] == '1') {
            SayKillLocked();
            SaySpawnLocked(g_tts_text);
        }
    }
    return ESP_OK;
}

esp_err_t volc_tts_wait_done(uint32_t timeout_ms) {
    (void)timeout_ms;
    return ESP_OK;
}

void volc_tts_stop(void) {
    std::lock_guard<std::mutex> lk(g_tts_mu);
    g_tts_open = false;
    g_tts_text.clear();
    SayKillLocked();
}

bool volc_tts_is_speaking(void) { return false; }

void volc_tts_shutdown(void) { volc_tts_stop(); }

}  // extern "C"
