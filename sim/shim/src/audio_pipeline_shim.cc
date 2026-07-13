// sim shim — mhal::audio_pipeline. No microphone on host, so StartCapture
// spawns a thread that feeds on_frame() synthetic PCM at the real frame
// cadence: a lively amplitude-varying tone while "speaking" (typing), near
// silence otherwise. That drives pi_screen's real-signal waveform (PushWaveLevel)
// through the exact same code path as hardware. IsVoiceDetected() mirrors the
// simulated typing session. (volc_asr_feed is a no-op stub on host.)
#include "metalio_hal/audio_pipeline.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "sim_hooks.h"

namespace mhal::audio_pipeline {

namespace {
std::atomic<bool> g_capturing{false};
std::thread g_cap_thread;

void CaptureLoop(CaptureConfig cfg, CaptureCallbacks cbs) {
    const int frame_ms = (cfg.frame_ms > 0) ? cfg.frame_ms : 20;
    const size_t n = static_cast<size_t>(frame_ms) * 16;  // 16kHz
    std::vector<int16_t> buf(n);
    float phase = 0.0f;
    float amp = 0.0f;  // smoothed amplitude envelope
    while (g_capturing.load()) {
        const bool voice = sim_asr_voice_detected();
        // Wander the target amplitude so bars look like real speech; near
        // silence when not "speaking".
        const float jitter = static_cast<float>(std::rand() & 0x7FFF) / 32768.0f;  // 0..1
        const float target = voice ? (5000.0f + 14000.0f * jitter) : 120.0f;
        amp += (target - amp) * 0.25f;
        const float freq = 180.0f + 90.0f * jitter;  // Hz, drifting
        const float dphase = 2.0f * 3.14159265f * freq / 16000.0f;
        for (size_t i = 0; i < n; i++) {
            phase += dphase;
            if (phase > 6.28318531f) phase -= 6.28318531f;
            float noise = (static_cast<float>(std::rand() & 0x7FFF) / 32768.0f - 0.5f) * 0.3f;
            float s = amp * (std::sin(phase) + noise);
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            buf[i] = static_cast<int16_t>(s);
        }
        if (cbs.on_frame) cbs.on_frame(buf.data(), n);
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));
    }
}
}  // namespace

int SampleRate() { return 16000; }

bool StartCapture(const CaptureConfig& cfg, CaptureCallbacks cbs) {
    if (g_capturing.exchange(true)) return false;
    g_cap_thread = std::thread(CaptureLoop, cfg, cbs);
    return true;
}

void StopCapture() {
    g_capturing = false;
    if (g_cap_thread.joinable()) g_cap_thread.join();
}

bool IsCapturing() { return g_capturing; }

bool IsVoiceDetected() { return g_capturing && sim_asr_voice_detected(); }

bool EnsurePlayback(const PlaybackConfig& cfg) {
    (void)cfg;
    return true;
}

size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms) {
    (void)pcm;
    (void)timeout_ms;
    return samples;
}

void FlushPlayback() {}

void OnPlaybackDrained(std::function<void()> cb) {
    if (cb) cb(); /* queue is always empty on host: fire immediately */
}

bool IsPlaybackIdle() { return true; }

}  // namespace mhal::audio_pipeline
