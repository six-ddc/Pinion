// media_stress — ASan/UBSan 下的 MediaController 切曲压力测试（无头，无 LVGL/SDL）。
//
// 背景：真机上"点下一曲"偶发堆破坏崩溃（heap poisoning 抓到 freed 块内被写入
// FreeRTOS 链表自引用指针，lwip/tcpip 随机姿势死）。radio 与本地文件都能触发，
// 共同路径是 TeardownCurrent/StartPump 的泵切换。本测试在 host 端用 ASan 高频
// 复现该路径：Open → Next/Prev/Toggle/Stop/Suspend/Resume 随机搅拌。UAF/越界
// 一旦发生，ASan 直接给出 use 栈 + free 栈。
//
// 运行（在仓库根目录，素材用 pi_sim_sd/Music）：
//   ./sim/build-asan/media_stress [iters]
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

#include "media_player/media_player.h"
#include "sim_hooks.h"

// audio_pipeline_shim 的采集路径引用（本测试不启采集，仅满足链接）
bool sim_asr_voice_detected(void) { return false; }

int main(int argc, char** argv) {
    int iters = argc > 1 ? std::atoi(argv[1]) : 300;
    unsigned seed = argc > 2 ? (unsigned)std::atoi(argv[2]) : 42;
    std::srand(seed);

    std::vector<media::MediaItem> items;
    for (const char* p : {"pi_sim_sd/Music/sine440.mp3", "pi_sim_sd/Music/dupe.mp3"}) {
        media::MediaItem m;
        m.title = p;
        m.path_or_url = p;
        m.is_stream = false;
        items.push_back(m);
    }

    auto& mc = media::MediaController::Instance();
    mc.StagePlaylist(items, 0);

    for (int i = 0; i < iters; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % 80));
        switch (std::rand() % 10) {
            case 0:
                mc.Toggle();
                break;
            case 1:
                mc.Stop();
                mc.PlayIndex(std::rand() % (int)items.size());
                break;
            case 2:
                mc.SuspendForSpeech();
                std::this_thread::sleep_for(std::chrono::milliseconds(50 + std::rand() % 150));
                mc.ResumeFromSpeech();
                break;
            case 3:
                mc.Prev();
                break;
            default:
                mc.Next();
                break;
        }
        if (i % 20 == 0)
            std::fprintf(stderr, "[stress] iter %d state=%d idx=%d pos=%ds\n", i, (int)mc.state(),
                         mc.index(), mc.position_s());
    }
    mc.Stop();
    std::fprintf(stderr, "[stress] done (%d iters, seed %u)\n", iters, seed);
    return 0;
}

// esp_log shim 的落地实现（不引 esp_shim.c——那个会拖上整个 pi-c host 库）
#include <cstdarg>
extern "C" void sim_log_write(char level, const char* tag, const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "%c %s: ", level, tag);
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}
