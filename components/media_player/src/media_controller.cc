// media_controller — MediaController 单例：状态机 / 播放列表 / pump 线程生命周期 /
// SetOnState 通知 / TTS 让路（Suspend/Resume）语义。
//
// 并发模型：两把锁。
//   mu_    —— 保护状态快照（state/index/playlist/pump_ 指针/位置）。所有快照读接口与
//             pump→controller 回调只碰这把锁，短临界区、绝不在持锁期间 join 线程。
//   ctrl_mu_ —— 串行化控制操作（Play/Stop/Next/…）。teardown 的线程 join 在此锁下、
//             但**不持 mu_** 时进行，规避「持 mu_ join 一个正等 mu_ 的 pump 线程」死锁。
//
// pump 回调用 Pump* 指针身份与当前 pump_ 比对丢弃陈旧事件（换曲/停止 teardown 期间在途）。
#include "media_player/media_player.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "media_internal.h"
#include "media_player/media_http_stream.h"
#include "media_player/media_source.h"
#include "metalio_hal/audio_pipeline.h"

#ifdef ESP_PLATFORM
#include "esp_pthread.h"
#endif

namespace media {

struct MediaController::Impl : public PumpHost {
    std::mutex ctrl_mu_;  // 串行化控制操作 + 线程 join
    std::mutex mu_;       // 保护下列状态快照

    std::vector<MediaItem> playlist_;
    int index_ = -1;
    MediaState state_ = MediaState::Stopped;
    Pump* pump_ = nullptr;         // 当前 pump（owned），nullptr = 无播放线程
    uint32_t session_gen_ = 0;     // 会话代次（每建一个 pump ++）
    int stored_position_s_ = 0;    // 无 pump 时的位置（Paused/Stopped 快照）

    // 暂停/让路续播状态
    bool suspended_for_speech_ = false;  // 仅 SuspendForSpeech 置位，供 ResumeFromSpeech 配对
    int resume_index_ = -1;              // 恢复起播索引
    uint64_t resume_skip_samples_ = 0;   // 恢复时先跳过的输出样本数（文件续播；流为 0=直播边缘）

    std::function<void()> on_state_;

    std::vector<Pump*> zombies_;  // 已自然结束（OnAllFinished/OnTrackError）待回收的 pump

    // —— 生命周期辅助（均要求已持 ctrl_mu_、且调用点不持 mu_，因为要 join 线程）——

    // 回收自然结束的 pump（线程已 stop，join 立即返回）。
    void ReapZombies() {
        std::vector<Pump*> dead;
        {
            std::lock_guard<std::mutex> lk(mu_);
            dead.swap(zombies_);
        }
        for (Pump* p : dead) {
            if (p->reader_thr.joinable()) p->reader_thr.join();
            if (p->decoder_thr.joinable()) p->decoder_thr.join();
            delete p;
        }
    }

    // 干净停掉当前 pump：置 stop、唤醒、Flush 播放队列静音，然后 join+delete（不持 mu_）。
    // 返回停掉的 pump 在停前的已喂样本数（供暂停记位）。
    uint64_t TeardownCurrent() {
        Pump* old = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            old = pump_;
            pump_ = nullptr;
        }
        if (old == nullptr) return 0;
        uint64_t fed = old->fed_samples.load();
        old->stop = true;
        // 若 reader 线程此刻正阻塞在网络流的 Read() 里，跨线程 Abort() 让它在有界延迟内
        // （见 media_http_esp.cc/media_http_curl.cc 的 Abort 实现）返回，而不必等完整的
        // socket 超时/重连退避——这是 Stop/切曲不卡秒级的关键（本模块早期版本的遗留问题）。
        MediaSource* src = nullptr;
        {
            std::lock_guard<std::mutex> lk(old->mu);
            src = old->current_source;
        }
        if (src != nullptr) src->Abort();
        old->cv.notify_all();
        mhal::audio_pipeline::FlushPlayback();  // 解阻 decoder 的 FeedPlayback + 立即静音
        if (old->reader_thr.joinable()) old->reader_thr.join();
        if (old->decoder_thr.joinable()) old->decoder_thr.join();
        delete old;
        return fed;
    }

    // 起一个新 pump 从 start_index 播放（skip_samples 用于续播近似 seek）。要求已持 ctrl_mu_。
    void StartPump(int start_index, uint64_t skip_samples) {
        mhal::audio_pipeline::EnsurePlayback();
        mhal::audio_pipeline::FlushPlayback();  // 干净起播：清残音，并推进代次

        Pump* p = new Pump();
        p->host = this;
        {
            std::lock_guard<std::mutex> lk(mu_);
            p->session = ++session_gen_;
            p->playlist = playlist_;
            p->start_index = start_index;
            p->skip_out_samples = skip_samples;
            p->playback_gen = mhal::audio_pipeline::PlaybackGen();
            pump_ = p;
            index_ = start_index;
            state_ = MediaState::Loading;
        }
#ifdef ESP_PLATFORM
        // ESP-IDF 默认 pthread 栈对 minimp3 偏紧：spawn 前抬到 16KB（仅真机路径）。
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.stack_size = 16 * 1024;
        cfg.prio = 4;  // 对齐播放任务优先级
        esp_pthread_set_cfg(&cfg);
#endif
        p->reader_thr = std::thread(PumpReaderMain, p);
        p->decoder_thr = std::thread(PumpDecoderMain, p);
    }

    void NotifyState() {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = on_state_;
        }
        if (cb) cb();
    }

    // —— PumpHost 回调（pump 线程上下文）——

    void OnTrackStarted(Pump* p, int idx) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;  // 陈旧 pump
            index_ = idx;
            state_ = MediaState::Playing;
            changed = true;
        }
        if (changed) NotifyState();
    }

    void OnTrackError(Pump* p, int idx, const char* msg) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;
            index_ = idx;
            state_ = MediaState::Error;
            stored_position_s_ = 0;
            zombies_.push_back(p);  // 线程即将退出，交给下次控制操作回收
            pump_ = nullptr;
            changed = true;
        }
        if (changed) {
            // SD 卡拔出/文件消失/网络彻底失联等中途读失败都走这里：状态推给 UI（Stage
            // C 的 mini 条/全屏页、Stage B 的 media.state DataHub 路径都订阅了
            // NotifyState），不 crash，用户能看到"出错"而非卡死或静默停播。
            ESP_LOGW("media_ctrl", "track %d error: %s -> Error", idx, msg ? msg : "?");
            NotifyState();
        }
    }

    void OnAllFinished(Pump* p) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;
            state_ = MediaState::Stopped;
            stored_position_s_ = 0;
            zombies_.push_back(p);
            pump_ = nullptr;
            changed = true;
        }
        if (changed) {
            ESP_LOGI("media_ctrl", "playlist finished → Stopped");
            NotifyState();
        }
    }

    // 网络流断线重连：把可见态在 Playing<->Loading 间短暂切换（用户看到"缓冲中"而非
    // 卡死的进度）。只在陈旧 pump 已换代/未在播放态时忽略，避免把 Paused/Stopped 误改。
    void OnReconnecting(Pump* p, bool reconnecting) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;  // 陈旧 pump 的迟到回调
            if (reconnecting && state_ == MediaState::Playing) {
                state_ = MediaState::Loading;
                changed = true;
            } else if (!reconnecting && state_ == MediaState::Loading) {
                state_ = MediaState::Playing;
                changed = true;
            }
        }
        if (changed) {
            ESP_LOGI("media_ctrl", "stream %s", reconnecting ? "reconnecting -> Loading" : "reconnected -> Playing");
            NotifyState();
        }
    }
};

// ============================ 单例 + 构造 ============================

MediaController& MediaController::Instance() {
    static MediaController inst;
    return inst;
}

MediaController::MediaController() : impl_(new Impl()) {}

MediaController::~MediaController() {
    {
        std::lock_guard<std::mutex> lk(impl_->ctrl_mu_);
        impl_->TeardownCurrent();
        impl_->ReapZombies();
    }
    delete impl_;
}

// ============================ 控制接口 ============================

void MediaController::StagePlaylist(std::vector<MediaItem> items, int start_index) {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    const int n = (int)items.size();
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->playlist_ = std::move(items);
        impl_->suspended_for_speech_ = false;
    }
    if (start_index >= 0 && start_index < n) {
        impl_->TeardownCurrent();
        impl_->StartPump(start_index, 0);
    }
    impl_->NotifyState();
}

void MediaController::PlayIndex(int index) {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    int n;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        n = (int)impl_->playlist_.size();
    }
    if (index < 0 || index >= n) return;
    impl_->TeardownCurrent();
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->suspended_for_speech_ = false;
        impl_->stored_position_s_ = 0;
    }
    impl_->StartPump(index, 0);
    impl_->NotifyState();
}

void MediaController::Stop() {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    bool had_pump;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        had_pump = impl_->pump_ != nullptr;
    }
    // 计时：验证 Stop 响应性（尤其流播放时 reader 线程可能阻塞在网络 Read 里）。
    int64_t t0 = had_pump ? esp_timer_get_time() : 0;
    impl_->TeardownCurrent();
    if (had_pump) {
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        ESP_LOGI("media_ctrl", "Stop: teardown took %dms", ms);
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->state_ = MediaState::Stopped;
        impl_->stored_position_s_ = 0;
        impl_->suspended_for_speech_ = false;
    }
    impl_->NotifyState();
}

void MediaController::Toggle() {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();

    MediaState st;
    int idx, n;
    bool is_stream = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        st = impl_->state_;
        idx = impl_->index_;
        n = (int)impl_->playlist_.size();
        if (idx >= 0 && idx < n) is_stream = impl_->playlist_[idx].is_stream;
    }

    if (st == MediaState::Playing) {
        // 暂停：记位（文件按已喂样本，流不记位）。
        uint64_t fed = impl_->TeardownCurrent();
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->resume_index_ = idx;
        impl_->resume_skip_samples_ = is_stream ? 0 : fed;
        impl_->stored_position_s_ = (int)(fed / 16000);
        impl_->state_ = MediaState::Paused;
    } else if (st == MediaState::Paused && impl_->resume_index_ >= 0) {
        int ri;
        uint64_t rskip;
        {
            std::lock_guard<std::mutex> lk(impl_->mu_);
            ri = impl_->resume_index_;
            rskip = impl_->resume_skip_samples_;
            impl_->suspended_for_speech_ = false;
        }
        impl_->StartPump(ri, rskip);
    } else if (st == MediaState::Stopped || st == MediaState::Error) {
        if (idx < 0 || idx >= n) idx = (n > 0) ? 0 : -1;
        if (idx >= 0) {
            std::lock_guard<std::mutex> lk(impl_->mu_);
            impl_->stored_position_s_ = 0;
        }
        if (idx >= 0) impl_->StartPump(idx, 0);
    }
    impl_->NotifyState();
}

void MediaController::Next() {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    int idx, n;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        idx = impl_->index_;
        n = (int)impl_->playlist_.size();
    }
    if (n == 0) return;
    int next = (idx < 0) ? 0 : idx + 1;
    if (next >= n) next = n - 1;  // 钳制在尾
    impl_->TeardownCurrent();
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->suspended_for_speech_ = false;
        impl_->stored_position_s_ = 0;
    }
    impl_->StartPump(next, 0);
    impl_->NotifyState();
}

void MediaController::Prev() {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    int idx, n;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        idx = impl_->index_;
        n = (int)impl_->playlist_.size();
    }
    if (n == 0) return;
    int prev = (idx <= 0) ? 0 : idx - 1;  // 钳制在首
    impl_->TeardownCurrent();
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->suspended_for_speech_ = false;
        impl_->stored_position_s_ = 0;
    }
    impl_->StartPump(prev, 0);
    impl_->NotifyState();
}

void MediaController::SuspendForSpeech() {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    MediaState st;
    int idx, n;
    bool is_stream = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        st = impl_->state_;
        idx = impl_->index_;
        n = (int)impl_->playlist_.size();
        if (idx >= 0 && idx < n) is_stream = impl_->playlist_[idx].is_stream;
    }
    if (st != MediaState::Playing) return;  // 非播放态无操作

    uint64_t fed = impl_->TeardownCurrent();
    {
        // 必须在调 NotifyState() 前释放 mu_：mu_ 是非递归锁，NotifyState() 内部会再次
        // 加锁取 on_state_ 回调，若这里不加花括号收口会自锁死锁（Stage D 接线 ASR/TTS
        // 首次真正调用 SuspendForSpeech 时才暴露的 Stage A 遗留 bug）。
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->suspended_for_speech_ = true;
        impl_->resume_index_ = idx;
        // 文件记位续播；流不记位——Resume 时重连拿最新音频（回直播边缘）。
        impl_->resume_skip_samples_ = is_stream ? 0 : fed;
        impl_->stored_position_s_ = (int)(fed / 16000);
        impl_->state_ = MediaState::Paused;
    }
    impl_->NotifyState();
}

void MediaController::ResumeFromSpeech() {
    std::lock_guard<std::mutex> ctrl(impl_->ctrl_mu_);
    impl_->ReapZombies();
    bool do_resume = false;
    int ri = -1;
    uint64_t rskip = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        if (impl_->suspended_for_speech_ && impl_->resume_index_ >= 0) {
            do_resume = true;
            ri = impl_->resume_index_;
            rskip = impl_->resume_skip_samples_;
            impl_->suspended_for_speech_ = false;
        }
    }
    if (!do_resume) return;  // 未由 Suspend 挂起 → 无操作
    impl_->StartPump(ri, rskip);
    impl_->NotifyState();
}

// ============================ 快照读 ============================

MediaState MediaController::state() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    return impl_->state_;
}

int MediaController::index() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    return impl_->index_;
}

int MediaController::position_s() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (impl_->pump_ != nullptr) return (int)(impl_->pump_->fed_samples.load() / 16000);
    return impl_->stored_position_s_;
}

int MediaController::duration_s() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (impl_->index_ >= 0 && impl_->index_ < (int)impl_->playlist_.size()) {
        return impl_->playlist_[impl_->index_].duration_s;
    }
    return 0;
}

MediaItem MediaController::current() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (impl_->index_ >= 0 && impl_->index_ < (int)impl_->playlist_.size()) {
        return impl_->playlist_[impl_->index_];
    }
    return MediaItem{};
}

int MediaController::playlist_size() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    return (int)impl_->playlist_.size();
}

MediaItem MediaController::item_at(int index) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (index >= 0 && index < (int)impl_->playlist_.size()) return impl_->playlist_[index];
    return MediaItem{};
}

void MediaController::SetOnState(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->on_state_ = std::move(cb);
}

}  // namespace media
