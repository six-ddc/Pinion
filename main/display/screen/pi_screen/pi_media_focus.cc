#include "pi_media_focus.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "esp_log.h"
#include "media_player/media_player.h"
#include "metalio_hal/audio_pipeline.h"  // IsPlaybackIdle：判 TTS 尾音是否还在播
#include "pi_ui_bridge.h"                // pi_agent_task_is_running

// 实现说明见头文件。去抖用一个"代次"计数器：任何 Suspend 触发点 ++gen；任何"活动
// 结束"触发点捕获当前 gen、睡 kResumeDebounceMs 后若 gen 未变才真正调
// ResumeFromSpeech()——这段等待期内若又发生一次 Suspend（gen 又变了），本次检查
// 直接放弃，交给新一轮自己的 Resume 检查收尾。用后台线程而非定时器：这两个入口都
// 可能从非 LVGL 线程（volc_tts 内部任务 / VoiceTask）调用，MediaController 的方法
// 本身就是全线程安全、非 LVGL 耦合的，不需要编组回 LVGL 线程。
//
// 额外守卫：agent 正在跑一轮（pi_agent_task_is_running()）时放弃本次 Resume（不
// 重排，也不报错）。典型触发：ASR 刚结束、prompt 已发出、agent 正在联网/思考，还
// 没轮到 TTS 说话——这段"思考间隙"若照常在 500ms 后恢复音乐，会出现"续播半秒又被
// TTS 掐断"的可闻抖动。放弃后由后续触发点收尾：本轮真说话了 → TTS on_finished 自己
// 排一次；本轮没说话（纯工具调用/TTS 关闭）→ UI_DONE/UI_ERROR 的 turn_ended 兜底
//（届时 agent 必已跑完，is_running() 为 false，不会被这层守卫再拦一次）。
namespace {

const char* kTag = "media_focus";
constexpr int kResumeDebounceMs = 500;

std::atomic<uint32_t> g_gen{0};

// LLM 回合中排队的起播意图（播放列表索引；-1 = 无）。写入方是 agent worker 线程
//（pi_card_media 的 play 路径），消费方是 ScheduleResume 的检查线程。
std::atomic<int> g_pending_play{-1};

void DoSuspend(const char* why) {
    g_gen.fetch_add(1, std::memory_order_relaxed);
    // 只在真的会起作用（当前确在播放）时打日志，避免 Stopped/Error/已暂停态下的
    // 高频误触发把串口刷屏（多轮 TTS 场景 4 的 no-op 静默要求）。
    bool acted = media::MediaController::Instance().state() == media::MediaState::Playing;
    media::MediaController::Instance().SuspendForSpeech();
    if (acted) ESP_LOGI(kTag, "suspend media (%s)", why);
}

void ScheduleResume(const char* why) {
    uint32_t my_gen = g_gen.load(std::memory_order_relaxed);
    std::thread([my_gen, why] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kResumeDebounceMs));
        if (g_gen.load(std::memory_order_relaxed) != my_gen) {
            return;  // 等待期间又被打断：这次检查作废，新一轮自己会再排一次
        }
        if (pi_agent_task_is_running()) {
            return;  // 思考间隙：放弃，交给 TTS on_finished 或 turn_ended 兜底收尾
        }
        auto& mc = media::MediaController::Instance();
        // TTS 尾音守卫（判据同 pi_screen 息屏门控：队列有声但不是媒体在放 ⇒ 声音是 TTS）：
        // UI_DONE 早于 TTS 播完是常态，turn_ended 的检查若照常放行，起播/续播的 Flush 会把
        // 尾音掐掉。放弃本次即可——排空时 volc 的 on_finished 必再触发一轮 tts_ended。
        if (!mhal::audio_pipeline::IsPlaybackIdle() && mc.state() != media::MediaState::Playing) {
            return;
        }
        int queued = g_pending_play.exchange(-1, std::memory_order_relaxed);
        if (queued != -1) {
            if (queued == PI_MEDIA_QUEUE_NEXT) {
                ESP_LOGI(kTag, "run queued next (%s)", why);
                mc.Next();  // 换曲对"已在播"依然成立，不受下面的用户开播作废规则约束
            } else if (queued == PI_MEDIA_QUEUE_PREV) {
                ESP_LOGI(kTag, "run queued prev (%s)", why);
                mc.Prev();
            } else if (mc.state() == media::MediaState::Playing) {
                // 等待期间用户已手动开播：尊重用户选择，play/resume 意图作废
                ESP_LOGI(kTag, "drop queued play %d, user already playing (%s)", queued, why);
            } else if (queued == PI_MEDIA_QUEUE_RESUME) {
                ESP_LOGI(kTag, "run queued resume (%s)", why);
                mc.Toggle();  // Paused→按记位续播；不依赖 suspended_for_speech_ 配对
            } else {
                ESP_LOGI(kTag, "start queued play %d (%s)", queued, why);
                mc.PlayIndex(queued);
            }
            return;
        }
        bool acted = mc.state() == media::MediaState::Paused;
        mc.ResumeFromSpeech();
        if (acted) ESP_LOGI(kTag, "resume media (%s)", why);
    }).detach();
}

}  // namespace

extern "C" {

void pi_media_focus_tts_audio_start(void) { DoSuspend("tts_audio_start"); }

void pi_media_focus_tts_ended(void) { ScheduleResume("tts_ended"); }

void pi_media_focus_turn_ended(void) { ScheduleResume("turn_ended"); }

void pi_media_focus_asr_start(void) { DoSuspend("asr_start"); }

void pi_media_focus_asr_ended(void) { ScheduleResume("asr_ended"); }

void pi_media_focus_queue_play(int index) {
    if (index == -1 || index < PI_MEDIA_QUEUE_PREV) return;  // 仅 >=0 或已定义哨兵
    g_pending_play.store(index, std::memory_order_relaxed);
    ESP_LOGI(kTag, "queue media action %d until speech ends", index);
}

int pi_media_focus_take_queued_play(void) {
    return g_pending_play.exchange(-1, std::memory_order_relaxed);
}

void pi_media_focus_clear_queued_play(void) {
    int prev = g_pending_play.exchange(-1, std::memory_order_relaxed);
    if (prev != -1) ESP_LOGI(kTag, "queued media action %d cleared", prev);
}

}  // extern "C"
