// media_controller — MediaController 单例：状态机 / 播放列表 / pump 线程生命周期 /
// SetOnState 通知 / TTS 让路（Suspend/Resume）语义。
//
// 并发模型：两把锁。
//   mu_    —— 保护状态快照（state/index/playlist/pump_ 指针/位置）。所有快照读接口与
//             pump→controller 回调只碰这把锁，短临界区、绝不在持锁期间等线程退出。
//   ctrl_mu_ —— 串行化控制操作（Play/Stop/Next/…）。teardown 等线程退出（JoinPump）在此
//             锁下、但**不持 mu_** 时进行，规避「持 mu_ 等一个正等 mu_ 的 pump 线程」死锁。
//             设备端等退出用 Pump::exit_sem 而非 pthread_join（假唤醒杀线程，见
//             media_internal.h 注释）。
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
#include "esp_heap_caps.h"  // MALLOC_CAP_SPIRAM：泵线程栈放 PSRAM
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

    // 当前曲目是否已真正出过声（OnPlaybackFlowing 至少触发过一次）。OnTrackStarted
    // （新曲开始）复位；OnReconnecting 的"已恢复"分支据此判断——若本曲从未真正播出过
    // （如一个从未连上过的 URL 在给最终放弃前的某次重试里恰好收到了几个字节又断），
    // 不能因为"字节又来了"就直接报 Playing，仍要等 OnPlaybackFlowing 首帧真正喂出。
    bool track_has_flowed_ = false;

    std::vector<Pump*> zombies_;  // 已自然结束（OnAllFinished/OnTrackError）待回收的 pump

    // —— 生命周期辅助（均要求已持 ctrl_mu_、且调用点不持 mu_，因为要等线程退出）——

    // 等 pump 两条线程真正退出。设备端**不能用 std::thread::join**（esp_pthread 的 join
    // 会被调用任务收到的无关 task notification 假唤醒，进而 vTaskDelete 掉还阻塞在 lwip
    // select 里的线程——"下一台"必现崩溃的根因，详见 media_internal.h Pump::exit_sem
    // 注释），改为等泵线程退出信号量；线程句柄在创建后已 detach。sim 端仍走 join。
    static void JoinPump(Pump* p) {
#ifdef ESP_PLATFORM
        while (p->threads_started > 0) {
            xSemaphoreTake(p->exit_sem, portMAX_DELAY);
            p->threads_started--;
        }
#else
        if (p->reader_thr.joinable()) p->reader_thr.join();
        if (p->decoder_thr.joinable()) p->decoder_thr.join();
#endif
    }

    // JoinPump 的非阻塞版：线程都退了返回 true（可安全 delete），否则 false。
    // teardown 现在是异步的（见 TeardownCurrent），旧泵的 reader 可能还要至多一个
    // socket 超时（~2.5s）才退出——控制操作不能为它卡住 UI，收不动就留到下次再收。
    static bool TryJoinPump(Pump* p) {
#ifdef ESP_PLATFORM
        while (p->threads_started > 0) {
            if (xSemaphoreTake(p->exit_sem, 0) != pdTRUE) return false;
            p->threads_started--;
        }
        return true;
#else
        if (p->threads_exited.load(std::memory_order_acquire) < p->threads_started) return false;
        // 线程体都跑完了，join 只等 pthread 尾声，毫秒级。
        if (p->reader_thr.joinable()) p->reader_thr.join();
        if (p->decoder_thr.joinable()) p->decoder_thr.join();
        return true;
#endif
    }

    // 回收已结束的 pump；还没退干净的留在 zombies_ 里下次再收（非阻塞）。
    void ReapZombies() {
        std::vector<Pump*> dead;
        {
            std::lock_guard<std::mutex> lk(mu_);
            dead.swap(zombies_);
        }
        std::vector<Pump*> keep;
        for (Pump* p : dead) {
            if (TryJoinPump(p)) {
                delete p;
            } else {
                keep.push_back(p);
            }
        }
        if (!keep.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            zombies_.insert(zombies_.end(), keep.begin(), keep.end());
        }
    }

    // 阻塞收掉最老的一个 zombie：仅在 StartPump 发现 zombies_ 堆积（弱网连续切台，旧
    // reader 最长要 ~2.5s 才观察到 Abort，见 TeardownCurrent 注释）到 ≥2 个时兜底调用，
    // 防止泵线程栈（内部 SRAM，2×16KB/泵）无界堆积打穿堆。等待有界：上界即上述 ~2.5s。
    // 要求已持 ctrl_mu_、且调用点不持 mu_（先在 mu_ 下摘出指针再放锁 join，避免持锁等
    // 线程退出）。
    void ReapOldestZombieBlocking() {
        Pump* oldest = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (zombies_.empty()) return;
            oldest = zombies_.front();
            zombies_.erase(zombies_.begin());
        }
        JoinPump(oldest);
        delete oldest;
    }

    // 析构专用：阻塞等所有 zombie 线程退出后释放（进程退出路径，不在 UI 线程上）。
    void DrainZombiesBlocking() {
        std::vector<Pump*> dead;
        {
            std::lock_guard<std::mutex> lk(mu_);
            dead.swap(zombies_);
        }
        for (Pump* p : dead) {
            JoinPump(p);
            delete p;
        }
    }

    // 停掉当前 pump：置 stop、Abort 字节源、唤醒、Flush 播放队列静音，旧泵挂 zombies_
    // 异步回收（不在本线程等它退出，见函数尾注释）。返回停前的已喂样本数（供暂停记位）。
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
        // Abort() 必须在持 old->mu 时调：reader 侧"清 current_source → Close → 析构 src"
        // 的第一步也在同一把锁下，出锁即拿不到指针，堵死"放锁后 Abort 撞上已析构 src"的
        // 微秒级 UAF 窗口（Abort 只置原子标志，不阻塞，持锁无害）。
        {
            std::lock_guard<std::mutex> lk(old->mu);
            if (old->current_source != nullptr) old->current_source->Abort();
        }
        old->cv.notify_all();
        mhal::audio_pipeline::FlushPlayback();  // 解阻 decoder 的 FeedPlayback + 立即静音
        // 异步收尸：不在调用线程（通常是 LVGL 任务）上等泵线程退出——reader 若正阻塞在
        // 网络 Read 里，最长要一个 socket 超时（~2.5s）才观察到 Abort，同步等会把 UI 冻住
        // （真机切台卡顿的主因之一）。旧泵已收到 stop/Abort 自行退出，挂 zombies_ 由下次
        // 控制操作 ReapZombies 非阻塞补收。期间的陈旧回调由 p != pump_ 判据丢弃，残余
        // FeedPlayback 由播放代次（playback_gen，StartPump 的 Flush 已推进）丢弃。
        {
            std::lock_guard<std::mutex> lk(mu_);
            zombies_.push_back(old);
        }
        return fed;
    }

    // 起一个新 pump 从 start_index 播放（skip_samples 用于续播近似 seek）。要求已持 ctrl_mu_。
    void StartPump(int start_index, uint64_t skip_samples) {
        // 建新泵前先非阻塞收一轮（TeardownCurrent 刚把旧泵挂进 zombies_）；弱网下连按
        // 切台 4-5 次会让 zombies_ 迅速堆到 3-4 个，每个泵占 2×16KB 内部 SRAM 栈——
        // 堆到 ≥2 就同步等最老的一个退出再继续，把堆积上限钳在 1-2 个泵。
        ReapZombies();
        size_t zombie_count;
        {
            std::lock_guard<std::mutex> lk(mu_);
            zombie_count = zombies_.size();
        }
        if (zombie_count >= 2) {
            ReapOldestZombieBlocking();
        }
        mhal::audio_pipeline::EnsurePlayback();
        mhal::audio_pipeline::FlushPlayback();  // 干净起播：清残音，并推进代次

        Pump* p = new Pump();
        p->host = this;
#ifdef ESP_PLATFORM
        // 线程退出握手信号量必须先于线程存在（见 media_internal.h Pump::exit_sem 注释）。
        p->exit_sem = xSemaphoreCreateCounting(2, 0);
        if (p->exit_sem == nullptr) {
            ESP_LOGE("media_ctrl", "pump exit_sem alloc failed");
            {
                std::lock_guard<std::mutex> lk(mu_);
                state_ = MediaState::Error;
            }
            delete p;
            NotifyState();
            return;
        }
#endif
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
            // 在此处（而非只靠 OnTrackStarted）就复位：新 pump 的字节源可能在
            // OnTrackStarted 被调用前就已经历一次失败+恢复（尤其 curl 侧后台线程一
            // 建好就跑），若不在这里先清掉上一个 pump 遗留的 true，OnReconnecting 的
            // "已恢复"分支可能在本曲从未真正出声前就误判为可以报 Playing。
            track_has_flowed_ = false;
        }
#ifdef ESP_PLATFORM
        // esp_pthread_set_cfg 改的是"调用任务今后创建的**所有** pthread"的默认配置，
        // 用完必须还原，否则 UI/agent 任务后续任何 std::thread 都会静默继承 16KB/PSRAM/
        // prio1（低优先级 + 小栈，对 TLS 等深栈线程是隐雷）。
        esp_pthread_cfg_t prev_cfg;
        const bool had_prev_cfg = esp_pthread_get_cfg(&prev_cfg) == ESP_OK;
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        // prio 4 不钉核：音频生产与消费同级（audio_playback/tts_audio 也是 4）。
        // 演进史：P2 时代渲染任务（swdraw P3、LVGL P5）压得住解码，真机实测放音乐
        // 时 dec 探针从正常 ~890ms/5s 恶化到 4900-6100ms/5s、产出 <1x 实时，播放环
        // 持续欠载（swdraw 忙时段完全对齐）；提到 P4 后渲染永远压不住解码，代价只是
        // 解码突发窗口（起播/切曲预蓄水位，1-2s）UI 帧率略降。突发有界：3s 水位节流
        // + 每帧 2ms 让步 + FeedPlayback 背压。core 1 上 P4 < LVGL(P5)，滚动不受影响
        // ——"泵抢渲染帧"的老病（P2>LVGL P1 时代）不会回来。线程命名 media_rd/
        // media_dec：esp_pthread 默认名都叫 "pthread"，sysmon 任务榜分不清谁是谁。
        //
        // 栈放**内部 SRAM**：曾放 PSRAM（当时动机是 minimp3 28KB 内部栈教训 + 内部占用
        // 归零），但 esp_audio_codec 的 AAC 解码栈上工作集大，栈走 PSRAM 实测把解码拖到
        // 近实时边缘（dec 占墙钟 ~90%），是卡顿另一半根因。16KB×2=32KB 内部堆可承受
        //（解码器大状态在库内堆分配，不吃栈）。约束不变：这两线程不得做需要关 cache 的
        // flash 操作（SD 走 SDMMC、网络走 esp-hosted，均不涉及）。
        cfg.prio = 4;
        cfg.stack_size = 16 * 1024;
        cfg.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        cfg.pin_to_core = -1;  // esp_pthread 惯用值：越界回落 PTHREAD_TASK_CORE_DEFAULT(-1)=无亲和
        cfg.thread_name = "media_rd";
        esp_pthread_set_cfg(&cfg);
#endif
        // 建线程可能因低内存失败抛 std::system_error：不接就是 std::terminate 整机
        // 重启。失败时优雅收场——已建的 reader 停掉等退出，撤销 pump 登记，报 Error。
        // 设备端线程创建成功即 detach（退出走 esp_pthread 自删除路径，永不被外部
        // vTaskDelete），生死改由 exit_sem 握手（见 JoinPump）。
        try {
            p->reader_thr = std::thread(PumpReaderMain, p);
#ifdef ESP_PLATFORM
            p->reader_thr.detach();
#endif
            p->threads_started = 1;
#ifdef ESP_PLATFORM
            cfg.thread_name = "media_dec";
            esp_pthread_set_cfg(&cfg);
#endif
            p->decoder_thr = std::thread(PumpDecoderMain, p);
#ifdef ESP_PLATFORM
            p->decoder_thr.detach();
#endif
            p->threads_started = 2;
        } catch (const std::exception& e) {
            ESP_LOGE("media_ctrl", "pump thread spawn failed: %s", e.what());
            p->stop = true;
            {
                std::lock_guard<std::mutex> lk(p->mu);
                if (p->current_source != nullptr) p->current_source->Abort();
            }
            p->cv.notify_all();
            JoinPump(p);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (pump_ == p) pump_ = nullptr;
                state_ = MediaState::Error;
            }
            delete p;
            NotifyState();
        }
#ifdef ESP_PLATFORM
        // 还原调用任务原有的 pthread 默认配置（无先前配置则还原为系统默认）。
        if (had_prev_cfg) {
            esp_pthread_set_cfg(&prev_cfg);
        } else {
            esp_pthread_cfg_t def = esp_pthread_get_default_config();
            esp_pthread_set_cfg(&def);
        }
#endif
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
        // 只更新曲目索引 + 保持/切回 Loading——源已打开不代表已在出声（流式 open 非
        // 阻塞，此刻字节可能一个都还没读到）。真正的 Playing 由 OnPlaybackFlowing 触发
        // （见下）。多曲连播时上一曲可能是 Playing，这里显式切回 Loading 让 UI 如实
        // 反映"新曲正在缓冲"。
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;  // 陈旧 pump
            index_ = idx;
            if (state_ != MediaState::Loading) {
                state_ = MediaState::Loading;
            }
            track_has_flowed_ = false;  // 新曲：Playing 需要重新用首帧触发（含 OnReconnecting 的恢复分支）
            changed = true;  // 索引变化本身也值得通知 UI（标题/曲目切换）
        }
        if (changed) NotifyState();
    }

    // 本曲第一帧真正喂给播放管线：Loading→Playing 的唯一触发点（文件/流统一）。
    void OnPlaybackFlowing(Pump* p, int idx) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;  // 陈旧 pump
            index_ = idx;
            track_has_flowed_ = true;
            if (state_ == MediaState::Loading) {
                state_ = MediaState::Playing;
                changed = true;
            }
        }
        if (changed) {
            ESP_LOGI("media_ctrl", "track %d first frame flowing -> Playing", idx);
            NotifyState();
        }
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
    //
    // "已恢复"分支的 track_has_flowed_ 守卫：字节源收到字节（TCP 连上、开始收数据）
    // 不等于"minimp3 已经解出并喂出至少一帧"——两者之间还隔着解码窗保留水位/首帧解码
    // 延迟。若本曲此前从未真正出过声就直接在这里报 Playing，会重现与 OnTrackStarted
    // 同类的"过早报 Playing"问题（实测：从未连上的 URL 在某次重试里若恰好收到几个字节
    // 又立刻断开，没有这个守卫会在此处误报一次 Playing）。只有本曲已经真正流过至少一帧
    // （track_has_flowed_）才允许"重连恢复"直接回到 Playing；否则耐心等 OnPlaybackFlowing。
    void OnReconnecting(Pump* p, bool reconnecting) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p != pump_) return;  // 陈旧 pump 的迟到回调
            if (reconnecting && state_ == MediaState::Playing) {
                state_ = MediaState::Loading;
                changed = true;
            } else if (!reconnecting && state_ == MediaState::Loading && track_has_flowed_) {
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
        impl_->TeardownCurrent();          // 异步：旧泵进 zombies_
        impl_->DrainZombiesBlocking();     // 析构必须等干净（不在 UI 线程上跑）
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
