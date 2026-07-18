// media_internal — media_controller.cc 与 media_pump.cc 之间的私有契约。
// 定义 pump 的共享上下文（两线程 + 字节环 + 握手）与 pump→controller 的回调宿主接口。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include "media_player/media_player.h"
#include "media_player/media_source.h"

namespace media {

struct Pump;

// pump 线程回调宿主：由 MediaController 实现。回调携带触发的 Pump*，宿主据其 session
// 与当前会话代次比对丢弃陈旧 pump（换曲/停止 teardown 期间在途回调）的事件。
struct PumpHost {
    // 源已打开、开始起播新曲：仅更新曲目索引，**不**代表已在出声（流式 open 是非阻塞
    // 的，此刻字节可能一个都还没读到）。宿主应保持/切回 Loading，真正的 Playing 由
    // 下面的 OnPlaybackFlowing 触发。
    virtual void OnTrackStarted(Pump* p, int index) = 0;
    virtual void OnTrackError(Pump* p, int index, const char* msg) = 0;  // 打开/读取失败 → Error
    virtual void OnAllFinished(Pump* p) = 0;                             // 播完最后一曲 → Stopped
    // 网络流断线重连状态变化：true=开始重连（宿主可把可见态切到 Loading），
    // false=已恢复数据（切回 Playing，仅当本曲已报过 OnPlaybackFlowing）。可能从 pump
    // 的 reader 线程或字节源自己的后台线程（如 curl 的 bg thread）调用，宿主实现须
    // 线程安全、非阻塞。
    virtual void OnReconnecting(Pump* p, bool reconnecting) = 0;
    // 本曲第一个解码帧真正喂给播放管线（而非仅仅"源已打开"）：Loading→Playing 的唯一
    // 触发点，文件/流统一处理。修复：此前 OnTrackStarted 一开源就报 Playing，导致从未
    // 连上的电台 URL 在长达 ~60s 的重连/放弃窗口内一直误报"正在播放"。decoder 线程调用。
    virtual void OnPlaybackFlowing(Pump* p, int index) = 0;
    virtual ~PumpHost() = default;
};

// 字节环容量上限。reader 写满则阻塞至 decoder 腾出空间或 stop。
// 32KB 即够：原 128KB 一开播就吃掉大半内部堆——真机实测 LVGL 每帧图层 lv_malloc 失败
//（esp_cache_msync 刷 NULL、控件画残），切曲时新旧泵短暂并存更是把堆打穿（sdio assert /
// lwip 崩溃重启）。电台 64kbps 下 32KB 仍有 ~4s 网络抖动余量（断线由重连退避兜底）；
// 文件源有 SD 随取随读 + 解码节流，无需深缓冲。
inline constexpr size_t kByteRingCap = 32 * 1024;

// 压缩字节环：固定容量循环缓冲。曾是 std::deque<uint8_t>——deque 按小节点分块分配，
// 在 SPIRAM_MALLOC_ALWAYSINTERNAL=4096 下 32KB 全部落**内部 SRAM**，播放期白吃 32KB
// 内部堆；改成一次性整块 new[]（>4KB 阈值，设备端自动落 PSRAM），容量语义不变（上限
// 检查在调用方，见 media_pump.cc RingWrite），线程安全仍由 Pump::mu 提供。
class ByteRing {
 public:
    ByteRing() : buf_(new uint8_t[kByteRingCap]) {}
    ~ByteRing() { delete[] buf_; }
    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;

    size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }

    // 追加 n 字节。调用方保证 n <= kByteRingCap - size()（RingWrite 持锁计算 chunk）。
    void append(const uint8_t* data, size_t n) {
        size_t tail = (head_ + len_) % kByteRingCap;
        size_t first = n < kByteRingCap - tail ? n : kByteRingCap - tail;
        std::memcpy(buf_ + tail, data, first);
        std::memcpy(buf_, data + first, n - first);
        len_ += n;
    }

    // 从头部取走至多 max 字节到 out，返回实际取走数。
    size_t pop_front(uint8_t* out, size_t max) {
        size_t n = max < len_ ? max : len_;
        size_t first = n < kByteRingCap - head_ ? n : kByteRingCap - head_;
        std::memcpy(out, buf_ + head_, first);
        std::memcpy(out + first, buf_, n - first);
        head_ = (head_ + n) % kByteRingCap;
        len_ -= n;
        return n;
    }

 private:
    uint8_t* buf_;
    size_t head_ = 0;  // 最旧字节下标
    size_t len_ = 0;   // 当前字节数
};

// pump 上下文：controller 创建/持有/销毁，两条线程体在 media_pump.cc。
// reader 线程：按 playlist 从 start_index 起逐曲开源、阻塞读 → 字节环；曲末（EOF）与
//   decoder 握手后推进下一曲（自动连播）；无限流不 EOF。
// decoder 线程：从字节环取压缩字节 → minimp3 榨干 → 降混+重采样 → FeedPlayback。
struct Pump {
    PumpHost* host = nullptr;
    uint32_t session = 0;                 // 会话代次（与 controller 当前代次比对，丢弃陈旧回调）
    std::vector<MediaItem> playlist;      // 起播时的列表快照（pump 自持，解耦 controller 锁）
    int start_index = 0;                  // 起播索引
    uint64_t skip_out_samples = 0;        // 续播/暂停恢复：解码后先丢弃这么多输出样本（文件 seek 近似）
    uint32_t playback_gen = 0;            // 起播时捕获的 FeedPlayback 代次（打断残音竞态收口）

    std::thread reader_thr;
    std::thread decoder_thr;
    std::atomic<bool> stop{false};        // teardown 信号（Stop/换曲/暂停）

#ifdef ESP_PLATFORM
    // 线程退出握手（设备端专用，sim 仍走 std::thread::join）。设备端**绝不能 join 泵
    // 线程**：ESP-IDF 的 pthread_join 用 xTaskNotifyWait 在 notification 槽 0 上等退出
    // 通知，且醒来后不复查线程状态就 vTaskDelete——而 UI 按钮回调跑在 esp_lvgl_adapter
    // 任务上，该任务是 VSync ISR / 跨线程唤醒等 xTaskNotifyGive 的高频目标，join 会在
    // ≤1 个 VSync 内被无关通知**假唤醒**，把还阻塞在 lwip select 里的 reader 整个删掉：
    // 栈立刻释放而栈上的 select_cb 仍挂在 lwip 全局链表上，tcpip 线程随后 UAF（真机
    // "下一台"必现崩溃的根因，MTVAL=0x1/0x2 那两种姿势都是它）。改为：线程创建后立即
    // detach（退出走 esp_pthread 自删除路径，无外部 vTaskDelete），teardown 用本计数
    // 信号量等两条线程真正跑完（PumpSignalExit 是线程触碰 Pump 的最后一条语句）。
    SemaphoreHandle_t exit_sem = nullptr;  // 计数信号量：每条泵线程退出前 Give 一次
#endif
    int threads_started = 0;               // 实际建起的线程数（controller 在 ctrl_mu_ 下读写）
    std::atomic<int> threads_exited{0};    // 已跑完的线程体数（PumpSignalExit 自增；sim 端非阻塞收尸判据）

    ~Pump() {
#ifdef ESP_PLATFORM
        if (exit_sem != nullptr) vSemaphoreDelete(exit_sem);
#endif
    }

    // 字节环 + 跨线程握手（一把锁 + 一个 cv）
    std::mutex mu;
    std::condition_variable cv;
    ByteRing ring;                        // 压缩字节缓冲，容量上限 kByteRingCap
    bool input_done = false;              // reader 已读完当前曲输入
    int reader_epoch = 0;                 // reader 每开一曲 ++（含首曲：0→1）
    int decoder_epoch = 0;                // decoder 已完成解码收尾的 epoch（握手：==reader_epoch 表示当前曲搬完）
    // 当前打开的字节源：reader 线程 Open 成功后写入、Close 前清空，用 mu 保护。teardown
    // 路径（另一线程）借此拿到指针调 Abort()，快速打断可能阻塞在网络 Read() 里的 reader
    // 线程，不必等完整的 socket/重连超时（见 MediaController::Impl::TeardownCurrent）。
    MediaSource* current_source = nullptr;

    // 统计快照（原子，controller/日志读）
    std::atomic<uint64_t> fed_samples{0};        // 当前曲已喂 16k 样本数 → position_s
    std::atomic<uint64_t> decoded_frames{0};     // 累计解码 MP3 帧数（诊断日志）
    std::atomic<uint64_t> resampled_samples{0};  // 累计重采样后样本数（诊断日志）
    std::atomic<size_t> ring_high_water{0};      // 字节环历史高水位（验证流式节流有界）
    std::atomic<int> cur_index{-1};              // 当前正在播的曲索引
};

// 流式节流阀：已缓冲 PCM（PlaybackFilled）超过该秒数就暂停读，防止无限流把队列冲满。
inline constexpr int kStreamBufferMaxSec = 5;

// 泵线程退出信号：设备端 Give 退出信号量——**这是线程触碰 p 的最后一条语句**，Give 之后
// teardown 随时可能 delete p（含 exit_sem 本体；FreeRTOS 的 Give 在队列自旋锁内完成全部
// 队列访问，等待者 Take 返回时 Give 已不再触碰信号量，随后删除安全）。sim 端 no-op（join 兜底）。
inline void PumpSignalExit(Pump* p) {
    p->threads_exited.fetch_add(1, std::memory_order_release);
#ifdef ESP_PLATFORM
    xSemaphoreGive(p->exit_sem);
#endif
}

// 两线程体（media_pump.cc 定义）。
void PumpReaderMain(Pump* p);
void PumpDecoderMain(Pump* p);

}  // namespace media
