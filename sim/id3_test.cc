// id3_test — host unit test + fuzz harness for media_id3 (Stage E).
//
// 直测 pi_sim_sd/Music/tag_*.mp3 五个夹具（gen_id3_fixtures.py 生成）的
// title/album/artist/cover 解析结果，并对曲1的字节做随机变异做粗 fuzz
// （每轮写临时文件、调用 ReadTags+ReadCover，任何 crash 都会让本进程非零退出/
// 段错误，CI 意义上"跑完不崩"即通过）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "media_player/media_id3.h"

namespace {

std::string SdMusic() { return "./pi_sim_sd/Music/"; }

void PrintTags(const char* label, const std::string& path) {
    media_id3::Tags t = media_id3::ReadTags(path);
    std::printf("[%s] path=%s has_any=%d\n  title=\"%s\"\n  album=\"%s\"\n  artist=\"%s\"\n", label,
               path.c_str(), t.has_any, t.title.c_str(), t.album.c_str(), t.artist.c_str());
}

void PrintCover(const char* label, const std::string& path) {
    size_t sz = 0;
    std::string mime;
    uint8_t* buf = media_id3::ReadCover(path, &sz, &mime);
    if (buf == nullptr) {
        std::printf("[%s] cover=NONE\n", label);
        return;
    }
    int w = 0, h = 0;
    bool ok = media_id3::PeekImageSize(buf, sz, &w, &h);
    std::printf("[%s] cover mime=%s size=%zuB peek=%s %dx%d\n", label, mime.c_str(), sz,
               ok ? "ok" : "FAIL", w, h);
    free(buf);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// 粗 fuzz：对一份合法 ID3+mp3 字节做 N 轮随机字节翻转/截断，写临时文件后调
// ReadTags/ReadCover，只要求"跑完不崩"（进程存活到打印 done）。
void FuzzRun(const std::vector<uint8_t>& seed, int rounds) {
    std::srand(12345);
    const std::string tmp_path = "/tmp/_id3_fuzz_tmp.mp3";
    int crashes_would_be_fatal = 0;
    for (int r = 0; r < rounds; r++) {
        std::vector<uint8_t> mut = seed;
        int nmut = 1 + std::rand() % 12;
        for (int k = 0; k < nmut; k++) {
            if (mut.empty()) break;
            size_t idx = static_cast<size_t>(std::rand()) % mut.size();
            mut[idx] = static_cast<uint8_t>(std::rand() & 0xFF);
        }
        // 10% 轮次额外做随机截断，覆盖"传输中断"场景。
        if (std::rand() % 10 == 0 && mut.size() > 16) {
            size_t cut = 8 + static_cast<size_t>(std::rand()) % (mut.size() - 8);
            mut.resize(cut);
        }
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(mut.data()), static_cast<std::streamsize>(mut.size()));
        }
        media_id3::Tags t = media_id3::ReadTags(tmp_path);
        (void)t;
        (void)media_id3::ProbeDurationS(tmp_path);  // 时长探测同受畸形输入安全契约约束
        size_t sz = 0;
        std::string mime;
        uint8_t* buf = media_id3::ReadCover(tmp_path, &sz, &mime);
        if (buf != nullptr) {
            int w = 0, h = 0;
            media_id3::PeekImageSize(buf, sz, &w, &h);
            free(buf);
        }
        crashes_would_be_fatal++;  // 若上面任何一步段错误，这行永远不会跑完全部 rounds
    }
    std::printf("[fuzz] %d/%d rounds completed without crash\n", crashes_would_be_fatal, rounds);
}

// ===========================================================================
// Self-contained unit tests (Stage E fix): construct ID3 tags / JPEG headers
// byte-by-byte, write to /tmp, assert parse results. No external fixtures / no
// LVGL — covers EXIF JFIF normalization, v2.4 non-syncsafe frame size, encrypted/
// compressed/DLI frame flags, isolated UTF-16 surrogate → U+FFFD, GBK trailing NUL.
// ===========================================================================
int g_fail = 0;
void Check(bool cond, const char* name, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", cond ? "PASS" : "FAIL", name, detail.empty() ? "" : " -- ",
               detail.c_str());
    if (!cond) g_fail++;
}

void PutBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back(x & 0xFF);
}
void PutSyncsafe(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 21) & 0x7F);
    v.push_back((x >> 14) & 0x7F);
    v.push_back((x >> 7) & 0x7F);
    v.push_back(x & 0x7F);
}

struct Frame {
    std::string id;
    std::vector<uint8_t> body;
    uint8_t flag_lo = 0;      // format flags byte (fh[9])
    bool size_raw = false;    // v2.4: write size as plain Be32 (simulate non-syncsafe encoder)
};

std::vector<uint8_t> BuildTag(int major, const std::vector<Frame>& frames) {
    std::vector<uint8_t> body;
    for (const Frame& fr : frames) {
        std::vector<uint8_t> fh;
        for (int k = 0; k < 4; k++) fh.push_back(k < (int)fr.id.size() ? (uint8_t)fr.id[k] : (uint8_t)' ');
        uint32_t sz = (uint32_t)fr.body.size();
        if (major == 4 && !fr.size_raw)
            PutSyncsafe(fh, sz);
        else
            PutBe32(fh, sz);  // v2.3 always Be32; v2.4+size_raw simulates a non-compliant encoder
        fh.push_back(0x00);        // flags hi
        fh.push_back(fr.flag_lo);  // flags lo
        body.insert(body.end(), fh.begin(), fh.end());
        body.insert(body.end(), fr.body.begin(), fr.body.end());
    }
    std::vector<uint8_t> tag = {'I', 'D', '3', (uint8_t)major, 0x00, 0x00};
    PutSyncsafe(tag, (uint32_t)body.size());  // tag size (always syncsafe)
    tag.insert(tag.end(), body.begin(), body.end());
    for (int k = 0; k < 64; k++) tag.push_back(0x00);  // fake padding/audio tail
    return tag;
}

bool WriteBytes(const std::string& path, const std::vector<uint8_t>& v) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o.good()) return false;
    o.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)v.size());
    return o.good();
}

std::vector<uint8_t> TextBody(uint8_t enc, const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> b = {enc};
    b.insert(b.end(), raw.begin(), raw.end());
    return b;
}

// A tiny baseline-shaped JPEG whose FIRST app marker is APP1/Exif (not JFIF APP0).
// Has a real SOF0 so PeekImageSize can read WxH; not fully decodable (no scan data),
// but that is irrelevant to the byte-level normalization invariants under test.
std::vector<uint8_t> ExifJpeg(uint16_t w, uint16_t h) {
    std::vector<uint8_t> j = {
        0xFF, 0xD8,                                            // SOI
        0xFF, 0xE1, 0x00, 0x08, 0x45, 0x78, 0x69, 0x66, 0x00,  // APP1 "Exif", len=8
        0x00,
        0xFF, 0xC0, 0x00, 0x11, 0x08,                          // SOF0, len=17, precision 8
        (uint8_t)(h >> 8), (uint8_t)(h & 0xFF),                // height
        (uint8_t)(w >> 8), (uint8_t)(w & 0xFF),                // width
        0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,  // 3 comps
        0xFF, 0xD9,                                            // EOI
    };
    return j;
}

int RunUnitTests() {
    std::printf("== unit tests ==\n");
    const std::string tmp = "/tmp/_id3_ut.mp3";

    // ---- T1: EXIF/non-JFIF JPEG header normalization -----------------------
    {
        std::vector<uint8_t> exif = ExifJpeg(400, 300);
        int iw = 0, ih = 0;
        bool pk = media_id3::PeekImageSize(exif.data(), exif.size(), &iw, &ih);
        Check(pk && iw == 400 && ih == 300, "T1 PeekImageSize(EXIF) reads WxH");

        size_t nlen = 0;
        uint8_t* norm = media_id3::NormalizeJpegHeader(exif.data(), exif.size(), &nlen);
        Check(norm != nullptr && nlen == exif.size() + 18, "T1 normalize allocates +18B");
        if (norm != nullptr) {
            static const uint8_t sig[10] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46};
            Check(std::memcmp(norm, sig, 10) == 0, "T1 normalized head == tjpgd JFIF signature");
            int nw = 0, nh = 0;
            bool npk = media_id3::PeekImageSize(norm, nlen, &nw, &nh);
            Check(npk && nw == 400 && nh == 300, "T1 PeekImageSize(normalized) still WxH (APP0 skipped)");
            Check(std::memcmp(norm + 20, exif.data() + 2, exif.size() - 2) == 0,
                 "T1 original payload preserved after inserted APP0");
            free(norm);
        }
        // Already-compliant JFIF must NOT be rewritten.
        std::vector<uint8_t> jfif = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46,
                                     0x49, 0x46, 0x00, 0x01, 0x01, 0x00};
        Check(media_id3::NormalizeJpegHeader(jfif.data(), jfif.size(), nullptr) == nullptr,
             "T1 compliant JFIF not rewritten");
        // PNG must NOT be rewritten.
        std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        Check(media_id3::NormalizeJpegHeader(png.data(), png.size(), nullptr) == nullptr,
             "T1 PNG not rewritten");
    }

    // ---- T2: v2.4 frame size written as non-syncsafe (raw big-endian) -------
    {
        std::vector<uint8_t> album;  // 200-byte body: enc + 199 'x' (interior never looks like a frame id)
        album.push_back(0x03);
        for (int k = 0; k < 199; k++) album.push_back('x');
        std::vector<uint8_t> artist = TextBody(0x03, {'A', 'r', 't', 'i', 's', 't'});
        std::vector<uint8_t> tag = BuildTag(4, {
            {"TALB", album, 0x00, /*size_raw=*/true},  // size 200=0xC8 -> non-syncsafe
            {"TPE1", artist, 0x00, false},
        });
        WriteBytes(tmp, tag);
        media_id3::Tags t = media_id3::ReadTags(tmp);
        Check(t.artist == "Artist", "T2 frame after non-syncsafe-sized frame is reached",
             "artist='" + t.artist + "'");
        Check(t.album.size() == 199, "T2 non-syncsafe frame body decoded at raw length");
    }

    // ---- T3a: v2.4 encrypted frame skipped (not decoded as text) -----------
    {
        std::vector<uint8_t> enc_tit2 = TextBody(0x03, {'S', 'E', 'C', 'R', 'E', 'T'});
        std::vector<uint8_t> album = TextBody(0x03, {'R', 'e', 'a', 'l', 'A', 'l', 'b'});
        std::vector<uint8_t> tag = BuildTag(4, {
            {"TIT2", enc_tit2, 0x04, false},  // 0x04 = encryption
            {"TALB", album, 0x00, false},
        });
        WriteBytes(tmp, tag);
        media_id3::Tags t = media_id3::ReadTags(tmp);
        Check(t.title.empty(), "T3a encrypted TIT2 not applied as title", "title='" + t.title + "'");
        Check(t.album == "RealAlb", "T3a frame after encrypted frame still parsed");
    }

    // ---- T3b: v2.4 compressed frame skipped --------------------------------
    {
        std::vector<uint8_t> comp = TextBody(0x03, {'Z', 'Z', 'Z'});
        std::vector<uint8_t> album = TextBody(0x03, {'O', 'K'});
        std::vector<uint8_t> tag = BuildTag(4, {
            {"TIT2", comp, 0x08, false},  // 0x08 = compression
            {"TALB", album, 0x00, false},
        });
        WriteBytes(tmp, tag);
        media_id3::Tags t = media_id3::ReadTags(tmp);
        Check(t.title.empty() && t.album == "OK", "T3b compressed frame skipped, next parsed");
    }

    // ---- T3c: v2.4 data-length-indicator shifts body by 4 ------------------
    {
        std::vector<uint8_t> body = {0x00, 0x00, 0x00, 0x02};  // DLI: 4 syncsafe length bytes
        std::vector<uint8_t> real = TextBody(0x03, {'H', 'i'});
        body.insert(body.end(), real.begin(), real.end());
        std::vector<uint8_t> tag = BuildTag(4, {{"TIT2", body, 0x01, false}});  // 0x01 = DLI
        WriteBytes(tmp, tag);
        media_id3::Tags t = media_id3::ReadTags(tmp);
        Check(t.title == "Hi", "T3c DLI frame body offset +4 applied", "title='" + t.title + "'");
    }

    // ---- T4: isolated UTF-16 high surrogate -> U+FFFD ----------------------
    {
        // enc=1 UTF-16+BOM(LE), units: 'A', 0xD800 (lone high surrogate), 'B'
        std::vector<uint8_t> raw = {0xFF, 0xFE, 0x41, 0x00, 0x00, 0xD8, 0x42, 0x00};
        std::vector<uint8_t> tag = BuildTag(4, {{"TIT2", TextBody(0x01, raw), 0x00, false}});
        WriteBytes(tmp, tag);
        media_id3::Tags t = media_id3::ReadTags(tmp);
        std::string want = "A\xEF\xBF\xBD" "B";  // "A" + U+FFFD + "B"
        Check(t.title == want, "T4 lone surrogate -> U+FFFD (no ill-formed UTF-8)",
             "len=" + std::to_string(t.title.size()));
    }

    // ---- T5: GBK enc=0 trailing NUL is trimmed -----------------------------
    {
        // GBK "你好" = C4 E3 BA C3 (not valid UTF-8 -> GBK branch), + trailing NUL
        std::vector<uint8_t> gbk = {0xC4, 0xE3, 0xBA, 0xC3, 0x00};
        std::vector<uint8_t> tag = BuildTag(4, {{"TIT2", TextBody(0x00, gbk), 0x00, false}});
        WriteBytes(tmp, tag);
        media_id3::Tags t = media_id3::ReadTags(tmp);
        Check(!t.title.empty() && t.title.back() != '\0', "T5 GBK title has no trailing NUL",
             "bytes=" + std::to_string(t.title.size()));
        Check(t.title == "你好", "T5 GBK '你好' decoded", "title='" + t.title + "'");
    }

    // ---- T6: ProbeDurationS —— CBR 估算与 Xing 帧数两条路径 ----------------
    {
        // MPEG1 Layer3 128kbps 44.1kHz stereo 帧头：FF FB 90 00，帧长 417B。
        const uint8_t kHdr[4] = {0xFF, 0xFB, 0x90, 0x00};
        const int kFrameLen = 417;
        // CBR：100 帧纯音频（无 tag）→ 100*1152/44100 ≈ 2s；估算 = bytes*8/128000。
        std::vector<uint8_t> cbr;
        for (int i = 0; i < 100; i++) {
            cbr.insert(cbr.end(), kHdr, kHdr + 4);
            cbr.insert(cbr.end(), kFrameLen - 4, 0x55);
        }
        WriteBytes(tmp, cbr);
        int d = media_id3::ProbeDurationS(tmp);
        Check(d == (int)((int64_t)cbr.size() * 8 / 128000), "T6 CBR duration estimate",
             "d=" + std::to_string(d));

        // Xing：首帧 side info(32B) 后放 "Xing"+flags(1)+frames=38*300（≈300s）。
        std::vector<uint8_t> vbr;
        vbr.insert(vbr.end(), kHdr, kHdr + 4);
        vbr.insert(vbr.end(), 32, 0x00);  // side info
        const char* xing = "Xing";
        vbr.insert(vbr.end(), xing, xing + 4);
        PutBe32(vbr, 1);          // flags: FRAMES
        PutBe32(vbr, 38 * 300);   // 帧数
        vbr.resize(kFrameLen, 0); // 补满首帧
        vbr.insert(vbr.end(), cbr.begin(), cbr.end());  // 后接若干真实帧
        WriteBytes(tmp, vbr);
        d = media_id3::ProbeDurationS(tmp);
        int expect = (int)((int64_t)(38 * 300) * 1152 / 44100);
        Check(d == expect, "T6 Xing frame-count duration",
             "d=" + std::to_string(d) + " expect=" + std::to_string(expect));

        // 垃圾文件（无同步头）→ 0。
        std::vector<uint8_t> junk(4096, 0x11);
        WriteBytes(tmp, junk);
        Check(media_id3::ProbeDurationS(tmp) == 0, "T6 garbage -> 0");
    }

    std::printf("== unit tests done: %d failure(s) ==\n", g_fail);
    return g_fail;
}

// Synthetic fuzz seed (valid v2.4 tag with text frames + a small APIC) so the fuzz
// harness is self-contained when the generated .mp3 fixtures are absent.
std::vector<uint8_t> MakeSeedTag() {
    std::vector<uint8_t> apic;
    apic.push_back(0x00);  // text encoding
    const char* mime = "image/jpeg";
    for (const char* p = mime; *p; p++) apic.push_back((uint8_t)*p);
    apic.push_back(0x00);  // mime NUL
    apic.push_back(0x03);  // picture type = front cover
    apic.push_back(0x00);  // empty description NUL
    std::vector<uint8_t> jpg = ExifJpeg(64, 64);
    apic.insert(apic.end(), jpg.begin(), jpg.end());
    return BuildTag(4, {
        {"TIT2", TextBody(0x03, {'S', 'o', 'n', 'g'}), 0x00, false},
        {"TALB", TextBody(0x03, {'A', 'l', 'b', 'u', 'm'}), 0x00, false},
        {"TPE1", TextBody(0x03, {'A', 'r', 't'}), 0x00, false},
        {"APIC", apic, 0x00, false},
    });
}

}  // namespace

int main(int argc, char** argv) {
    int fuzz_rounds = 20000;
    if (argc > 1) fuzz_rounds = std::atoi(argv[1]);

    std::string dir = SdMusic();
    PrintTags("track1 utf8+jpeg", dir + "tag_utf8.mp3");
    PrintCover("track1 utf8+jpeg", dir + "tag_utf8.mp3");
    PrintTags("track2 gbk", dir + "tag_gbk.mp3");
    PrintTags("track3 none", dir + "tag_none.mp3");
    PrintTags("track4 malformed", dir + "tag_malformed.mp3");
    PrintCover("track4 malformed", dir + "tag_malformed.mp3");
    PrintTags("track5 utf8+png", dir + "tag_png.mp3");
    PrintCover("track5 utf8+png", dir + "tag_png.mp3");

    int fails = RunUnitTests();

    // Fuzz: prefer a real fixture seed; else a self-contained synthetic tag.
    std::vector<uint8_t> seed;
    if (!ReadFile(dir + "tag_utf8.mp3", &seed)) {
        std::printf("[fuzz] fixture seed absent -> using synthetic seed\n");
        seed = MakeSeedTag();
    }
    if (seed.size() > 65536) seed.resize(65536);  // 只 fuzz 前 64KB，覆盖面不受影响
    FuzzRun(seed, fuzz_rounds);

    std::printf("id3_test: all checks completed (unit failures=%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
