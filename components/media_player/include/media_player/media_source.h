// media_source — 媒体管线的字节源抽象。上层（media_pump 的 reader 线程）只认这个接口，
// 不关心字节来自 SD 卡文件还是网络电台流。实现方保证 Read 是唯一读者线程调用（非并发）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace media {

struct MediaSource {
    // 读取至多 max 字节到 buf。返回：
    //   >0  实际读到的字节数
    //    0  EOF（有界源如文件读完；无限流永远不返回 0，除非被 Close 中断）
    //   <0  读错误 / 已被 Close 中断
    // 阻塞语义：有界源可即时返回；流式源在数据到达前可阻塞，但应保持较短的内部超时，
    // 让上层能及时观察停止信号（reader 每次 Read 之间轮询 pump->stop）。
    virtual int Read(uint8_t* buf, size_t max) = 0;

    // 释放底层资源（关文件 / 断连 + 停内部线程）。Read 之后由同一 reader 线程调用。
    virtual void Close() = 0;

    // 是否无限流（电台）。决定 pump 的节流策略与"曲末→下一曲"语义。
    virtual bool IsStream() const = 0;

    // 跨线程中止：由**非** reader 线程调用（如 MediaController 的 teardown 路径），
    // 让一个正阻塞在 Read() 里的调用尽快（有界延迟）返回 <0，而不必等完整的网络/重连
    // 超时。默认 no-op（文件源的 fread 本就不会长阻塞，不必实现）。必须线程安全、
    // 非阻塞、可重复调用。调用后 Read 的返回值不再重要——调用方即将丢弃本 source。
    virtual void Abort() {}

    virtual ~MediaSource() = default;
};

// 打开 SD 卡 / 本地文件源（POSIX fopen/fread，双端共享实现）。失败返回 nullptr。
// path 为绝对路径（真机 "/sdcard/Music/x.mp3"，sim 直接传相对/绝对路径）。
std::unique_ptr<MediaSource> OpenFileSource(const char* path);

}  // namespace media
