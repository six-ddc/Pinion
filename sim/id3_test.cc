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

}  // namespace

int main(int argc, char** argv) {
    int fuzz_rounds = 1000;
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

    std::vector<uint8_t> seed;
    if (ReadFile(dir + "tag_utf8.mp3", &seed)) {
        // 只 fuzz 前 64KB（含整个 tag 区 + 一点音频），fread 更快，覆盖面不受影响。
        if (seed.size() > 65536) seed.resize(65536);
        FuzzRun(seed, fuzz_rounds);
    } else {
        std::printf("[fuzz] SKIP: seed file not found (run gen_id3_fixtures.py first)\n");
    }

    std::printf("id3_test: all checks completed\n");
    return 0;
}
