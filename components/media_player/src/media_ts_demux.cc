// media_ts_demux — 实现。格式参考 ISO 13818-1（TS 包/PSI/PES 的最小子集，见头文件范围声明）。
#include "media_ts_demux.h"

#include <cstring>

namespace media {
namespace {

constexpr size_t kPkt = 188;
constexpr uint8_t kSync = 0x47;
constexpr int kStreamTypeAdtsAac = 0x0F;

}  // namespace

void TsDemux::Reset() {
    carry_.clear();
    pmt_pid_ = -1;
    audio_pid_ = -1;
    unsupported_stream_type_ = 0;
    audio_cc_ = -1;
    drop_until_pusi_ = false;
}

void TsDemux::Feed(const uint8_t* data, size_t len, std::vector<uint8_t>& adts_out) {
    if (data == nullptr || len == 0) return;
    carry_.insert(carry_.end(), data, data + len);

    size_t off = 0;
    while (carry_.size() - off >= kPkt) {
        // 边界有效性：本位是 0x47 且（可证时）188B 之后仍是 0x47。只查首字节会把
        // "恰好以 0x47 开头的截断残包"整吞 188B，连带吞掉后面真分片的 PAT（实测教训）。
        const bool head_ok = carry_[off] == kSync;
        const bool stride_ok = carry_.size() - off < kPkt + 1 || carry_[off + kPkt] == kSync;
        if (!head_ok || !stride_ok) {
            // 失步：从下一字节起扫描 0x47，同样带跨包步长校验（抗 payload 中的伪同步字节）。
            size_t scan = off + 1;
            while (scan < carry_.size() && carry_[scan] != kSync) scan++;
            if (scan + kPkt < carry_.size() && carry_[scan + kPkt] != kSync) {
                off = scan + 1;  // 伪同步：跳过继续扫
                continue;
            }
            off = scan;
            continue;  // 重新检查剩余量是否够一包
        }
        ParsePacket(carry_.data() + off, adts_out);
        off += kPkt;
    }
    carry_.erase(carry_.begin(), carry_.begin() + off);
}

void TsDemux::ParsePacket(const uint8_t* pkt, std::vector<uint8_t>& adts_out) {
    const bool tei = (pkt[1] & 0x80) != 0;
    if (tei) return;  // 传输错误指示：整包不可信
    const bool pusi = (pkt[1] & 0x40) != 0;
    const int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    const int afc = (pkt[3] >> 4) & 0x3;  // 1=纯 payload 2=纯 adaptation 3=两者
    const int cc = pkt[3] & 0x0F;

    if (afc == 0 || afc == 2) return;  // 无 payload
    size_t p = 4;
    if (afc == 3) {
        const size_t af_len = pkt[4];
        p = 5 + af_len;
        if (p >= kPkt) return;  // adaptation 长度越界：坏包
    }

    if (pid == 0x0000) {  // PAT
        ParsePat(pkt + p, kPkt - p);
        return;
    }
    if (pid == pmt_pid_) {
        ParsePmt(pkt + p, kPkt - p);
        return;
    }
    if (pid != audio_pid_ || audio_pid_ < 0) return;

    // 音频 ES：连续计数校验（重复包丢弃、断档丢至下一 PUSI）。
    if (audio_cc_ >= 0) {
        if (cc == audio_cc_) return;  // 重复包（CC 未推进）
        if (cc != ((audio_cc_ + 1) & 0x0F)) drop_until_pusi_ = true;
    }
    audio_cc_ = cc;

    if (pusi) {
        drop_until_pusi_ = false;
        // PES 头：00 00 01 <stream_id> <len:2> <flags:2> <hdr_len:1> ...
        if (kPkt - p < 9 || pkt[p] != 0x00 || pkt[p + 1] != 0x00 || pkt[p + 2] != 0x01) return;
        const size_t hdr_len = pkt[p + 8];
        p += 9 + hdr_len;
        if (p >= kPkt) return;
    } else if (drop_until_pusi_) {
        return;
    }
    adts_out.insert(adts_out.end(), pkt + p, pkt + kPkt);
}

void TsDemux::ParsePat(const uint8_t* payload, size_t len) {
    if (pmt_pid_ >= 0 || len < 1) return;
    const size_t ptr = payload[0];  // pointer_field（PUSI 包的 section 偏移）
    size_t p = 1 + ptr;
    if (p + 8 > len || payload[p] != 0x00) return;  // table_id 0x00 = PAT
    const size_t section_len = ((payload[p + 1] & 0x0F) << 8) | payload[p + 2];
    const size_t end = p + 3 + section_len;
    if (end > len) return;  // 跨包 section：不支持（见头文件范围声明）
    p += 8;                 // 跳过 table 头至节目循环
    while (p + 4 <= end - 4) {  // 尾部 4B CRC
        const int prog = (payload[p] << 8) | payload[p + 1];
        const int pid = ((payload[p + 2] & 0x1F) << 8) | payload[p + 3];
        if (prog != 0) {  // prog 0 是 NIT
            pmt_pid_ = pid;
            return;
        }
        p += 4;
    }
}

void TsDemux::ParsePmt(const uint8_t* payload, size_t len) {
    if (audio_pid_ >= 0 || unsupported_stream_type_ != 0 || len < 1) return;
    const size_t ptr = payload[0];
    size_t p = 1 + ptr;
    if (p + 12 > len || payload[p] != 0x02) return;  // table_id 0x02 = PMT
    const size_t section_len = ((payload[p + 1] & 0x0F) << 8) | payload[p + 2];
    const size_t end = p + 3 + section_len;
    if (end > len) return;  // 跨包 section：不支持
    const size_t prog_info_len = ((payload[p + 10] & 0x0F) << 8) | payload[p + 11];
    p += 12 + prog_info_len;
    while (p + 5 <= end - 4) {
        const int stream_type = payload[p];
        const int pid = ((payload[p + 1] & 0x1F) << 8) | payload[p + 2];
        const size_t es_info_len = ((payload[p + 3] & 0x0F) << 8) | payload[p + 4];
        // 音频类流：0x0F=ADTS AAC（支持）；0x11=LATM、0x03/0x04=MP1/MP2 等（不支持，记录）。
        if (stream_type == kStreamTypeAdtsAac) {
            audio_pid_ = pid;
            return;
        }
        if (stream_type == 0x11 || stream_type == 0x03 || stream_type == 0x04) {
            unsupported_stream_type_ = stream_type;
            return;
        }
        p += 5 + es_info_len;
    }
}

}  // namespace media
