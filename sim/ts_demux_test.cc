// ts_demux_test — media_ts_demux 宿主单测（id3_test 范式：真实 fixture + 合成字节向量 + 变异 fuzz）。
// fixture: sim/testdata/cnr_seg.ts（CNR 中国之声 HLS 直播分片实录，MPEG-TS + ADTS AAC-LC 48k 立体声）。
// 构建带 ASan/UBSan（见 sim/CMakeLists.txt），fuzz 的"不崩"断言由 sanitizer 兜底。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "media_hls.h"
#include "media_ts_demux.h"

// shim esp_log.h 的落点（正经实现在 esp_shim.c，但它牵 pi-c 运行时，测试不值当）：直印 stderr。
extern "C" void sim_log_write(char level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "%c %s: ", level, tag);
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            g_failures++;                                    \
            std::fprintf(stderr, "[FAIL] " __VA_ARGS__);     \
            std::fprintf(stderr, "  (%s:%d)\n", __FILE__, __LINE__); \
        }                                                    \
    } while (0)

std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 校验 ADTS 帧长链：从头逐帧走 syncword(FFF) + frame_length，返回完整帧数；
// consumed_out 为链走到的字节数（尾部允许一个不完整帧：直播分片可能切在帧中）。
int WalkAdtsChain(const std::vector<uint8_t>& adts, size_t* consumed_out) {
    size_t p = 0;
    int frames = 0;
    while (p + 7 <= adts.size()) {
        if (adts[p] != 0xFF || (adts[p + 1] & 0xF0) != 0xF0) break;  // 失步即止
        const size_t frame_len = ((adts[p + 3] & 0x03) << 11) | (adts[p + 4] << 3) | (adts[p + 5] >> 5);
        if (frame_len < 7) break;
        if (p + frame_len > adts.size()) break;  // 尾部不完整帧
        p += frame_len;
        frames++;
    }
    if (consumed_out != nullptr) *consumed_out = p;
    return frames;
}

// —— 合成最小 TS：PAT + PMT + 单 PES（已知 ADTS 载荷）——覆盖解析路径的白盒断言 ——

void PutPacketHeader(std::vector<uint8_t>& out, int pid, bool pusi, int cc) {
    out.push_back(0x47);
    out.push_back((uint8_t)(((pusi ? 1 : 0) << 6) | ((pid >> 8) & 0x1F)));
    out.push_back((uint8_t)(pid & 0xFF));
    out.push_back((uint8_t)(0x10 | (cc & 0x0F)));  // afc=1 纯 payload
}

void PadTo188(std::vector<uint8_t>& out, size_t pkt_start) {
    while (out.size() - pkt_start < 188) out.push_back(0xFF);
}

std::vector<uint8_t> BuildPat(int pmt_pid) {
    std::vector<uint8_t> out;
    PutPacketHeader(out, 0x0000, true, 0);
    out.push_back(0x00);  // pointer_field
    // PAT section: table_id len=13 (5 hdr + 4 prog + 4 crc)
    const uint8_t sec[] = {0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00,
                           0x00, 0x01,  // program_number 1
                           (uint8_t)(0xE0 | ((pmt_pid >> 8) & 0x1F)), (uint8_t)(pmt_pid & 0xFF),
                           0x00, 0x00, 0x00, 0x00};  // CRC 占位（demux 不校验）
    out.insert(out.end(), sec, sec + sizeof(sec));
    PadTo188(out, 0);
    return out;
}

std::vector<uint8_t> BuildPmt(int pmt_pid, int audio_pid, uint8_t stream_type) {
    std::vector<uint8_t> out;
    PutPacketHeader(out, pmt_pid, true, 0);
    out.push_back(0x00);  // pointer_field
    // PMT section: 9B 固定头(至 program_info_length) + 5B 流条目 + 4B CRC → section_length=18
    const uint8_t sec[] = {0x02, 0xB0, 0x12, 0x00, 0x01, 0xC1, 0x00, 0x00,
                           (uint8_t)(0xE0 | ((audio_pid >> 8) & 0x1F)), (uint8_t)(audio_pid & 0xFF),  // PCR PID
                           0xF0, 0x00,  // program_info_length 0
                           stream_type, (uint8_t)(0xE0 | ((audio_pid >> 8) & 0x1F)), (uint8_t)(audio_pid & 0xFF),
                           0xF0, 0x00,  // ES_info_length 0
                           0x00, 0x00, 0x00, 0x00};
    out.insert(out.end(), sec, sec + sizeof(sec));
    PadTo188(out, 0);
    return out;
}

// 单包 PES：9B PES 头（hdr_len=0）+ payload 直到包尾填满（payload 不足则 0xFF 垫在 payload 前
// 不行——demux 直通 payload，垫会混入；因此这里让 payload 恰好填满包）。
std::vector<uint8_t> BuildAudioPes(int audio_pid, int cc, bool pusi, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    PutPacketHeader(out, audio_pid, pusi, cc);
    if (pusi) {
        const uint8_t pes_hdr[] = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00};
        out.insert(out.end(), pes_hdr, pes_hdr + sizeof(pes_hdr));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    if (out.size() != 188) {
        std::fprintf(stderr, "[test-bug] PES packet size %zu != 188\n", out.size());
        std::abort();
    }
    return out;
}

// 造一段恰好填满 n 个 TS 包 payload 的伪 ADTS 数据（本测试只验直通字节，不验可解码性）。
std::vector<uint8_t> FakePayload(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(seed + i * 7);
    return v;
}

void TestSynthetic() {
    const int kPmtPid = 0x100, kAudioPid = 0x101;
    // 首包 PES payload = 188-4-9 = 175B；后续包 = 184B
    auto pay1 = FakePayload(175, 0x11);
    auto pay2 = FakePayload(184, 0x22);
    media::TsDemux d;
    std::vector<uint8_t> out;
    auto pat = BuildPat(kPmtPid);
    auto pmt = BuildPmt(kPmtPid, kAudioPid, 0x0F);
    d.Feed(pat.data(), pat.size(), out);
    CHECK(!d.audio_locked(), "PAT 之后不应已锁定音频\n");
    d.Feed(pmt.data(), pmt.size(), out);
    CHECK(d.audio_locked(), "PMT(0x0F) 之后应锁定音频 PID\n");
    auto pes1 = BuildAudioPes(kAudioPid, 0, true, pay1);
    auto pes2 = BuildAudioPes(kAudioPid, 1, false, pay2);
    d.Feed(pes1.data(), pes1.size(), out);
    d.Feed(pes2.data(), pes2.size(), out);
    std::vector<uint8_t> want = pay1;
    want.insert(want.end(), pay2.begin(), pay2.end());
    CHECK(out == want, "PES payload 直通字节应逐字节一致 (got %zu want %zu)\n", out.size(), want.size());

    // CC 重复包应被丢弃；CC 断档应丢到下一个 PUSI
    std::vector<uint8_t> out2;
    d.Feed(pes2.data(), pes2.size(), out2);  // cc=1 重复
    CHECK(out2.empty(), "CC 重复包应被丢弃\n");
    auto pes_gap = BuildAudioPes(kAudioPid, 5, false, pay2);  // cc 跳 1→5
    d.Feed(pes_gap.data(), pes_gap.size(), out2);
    CHECK(out2.empty(), "CC 断档后非 PUSI 包应被丢弃\n");
    auto pes_resume = BuildAudioPes(kAudioPid, 6, true, pay1);
    d.Feed(pes_resume.data(), pes_resume.size(), out2);
    CHECK(out2 == pay1, "断档后 PUSI 包应恢复直通\n");

    // LATM(0x11)：应报不支持而非静默
    media::TsDemux d2;
    std::vector<uint8_t> out3;
    auto pmt_latm = BuildPmt(kPmtPid, kAudioPid, 0x11);
    d2.Feed(pat.data(), pat.size(), out3);
    d2.Feed(pmt_latm.data(), pmt_latm.size(), out3);
    CHECK(!d2.audio_locked() && d2.found_unsupported() && d2.unsupported_stream_type() == 0x11,
          "LATM 应记录为不支持类型\n");
}

// —— 多 ES PMT / 损坏 PES 的定向白盒断言（覆盖 ES 遍历、drop-till-PUSI、重同步尾包扣留）——

// 任意条数 ES 的 PMT（每条 ES_info_length=0）。section_length 随 ES 数计算。
std::vector<uint8_t> BuildPmtMulti(int pmt_pid, int pcr_pid,
                                   const std::vector<std::pair<uint8_t, int>>& es) {
    std::vector<uint8_t> out;
    PutPacketHeader(out, pmt_pid, true, 0);
    out.push_back(0x00);  // pointer_field
    const int section_len = 9 + 5 * (int)es.size() + 4;  // 固定头 9 + ES*5 + CRC 4
    std::vector<uint8_t> sec = {0x02, (uint8_t)(0xB0 | ((section_len >> 8) & 0x0F)),
                                (uint8_t)(section_len & 0xFF), 0x00, 0x01, 0xC1, 0x00, 0x00,
                                (uint8_t)(0xE0 | ((pcr_pid >> 8) & 0x1F)), (uint8_t)(pcr_pid & 0xFF),
                                0xF0, 0x00};  // program_info_length 0
    for (const auto& e : es) {
        sec.push_back(e.first);
        sec.push_back((uint8_t)(0xE0 | ((e.second >> 8) & 0x1F)));
        sec.push_back((uint8_t)(e.second & 0xFF));
        sec.push_back(0xF0);
        sec.push_back(0x00);  // ES_info_length 0
    }
    sec.insert(sec.end(), {0x00, 0x00, 0x00, 0x00});  // CRC 占位
    out.insert(out.end(), sec.begin(), sec.end());
    PadTo188(out, 0);
    return out;
}

// PUSI 包，PES 头合法但 PES_header_data_length 巨大（跨出本包）→ demux 应 drop 至下一 PUSI。
std::vector<uint8_t> BuildPesBadHdrLen(int pid, int cc, uint8_t hdr_len) {
    std::vector<uint8_t> out;
    PutPacketHeader(out, pid, true, cc);
    const uint8_t pes_hdr[] = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0x00, hdr_len};
    out.insert(out.end(), pes_hdr, pes_hdr + sizeof(pes_hdr));
    PadTo188(out, 0);
    return out;
}

// PUSI 包，PES 起始码损坏（非 00 00 01）→ demux 应 drop 至下一 PUSI。
std::vector<uint8_t> BuildPesBadStart(int pid, int cc) {
    std::vector<uint8_t> out;
    PutPacketHeader(out, pid, true, cc);
    const uint8_t bad[] = {0xDE, 0xAD, 0xBE, 0xC0, 0x00, 0x00, 0x80, 0x00, 0x00};
    out.insert(out.end(), bad, bad + sizeof(bad));
    PadTo188(out, 0);
    return out;
}

void TestSyntheticExtra() {
    const int kPmtPid = 0x100, kAudioPid = 0x101;
    auto pat = BuildPat(kPmtPid);

    // (a) PMT 只含 0x1B(H.264)+0x81(AC-3) → 不锁定且 unsupported 置位（记首个 0x1B）
    {
        media::TsDemux d;
        std::vector<uint8_t> out;
        auto pmt = BuildPmtMulti(kPmtPid, kAudioPid, {{0x1B, 0x101}, {0x81, 0x102}});
        d.Feed(pat.data(), pat.size(), out);
        d.Feed(pmt.data(), pmt.size(), out);
        CHECK(!d.audio_locked() && d.found_unsupported() && d.unsupported_stream_type() == 0x1B,
              "(a) 无 0x0F 的 PMT 应记不支持且记首个类型 0x1B (got locked=%d type=0x%02X)\n",
              d.audio_locked(), d.unsupported_stream_type());
    }

    // (b) PMT ES 顺序 0x03 在前 0x0F 在后 → 锁定 0x0F 的 PID 并出帧
    {
        media::TsDemux d;
        std::vector<uint8_t> out;
        auto pmt = BuildPmtMulti(kPmtPid, kAudioPid, {{0x03, 0x105}, {0x0F, kAudioPid}});
        d.Feed(pat.data(), pat.size(), out);
        d.Feed(pmt.data(), pmt.size(), out);
        CHECK(d.audio_locked() && !d.found_unsupported(), "(b) 全表扫到 0x0F 应锁定不置不支持\n");
        auto pay = FakePayload(175, 0x33);
        auto pes = BuildAudioPes(kAudioPid, 0, true, pay);
        d.Feed(pes.data(), pes.size(), out);
        CHECK(out == pay, "(b) 锁定 0x0F 的 PID 后应直通其 payload (got %zu want %zu)\n", out.size(),
              pay.size());
    }

    // (c) PMT 无任何 ES → unsupported（类型记 -1）
    {
        media::TsDemux d;
        std::vector<uint8_t> out;
        auto pmt = BuildPmtMulti(kPmtPid, kAudioPid, {});
        d.Feed(pat.data(), pat.size(), out);
        d.Feed(pmt.data(), pmt.size(), out);
        CHECK(!d.audio_locked() && d.found_unsupported() && d.unsupported_stream_type() == -1,
              "(c) 无 ES 的 PMT 应置不支持（类型 -1）(got locked=%d type=%d)\n", d.audio_locked(),
              d.unsupported_stream_type());
    }

    auto pmt_ok = BuildPmt(kPmtPid, kAudioPid, 0x0F);

    // (d) PES_header_data_length 跨出首包 → 首包与后续 continuation 包均零输出
    {
        media::TsDemux d;
        std::vector<uint8_t> out;
        d.Feed(pat.data(), pat.size(), out);
        d.Feed(pmt_ok.data(), pmt_ok.size(), out);
        auto pes_bad = BuildPesBadHdrLen(kAudioPid, 0, 200);  // 200 > 184-9，跨包
        auto cont = BuildAudioPes(kAudioPid, 1, false, FakePayload(184, 0x44));
        d.Feed(pes_bad.data(), pes_bad.size(), out);
        d.Feed(cont.data(), cont.size(), out);
        CHECK(out.empty(), "(d) 超长 PES 头应 drop 首包及后续 continuation (got %zu)\n", out.size());
    }

    // (e) PUSI 包起始码损坏 → 该 PES 后续包零输出
    {
        media::TsDemux d;
        std::vector<uint8_t> out;
        d.Feed(pat.data(), pat.size(), out);
        d.Feed(pmt_ok.data(), pmt_ok.size(), out);
        auto pes_bad = BuildPesBadStart(kAudioPid, 0);
        auto cont = BuildAudioPes(kAudioPid, 1, false, FakePayload(184, 0x55));
        d.Feed(pes_bad.data(), pes_bad.size(), out);
        d.Feed(cont.data(), cont.size(), out);
        CHECK(out.empty(), "(e) 起始码损坏应 drop 至下一 PUSI (got %zu)\n", out.size());
    }

    // (f) 垃圾前缀 + 恰 188B 伪包（首字节 0x47）单次 Feed → 零输出（重同步后不可验证尾巴被扣住）
    {
        media::TsDemux d;
        std::vector<uint8_t> out;
        d.Feed(pat.data(), pat.size(), out);
        d.Feed(pmt_ok.data(), pmt_ok.size(), out);
        out.clear();
        std::vector<uint8_t> buf(50, 0x00);  // 垃圾前缀（无 0x47）
        buf.push_back(0x47);                  // 伪包首字节
        auto tail = FakePayload(187, 0x66);   // 补足 188B，内容非 0x47
        buf.insert(buf.end(), tail.begin(), tail.end());
        d.Feed(buf.data(), buf.size(), out);
        CHECK(out.empty(), "(f) 重同步后的 188B 伪尾包应被 carry 扣住不输出 (got %zu)\n", out.size());
    }
}

void TestFixture(const std::vector<uint8_t>& ts) {
    // 一次性喂入
    media::TsDemux d;
    std::vector<uint8_t> whole;
    d.Feed(ts.data(), ts.size(), whole);
    CHECK(d.audio_locked(), "fixture 应锁定音频 ES\n");
    CHECK(whole.size() > 100000, "10s 194kbps 分片应产出 >100KB ADTS (got %zu)\n", whole.size());
    CHECK(whole.size() >= 2 && whole[0] == 0xFF && (whole[1] & 0xF0) == 0xF0, "输出应以 ADTS 同步字开头\n");
    size_t consumed = 0;
    int frames = WalkAdtsChain(whole, &consumed);
    CHECK(frames > 400, "10s@48k 应有 >400 个 AAC 帧 (got %d)\n", frames);
    CHECK(whole.size() - consumed < 4096, "帧长链应覆盖到接近末尾 (leftover %zu)\n", whole.size() - consumed);

    // 随机小分块喂入 → 与一次性完全一致
    std::mt19937 rng(42);
    media::TsDemux d2;
    std::vector<uint8_t> chunked;
    size_t p = 0;
    while (p < ts.size()) {
        size_t n = 1 + rng() % 997;
        if (p + n > ts.size()) n = ts.size() - p;
        d2.Feed(ts.data() + p, n, chunked);
        p += n;
    }
    CHECK(chunked == whole, "分块喂入输出应与一次性逐字节一致 (%zu vs %zu)\n", chunked.size(), whole.size());

    // 中途起流：本 fixture 的 PAT/PMT 只在分片头各出现一次（HLS 分片惯例），掉头后当前
    // 分片锁不上是预期；断言在于**字节级重同步**——垃圾尾料之后"下一分片"完整到达时，
    // 非对齐残留不得毒化后续解析，仍能锁定并产出完整链。
    media::TsDemux d3;
    std::vector<uint8_t> mid;
    d3.Feed(ts.data() + 1000, 4700, mid);  // 非 188 对齐的残料（掉了 PAT/PMT）
    CHECK(mid.empty() && !d3.audio_locked(), "无 PAT/PMT 的残料不应产出\n");
    d3.Feed(ts.data(), ts.size(), mid);    // "下一分片"完整到达
    CHECK(d3.audio_locked(), "残料后完整分片应重同步并锁定\n");
    size_t mid_consumed = 0;
    int mid_frames = WalkAdtsChain(mid, &mid_consumed);
    CHECK(mid_frames > 400, "重同步后应产出完整帧链 (got %d)\n", mid_frames);

    // 随机删 188B 包（模拟丢包）→ 不崩、仍有大量输出
    std::vector<uint8_t> holey;
    {
        std::vector<uint8_t> cut;
        cut.reserve(ts.size());
        for (size_t off = 0; off + 188 <= ts.size(); off += 188) {
            if (rng() % 20 == 0) continue;  // 丢 ~5% 包
            cut.insert(cut.end(), ts.begin() + off, ts.begin() + off + 188);
        }
        media::TsDemux d4;
        d4.Feed(cut.data(), cut.size(), holey);
        CHECK(d4.audio_locked() && holey.size() > 50000, "丢包流仍应解出大量音频 (got %zu)\n", holey.size());
    }

    // 变异 fuzz：随机翻字节，不崩即过（ASan/UBSan 兜底）
    std::vector<uint8_t> fuzz = ts;
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 50; i++) fuzz[rng() % fuzz.size()] = (uint8_t)rng();
        media::TsDemux df;
        std::vector<uint8_t> sink;
        df.Feed(fuzz.data(), fuzz.size(), sink);
    }
    // 纯垃圾也不崩
    std::vector<uint8_t> junk(64 * 1024);
    for (auto& b : junk) b = (uint8_t)rng();
    media::TsDemux dj;
    std::vector<uint8_t> sink;
    dj.Feed(junk.data(), junk.size(), sink);
}

// —— HLS playlist 纯解析层（media_hls.h）——

void TestHlsParse() {
    // JoinUrl：绝对透传 / 根相对 / 目录相对 / query 逐字节保留 / base 的 query 剥离
    CHECK(media::JoinUrl("http://a.cn/live/x/playlist.m3u8", "http://b.cn/y.ts") == "http://b.cn/y.ts",
          "绝对 URL 应透传\n");
    CHECK(media::JoinUrl("http://a.cn/live/x/playlist.m3u8", "seg1.ts") == "http://a.cn/live/x/seg1.ts",
          "目录相对拼接错误\n");
    CHECK(media::JoinUrl("http://a.cn/live/x/playlist.m3u8", "/z/seg.ts") == "http://a.cn/z/seg.ts",
          "根相对拼接错误\n");
    // satellitepull 形态：base 带会话 token（拼接时剥离），分片自带 token（逐字节保留）
    const std::string base = "http://112.192.19.74/satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8"
                             "?wsSession=abc-123&wsIPSercert=DEF&wsBindIP=2";
    const std::string seg = "1784647643.ts?wsSession=abc-123&wsIPSercert=DEF&wsBindIP=2&wsApp=HLS";
    CHECK(media::JoinUrl(base, seg) ==
              "http://112.192.19.74/satellitepull.cnr.cn/live/wxszfy971/" + seg,
          "token query 应逐字节保留且 base query 不参与拼接\n");

    CHECK(media::UrlIsHls("http://x/index.m3u8") && media::UrlIsHls("http://x/a.M3U8?tok=1") &&
              !media::UrlIsHls("http://x/64k.mp3") && !media::UrlIsHls("http://x/m3u8"),
          "UrlIsHls 判定错误\n");

    // master playlist：选最高带宽变体
    const std::string master = "#EXTM3U\r\n"
                               "#EXT-X-STREAM-INF:PROGRAM-ID=1, BANDWIDTH=230000\r\n"
                               "http://edge/a/playlist.m3u8?tok=1\r\n"
                               "#EXT-X-STREAM-INF:PROGRAM-ID=1, BANDWIDTH=64000\r\n"
                               "low/playlist.m3u8\r\n";
    auto mp = media::ParseM3u8(master, "http://cnr/live/x/playlist.m3u8");
    CHECK(mp.is_master && mp.variants.size() == 2 && mp.variants[0].bandwidth == 230000 &&
              mp.variants[0].url == "http://edge/a/playlist.m3u8?tok=1" &&
              mp.variants[1].url == "http://cnr/live/x/low/playlist.m3u8",
          "master 解析错误\n");

    // media playlist：SEQ/TARGETDURATION/分片序
    const std::string mediapl = "#EXTM3U\n"
                                "#EXT-X-VERSION:3\n"
                                "#EXT-X-MEDIA-SEQUENCE:15461468\n"
                                "#EXT-X-TARGETDURATION:10\n"
                                "#EXTINF:10.006,\n"
                                "15461468.ts\n"
                                "#EXTINF:10.005,\n"
                                "15461469.ts\n";
    auto pl = media::ParseM3u8(mediapl, "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8");
    CHECK(!pl.is_master && pl.media_sequence == 15461468 && pl.target_duration_s == 10 &&
              pl.segment_urls.size() == 2 && !pl.endlist &&
              pl.segment_urls[0] == "http://ngcdn001.cnr.cn/live/zgzs/15461468.ts",
          "media playlist 解析错误\n");

    auto vod = media::ParseM3u8("#EXTM3U\n#EXTINF:3,\na.ts\n#EXT-X-ENDLIST\n", "http://x/i.m3u8");
    CHECK(vod.endlist && vod.segment_urls.size() == 1, "ENDLIST 解析错误\n");

    // (g) 不支持特性：EXT-X-MAP（fMP4）与加密 EXT-X-KEY 置 unsupported；METHOD=NONE 不置
    auto m_map = media::ParseM3u8("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:3,\na.ts\n", "http://x/i.m3u8");
    CHECK(m_map.unsupported, "(g) EXT-X-MAP 应置不支持\n");
    auto m_key = media::ParseM3u8("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:3,\na.ts\n",
                                  "http://x/i.m3u8");
    CHECK(m_key.unsupported, "(g) 加密 EXT-X-KEY(AES-128) 应置不支持\n");
    auto m_none = media::ParseM3u8("#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:3,\na.ts\n", "http://x/i.m3u8");
    CHECK(!m_none.unsupported, "(g) METHOD=NONE 不应置不支持\n");

    // (h) AVERAGE-BANDWIDTH 在前、BANDWIDTH 在后并存 → 取 BANDWIDTH 的精确值
    auto m_bw = media::ParseM3u8(
        "#EXTM3U\n#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=1000,BANDWIDTH=230000\nhttp://e/v.m3u8\n",
        "http://x/i.m3u8");
    CHECK(m_bw.variants.size() == 1 && m_bw.variants[0].bandwidth == 230000,
          "(h) 应取 BANDWIDTH 精确匹配值而非 AVERAGE-BANDWIDTH (got %lld)\n",
          (long long)(m_bw.variants.empty() ? -1 : m_bw.variants[0].bandwidth));

    // (i) 协议相对 URL（//host/path）→ 接 base 的 scheme
    CHECK(media::JoinUrl("https://a.cn/live/x/playlist.m3u8", "//cdn.b.cn/seg.ts?t=1") ==
              "https://cdn.b.cn/seg.ts?t=1",
          "(i) 协议相对 URL 应接 base 的 scheme\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* fixture = argc > 1 ? argv[1] : "sim/testdata/cnr_seg.ts";
    TestHlsParse();
    TestSynthetic();
    TestSyntheticExtra();
    auto ts = ReadFile(fixture);
    if (ts.empty()) {
        std::fprintf(stderr, "ts_demux_test: fixture %s 不可读（从仓库根目录运行）\n", fixture);
        return 2;
    }
    TestFixture(ts);
    if (g_failures == 0) {
        std::printf("ts_demux_test: all passed\n");
        return 0;
    }
    std::printf("ts_demux_test: %d failure(s)\n", g_failures);
    return 1;
}
