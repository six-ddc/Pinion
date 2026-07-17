// sim shim — volc_speech ASR/TTS stubs.
//
// ASR: a "session" is opened by pi_screen's VoiceTask exactly like on device;
// speech is injected by typing in the SDL window (sim_asr_type). on_delta
// fires with the full accumulated line (matching the server's full-text
// semantics); volc_asr_stop() delivers on_final synchronously.
//
// TTS: collects the streamed text deltas and prints the finished utterance to
// the console; PI_SIM_SAY=1 additionally speaks it via macOS `say`.
//
// Stage D: on_audio_start/on_finished are now fired (previously stubbed out as
// no-ops), so pi_media_focus's TTS<->media focus arbitration is exercisable in
// sim. on_audio_start fires on the first non-empty feed_text of a session
// (approximates "first audio frame about to play" without a real decode
// pipeline). on_finished fires from a detached thread after a duration
// estimated from the spoken text length (~70ms/UTF-8 char, clamped to
// [400ms, 4000ms]) so there's an observable "TTS speaking" window in logs to
// verify Suspend-before/Resume-after timing against, independent of whether
// `say` itself is actually running (PI_SIM_SAY unset -> still fires on time).
#include <signal.h>
#include <spawn.h>
#include <sys/time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

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
volc_tts_callbacks_t g_tts_cbs{};
bool g_tts_audio_started = false;  // 本会话是否已触发过 on_audio_start（只触发一次）
uint32_t g_tts_session_gen = 0;    // 每次 speak_begin ++；on_finished 的延迟线程据此判断
                                    // 会话是否已被新一场 speak_begin/stop 顶掉，避免误触发

// 粗略估算朗读这段文本需要的时长：中英文都按 ~70ms/UTF-8 字节估，夹在
// [400ms, 4000ms] 之间——只为让 on_finished 有一个可观测的延迟窗口用来验证
// Suspend/Resume 时序，不追求跟 macOS `say` 实际发声时长精确对齐。
int EstimateSpeakMs(size_t utf8_len) {
    int64_t ms = (int64_t)utf8_len * 70;
    if (ms < 400) ms = 400;
    if (ms > 4000) ms = 4000;
    return (int)ms;
}

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
    if (g_asr_cbs.on_delta != nullptr)
        g_asr_cbs.on_delta(g_asr_text.c_str(), VOLC_ASR_COMMITTED_UNKNOWN, g_asr_cbs.ctx);
}

void sim_asr_backspace(void) {
    std::lock_guard<std::mutex> lk(g_asr_mu);
    if (!g_asr_active || g_asr_text.empty()) return;
    size_t n = g_asr_text.size();
    while (n > 0 && (static_cast<unsigned char>(g_asr_text[n - 1]) & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    g_asr_text.resize(n);
    g_asr_last_input_ms = NowMs();
    if (g_asr_cbs.on_delta != nullptr)
        g_asr_cbs.on_delta(g_asr_text.c_str(), VOLC_ASR_COMMITTED_UNKNOWN, g_asr_cbs.ctx);
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
    std::lock_guard<std::mutex> lk(g_tts_mu);
    if (g_tts_open) return ESP_ERR_INVALID_STATE;
    g_tts_open = true;
    g_tts_text.clear();
    g_tts_cbs = (cbs != nullptr) ? *cbs : volc_tts_callbacks_t{};
    g_tts_audio_started = false;
    g_tts_session_gen++;
    return ESP_OK;
}

esp_err_t volc_tts_feed_text(const char* text_utf8) {
    volc_tts_callbacks_t cbs{};
    bool fire_audio_start = false;
    {
        std::lock_guard<std::mutex> lk(g_tts_mu);
        if (!g_tts_open) return ESP_ERR_INVALID_STATE;
        if (text_utf8 != nullptr) g_tts_text += text_utf8;
        // 首次真正拿到非空文本：近似真机"第一帧音频即将播放"（on_audio_start）。
        if (!g_tts_audio_started && !g_tts_text.empty()) {
            g_tts_audio_started = true;
            fire_audio_start = true;
            cbs = g_tts_cbs;
        }
    }
    if (fire_audio_start && cbs.on_audio_start != nullptr) cbs.on_audio_start(cbs.ctx);
    return ESP_OK;
}

esp_err_t volc_tts_speak_end(void) {
    volc_tts_callbacks_t cbs{};
    std::string text;
    uint32_t my_gen;
    {
        std::lock_guard<std::mutex> lk(g_tts_mu);
        if (!g_tts_open) return ESP_ERR_INVALID_STATE;
        g_tts_open = false;
        text = g_tts_text;
        cbs = g_tts_cbs;
        my_gen = g_tts_session_gen;
    }
    if (!text.empty()) {
        fprintf(stderr, "[sim][TTS] %s\n", text.c_str());
        const char* say = getenv("PI_SIM_SAY");
        if (say != nullptr && say[0] == '1') {
            std::lock_guard<std::mutex> lk(g_tts_mu);
            SayKillLocked();
            SaySpawnLocked(text);
        }
    }
    // on_finished 延迟触发，模拟"文本已喂完，音频还要放一会儿才排空"的真机时序
    // （见 EstimateSpeakMs）。用会话代次收口：若期间发生了新一场 speak_begin 或
    // volc_tts_stop（都会 ++gen 或已经把 cbs 清了），这次延迟回调直接放弃，不会
    // 误把新会话的音乐让路状态提前解除，也不会调用悬空的旧 ctx。
    if (cbs.on_finished != nullptr) {
        int delay_ms = EstimateSpeakMs(text.size());
        std::thread([cbs, my_gen, delay_ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            {
                std::lock_guard<std::mutex> lk(g_tts_mu);
                if (g_tts_session_gen != my_gen) return;  // 已被新会话/stop 顶掉
            }
            cbs.on_finished(cbs.ctx);
        }).detach();
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
    g_tts_session_gen++;  // 让任何在途的 on_finished 延迟线程放弃（会话已被打断收尾）
    SayKillLocked();
}

bool volc_tts_is_speaking(void) { return false; }

void volc_tts_shutdown(void) { volc_tts_stop(); }

}  // extern "C"
