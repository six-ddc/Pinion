/* sim shim — same declarations as components/metalio_hal/include/metalio_hal/
 * audio_pipeline.h. Capture produces no frames on host; IsVoiceDetected() is
 * driven by the simulated ASR typing session (see sim_hooks.h) so pi_screen's
 * VAD auto-send logic works unchanged. Playback is an accept-everything stub. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace mhal::audio_pipeline {

int SampleRate();

// —— capture ——

struct CaptureConfig {
    int frame_ms = 20;
    bool enable_vad = true;
    int vad_enter_threshold = 1000;
    int vad_exit_threshold = 400;
    int vad_hangover_ms = 600;
};

struct CaptureCallbacks {
    std::function<void(const int16_t* pcm, size_t samples)> on_frame;
    std::function<void(bool speaking)> on_vad;
};

bool StartCapture(const CaptureConfig& cfg, CaptureCallbacks cbs);
void StopCapture();
bool IsCapturing();
bool IsVoiceDetected();

// —— playback ——

struct PlaybackConfig {
    size_t queue_bytes = 64 * 1024;
    uint32_t prestart_ms = 100;
    uint32_t idle_power_off_ms = 15000;
};

bool EnsurePlayback(const PlaybackConfig& cfg = {});
size_t FeedPlayback(const int16_t* pcm, size_t samples, uint32_t timeout_ms);
void FlushPlayback();
void OnPlaybackDrained(std::function<void()> cb);
bool IsPlaybackIdle();

}  // namespace mhal::audio_pipeline
