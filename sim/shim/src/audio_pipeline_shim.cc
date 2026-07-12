// sim shim — mhal::audio_pipeline. No real capture: on_frame is never called
// (volc_asr_feed is a no-op on host anyway); IsVoiceDetected() mirrors the
// simulated typing session so pi_screen's key-mode VAD auto-send works.
#include "metalio_hal/audio_pipeline.h"

#include <atomic>

#include "sim_hooks.h"

namespace mhal::audio_pipeline {

namespace {
std::atomic<bool> g_capturing{false};
}

int SampleRate() { return 16000; }

bool StartCapture(const CaptureConfig& cfg, CaptureCallbacks cbs) {
    (void)cfg;
    (void)cbs;
    if (g_capturing.exchange(true)) return false;
    return true;
}

void StopCapture() { g_capturing = false; }

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
