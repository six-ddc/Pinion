// media_ts_demux — 最小 MPEG-TS 音频解封装（HLS 电台分片 → ADTS AAC 字节流）。
// 可移植零依赖（device/sim 同源），供 media_hls.cc 在源内部使用、sim ts_demux_test 单测。
//
// 范围与取向（够用即可，不是通用 demuxer）：
//   - 只认音频：PAT→PMT→遍历全部 ES 条目，锁定 stream_type 0x0F（ADTS AAC）的 PID；
//     全表无 0x0F（LATM 0x11、MP1/MP2 0x03/0x04 等，或一个 ES 都没有）时明确拒绝
//     （found_unsupported() 供上层报错，而不是无声空转）。
//   - 字节流式 Feed：接受任意大小分块，自持 <188B 残包拼接；非 188 对齐/中途起流/垃圾
//     前缀靠 0x47 同步字节扫描重同步（带下一包校验，抗 payload 里的伪 0x47）。
//   - 连续计数断档：丢当前 PES 余量、等下一个 PUSI 再续（ADTS 自带同步字，解码器可从
//     半帧垃圾中重锁定，这里只保证不把断档拼成假连续）。
//   - PSI（PAT/PMT）仅支持单包 section——直播电台节目表极小，跨包 section 不在范围内。
//   - PES payload 原样直通（ADTS 在 TS 里本就自带帧头，无需再造）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

class TsDemux {
 public:
    // 喂入一段 TS 字节，解出的 ADTS 音频字节**追加**到 adts_out（不清空）。
    void Feed(const uint8_t* data, size_t len, std::vector<uint8_t>& adts_out);

    // 复位全部流状态（残包缓冲 + PID 表 + 连续计数）。分片不连续（SEQ 跳变/重解析）时调用。
    void Reset();

    // 已锁定音频 ES（PMT 解析成功且 stream_type 受支持）。
    bool audio_locked() const { return audio_pid_ >= 0; }
    // PMT 全表无 0x0F（不支持的类型如 LATM 0x11，或无 ES）：上层据此报"流不支持"而非静默无声。
    // unsupported_stream_type() 为见过的首个 stream_type；一个 ES 都没有时为 -1。
    bool found_unsupported() const { return unsupported_stream_type_ != 0; }
    int unsupported_stream_type() const { return unsupported_stream_type_; }

 private:
    void ParsePacket(const uint8_t* pkt, std::vector<uint8_t>& adts_out);  // pkt 固定 188B、首字节 0x47
    void ParsePat(const uint8_t* payload, size_t len);
    void ParsePmt(const uint8_t* payload, size_t len);

    std::vector<uint8_t> carry_;         // 跨 Feed 残包（<188B）
    int pmt_pid_ = -1;                   // PAT 解出的首节目 PMT PID
    int audio_pid_ = -1;                 // PMT 解出的音频 ES PID（stream_type 0x0F）
    int unsupported_stream_type_ = 0;    // 首个音频流不受支持时记录其 stream_type
    int audio_cc_ = -1;                  // 音频 PID 连续计数（-1 = 未见过）
    bool drop_until_pusi_ = false;       // 断档后丢弃至下一个 PES 起始
};

}  // namespace media
