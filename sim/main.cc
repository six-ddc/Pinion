// pi_sim — desktop (SDL2) simulator for the pi_screen UI.
//
// Replicates main/main.cc's boot chain minus hardware: an LVGL SDL window
// stands in for mhal::Init()'s display bring-up; screen creation, lifecycle
// attach and screen load are verbatim. The agent path is the real one —
// pi_agent_task.c + pi-c POSIX port (libcurl) → real DeepSeek API.
//
// Controls:
//   F1               PWR_KEY click (idle→listen, listen→cancel, chat→listen/stop)
//   F2               PWR_KEY long-press 1.2s (quick panel open/close)
//   typing           speech while listening (pause 1s = VAD silence → auto-send)
//   Enter            end of speech immediately
//   Backspace        delete last codepoint of the "utterance"
//   F12              screenshot (BMP, path from PI_SIM_SHOT or pi_sim_shot.bmp)
//   mouse            touch
//
// Env knobs: PI_SIM_SAY=1 (speak TTS via macOS `say`), PI_SIM_AUTODEMO=<text>
// (scripted demo: press key, type text, send), PI_SIM_SHOT / PI_SIM_SHOT_MS /
// PI_SIM_EXIT_MS (unattended screenshot + exit, for CI/self-test).
#include <SDL.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"

#include "IOExpander.hpp"
#include "pi_screen.h"
#include "screen_util.h"
#include "sim_hooks.h"

namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_pwr_key_pending{false};
std::atomic<bool> g_pwr_key_long_pending{false};
std::atomic<bool> g_shot_pending{false};

uint32_t TickCb() { return SDL_GetTicks(); }

// Runs inside SDL_PumpEvents — i.e. on the LVGL thread, under the LVGL lock
// (the SDL driver pumps from an lv_timer). No LVGL calls here; just record.
int EventWatch(void*, SDL_Event* ev) {
    switch (ev->type) {
        case SDL_QUIT:
            g_quit = true;
            break;
        case SDL_KEYDOWN:
            if (ev->key.keysym.sym == SDLK_F1) {
                g_pwr_key_pending = true;
            } else if (ev->key.keysym.sym == SDLK_F2) {
                g_pwr_key_long_pending = true;
            } else if (ev->key.keysym.sym == SDLK_F12) {
                g_shot_pending = true;
            } else if (sim_asr_session_active()) {
                if (ev->key.keysym.sym == SDLK_BACKSPACE) sim_asr_backspace();
                if (ev->key.keysym.sym == SDLK_RETURN) sim_asr_end_of_speech();
            }
            break;
        case SDL_TEXTINPUT:
            if (sim_asr_session_active()) sim_asr_type(ev->text.text);
            break;
        default:
            break;
    }
    return 0;
}

void Put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

// 32bpp top-down BMP; pixel data is LVGL XRGB8888 (B,G,R,X little-endian),
// which is exactly BMP's BGRX byte order.
bool WriteBmp32(const char* path, const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const uint32_t row_bytes = w * 4;
    const uint32_t img_bytes = row_bytes * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    Put32(hdr + 2, 54 + img_bytes);
    Put32(hdr + 10, 54);
    Put32(hdr + 14, 40);
    Put32(hdr + 18, w);
    Put32(hdr + 22, static_cast<uint32_t>(-static_cast<int32_t>(h)));
    hdr[26] = 1;
    hdr[28] = 32;
    Put32(hdr + 34, img_bytes);
    bool ok = std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (uint32_t y = 0; ok && y < h; y++) {
        ok = std::fwrite(data + static_cast<size_t>(y) * stride, 1, row_bytes, f) == row_bytes;
    }
    std::fclose(f);
    return ok;
}

const char* ShotPath() {
    const char* p = std::getenv("PI_SIM_SHOT");
    return (p != nullptr && p[0] != '\0') ? p : "pi_sim_shot.bmp";
}

void TakeScreenshot(const char* path) {
    lv_lock();
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
    if (buf != nullptr) {
        if (WriteBmp32(path, buf->data, buf->header.w, buf->header.h, buf->header.stride)) {
            std::fprintf(stderr, "[sim] screenshot -> %s\n", path);
        } else {
            std::fprintf(stderr, "[sim] screenshot write failed: %s\n", path);
        }
        lv_draw_buf_destroy(buf);
    } else {
        std::fprintf(stderr, "[sim] lv_snapshot_take failed\n");
    }
    lv_unlock();
}

// ---- virtual touch: a second pointer indev, driven by PI_SIM_CMDFILE ----
// Equivalent of the GT911: lets tests press/hold/move/release at exact pixels.

struct VirtTouch {
    bool pressed = false;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t release_at = 0;  // 0 = hold until an explicit release
};
VirtTouch g_touch;
std::mutex g_touch_mu;

void VirtTouchRead(lv_indev_t*, lv_indev_data_t* data) {
    static bool last_reported = false;
    static uint32_t reads = 0, last_ms = 0;
    if (std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr) {
        uint32_t now = SDL_GetTicks();
        if (++reads % 16 == 1) {
            std::fprintf(stderr, "[sim][vtouch] read#%u dt=%ums\n", reads, now - last_ms);
        }
        last_ms = now;
    }
    std::lock_guard<std::mutex> lk(g_touch_mu);
    if (g_touch.pressed && g_touch.release_at != 0 && SDL_GetTicks() >= g_touch.release_at) {
        g_touch.pressed = false;
    }
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    data->state = g_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (g_touch.pressed != last_reported) {
        last_reported = g_touch.pressed;
        std::fprintf(stderr, "[sim][vtouch] %s (%d,%d)\n", g_touch.pressed ? "press" : "release",
                     (int)g_touch.x, (int)g_touch.y);
    }
}

// One command per line: key | longkey | type <text> | enter | backspace |
// click <x> <y> | press <x> <y> | move <x> <y> | release | shot <path> | quit
void ExecCmd(const std::string& line) {
    std::fprintf(stderr, "[sim][cmd] %s\n", line.c_str());
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "key") {
        g_pwr_key_pending = true;
    } else if (cmd == "longkey") {
        g_pwr_key_long_pending = true;
    } else if (cmd == "enter") {
        sim_asr_end_of_speech();
    } else if (cmd == "backspace") {
        sim_asr_backspace();
    } else if (cmd == "type") {
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        sim_asr_type(rest.c_str());
    } else if (cmd == "click" || cmd == "press") {
        int x = 0, y = 0;
        ss >> x >> y;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = true;
        g_touch.x = x;
        g_touch.y = y;
        g_touch.release_at = (cmd == "click") ? SDL_GetTicks() + 250 : 0;
    } else if (cmd == "move") {
        int x = 0, y = 0;
        ss >> x >> y;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.x = x;
        g_touch.y = y;
    } else if (cmd == "release") {
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = false;
        g_touch.release_at = 0;
    } else if (cmd == "shot") {
        std::string p;
        ss >> p;
        TakeScreenshot(p.empty() ? ShotPath() : p.c_str());
    } else if (cmd == "quit") {
        g_quit = true;
    }
}

void PollCmdFile(uint32_t now) {
    static const char* cmdfile = std::getenv("PI_SIM_CMDFILE");
    static uint32_t last_poll = 0;
    if (cmdfile == nullptr || cmdfile[0] == '\0' || now - last_poll < 100) return;
    last_poll = now;
    std::ifstream f(cmdfile);
    if (!f.good()) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    f.close();
    ::unlink(cmdfile);
    for (const auto& l : lines) ExecCmd(l);
}

uint32_t EnvMs(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0;
}

// Post-lv_timer_handler pump on the main thread (LVGL lock NOT held here).
void Pump() {
    static const char* demo_text = std::getenv("PI_SIM_AUTODEMO");
    static const uint32_t shot_ms = EnvMs("PI_SIM_SHOT_MS");
    static const uint32_t exit_ms = EnvMs("PI_SIM_EXIT_MS");
    static int demo_phase = 0;
    static bool shot_done = false;
    const uint32_t now = SDL_GetTicks();

    PollCmdFile(now);

    if (demo_text != nullptr && demo_text[0] != '\0') {
        if (demo_phase == 0 && now > 1500) {
            g_pwr_key_pending = true;
            demo_phase = 1;
        } else if (demo_phase == 1 && now > 2600 && sim_asr_session_active()) {
            sim_asr_type(demo_text);
            demo_phase = 2;
        } else if (demo_phase == 2 && now > 3100) {
            sim_asr_end_of_speech();
            demo_phase = 3;
        }
    }

    if (g_pwr_key_pending.exchange(false)) {
        lv_lock();
        IOExpander::getInstance().simTriggerClick(IOExpander::Pin::PWR_KEY);
        lv_unlock();
    }
    if (g_pwr_key_long_pending.exchange(false)) {
        lv_lock();
        IOExpander::getInstance().simTriggerLongPress(IOExpander::Pin::PWR_KEY);
        lv_unlock();
    }
    if (g_shot_pending.exchange(false)) TakeScreenshot(ShotPath());
    if (shot_ms > 0 && !shot_done && now > shot_ms) {
        shot_done = true;
        TakeScreenshot(ShotPath());
    }
    if (exit_ms > 0 && now > exit_ms) g_quit = true;
}

}  // namespace

int main() {
    signal(SIGCHLD, SIG_IGN); /* auto-reap the optional `say` children */

#ifdef __APPLE__
    /* Inherited background QoS (e.g. launched from a script/daemon) stretches a
     * 10ms SDL_Delay to ~95ms via timer coalescing — the whole UI drops to ~10fps
     * and sub-100ms touch gestures get lost. Pin the LVGL thread to interactive. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    std::fprintf(stderr,
                 "pi_sim — Metalio Claw pi_screen simulator (LVGL %d.%d SDL2)\n"
                 "  F1        = PWR_KEY(待机->聆听 / 聆听->取消 / chat->聆听|STOP)\n"
                 "  F2        = PWR_KEY 长按 1.2s(快捷面板呼出/收起)\n"
                 "  打字      = 聆听时说话(停顿1秒自动发送; 回车立即; 退格删字)\n"
                 "  F12       = 截图 BMP\n"
                 "  鼠标      = 触摸\n",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);

    lv_init();
    lv_tick_set_cb(TickCb);

    lv_display_t* disp = lv_sdl_window_create(720, 720);
    lv_sdl_window_set_title(disp, "Metalio Claw — pi_screen sim");
    lv_sdl_mouse_create();
    SDL_AddEventWatch(EventWatch, nullptr);
    SDL_StartTextInput();

    lv_indev_t* vtouch = lv_indev_create();
    lv_indev_set_type(vtouch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(vtouch, VirtTouchRead);

    if (std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr) {
        lv_timer_create(
            [](lv_timer_t*) {
                static uint32_t n = 0, last = 0;
                if (++n % 32 == 1) {
                    uint32_t now = SDL_GetTicks();
                    std::fprintf(stderr, "[sim][t33] fire#%u dt=%ums lv_tick=%u sdl=%u\n", n,
                                 now - last, lv_tick_get(), now);
                    last = now;
                }
            },
            33, nullptr);
    }

    // main/main.cc boot chain, hardware-free part, verbatim semantics.
    lv_lock();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* pi = PiScreen::Create();
    screen_attach_lifecycle(pi, [](screen_lifecycle_event_t e) { PiScreen::LifecycleCallback(e); });
    lv_screen_load(pi);
    if (old_scr != nullptr && old_scr != pi) lv_obj_delete(old_scr);
    lv_unlock();

    const bool loop_debug = std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr;
    uint32_t stat_loops = 0, stat_handler = 0, stat_pump = 0, stat_last = SDL_GetTicks();
    while (!g_quit) {
        uint32_t t0 = SDL_GetTicks();
        uint32_t wait = lv_timer_handler();
        uint32_t t1 = SDL_GetTicks();
        Pump();
        uint32_t t2 = SDL_GetTicks();
        if (loop_debug) {
            stat_loops++;
            stat_handler += t1 - t0;
            stat_pump += t2 - t1;
            if (t2 - stat_last >= 2000) {
                std::fprintf(stderr, "[sim][loop] %.1f loops/s, handler avg %.1fms, pump avg %.1fms\n",
                             stat_loops * 1000.0 / (t2 - stat_last), (double)stat_handler / stat_loops,
                             (double)stat_pump / stat_loops);
                stat_loops = stat_handler = stat_pump = 0;
                stat_last = t2;
            }
        }
        if (wait == LV_NO_TIMER_READY || wait > 10) wait = 10;
        SDL_Delay(wait);
    }
    return 0;
}
