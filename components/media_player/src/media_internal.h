// media_internal — media_controller.cc 与 media_pump.cc 之间的私有契约。
// 定义 pump 的共享上下文（两线程 + 字节环 + 握手）与 pump→controller 的回调宿主接口。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "media_player/media_player.h"
#include "media_player/media_source.h"

namespace media {

struct Pump;

// pump 线程回调宿主：由 MediaController 实现。回调携带触发的 Pump*，宿主据其 session
// 与当前会话代次比对丢弃陈旧 pump（换曲/停止 teardown 期间在途回调）的事件。
struct PumpHost {
    virtual void OnTrackStarted(Pump* p, int index) = 0;                 // Loading→Playing，位置已在 pump 内重置
    virtual void OnTrackError(Pump* p, int index, const char* msg) = 0;  // 打开/读取失败 → Error
    virtual void OnAllFinished(Pump* p) = 0;                             // 播完最后一曲 → Stopped
    // 网络流断线重连状态变化：true=开始重连（宿主可把可见态切到 Loading），
    // false=已恢复数据（切回 Playing）。可能从 pump 的 reader 线程或字节源自己的后台
    // 线程（如 curl 的 bg thread）调用，宿主实现须线程安全、非阻塞。
    virtual void OnReconnecting(Pump* p, bool reconnecting) = 0;
    virtual ~PumpHost() = default;
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

    // 字节环 + 跨线程握手（一把锁 + 一个 cv）
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint8_t> ring;             // 压缩字节缓冲，容量上限 kByteRingCap
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

// 字节环容量上限（~128KB）。reader 写满则阻塞至 decoder 腾出空间或 stop。
inline constexpr size_t kByteRingCap = 128 * 1024;
// 流式节流阀：已缓冲 PCM（PlaybackFilled）超过该秒数就暂停读，防止无限流把队列冲满。
inline constexpr int kStreamBufferMaxSec = 5;

// 两线程体（media_pump.cc 定义）。
void PumpReaderMain(Pump* p);
void PumpDecoderMain(Pump* p);

}  // namespace media
