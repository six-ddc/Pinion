// media_file_source — 本地/SD 卡文件字节源，POSIX fopen/fread（device/sim 共享）。
#include <cstdio>

#include "media_player/media_source.h"

namespace media {
namespace {

class FileSource : public MediaSource {
 public:
    explicit FileSource(FILE* f) : fp_(f) {}
    ~FileSource() override { Close(); }

    int Read(uint8_t* buf, size_t max) override {
        if (fp_ == nullptr || buf == nullptr || max == 0) return -1;
        size_t n = std::fread(buf, 1, max, fp_);
        if (n > 0) return (int)n;
        // n==0：EOF 或错误。文件源用 feof 区分（错误极罕见，按 EOF 处理即进下一曲）。
        return std::feof(fp_) ? 0 : -1;
    }

    void Close() override {
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    bool IsStream() const override { return false; }

 private:
    FILE* fp_ = nullptr;
};

}  // namespace

std::unique_ptr<MediaSource> OpenFileSource(const char* path) {
    if (path == nullptr || path[0] == '\0') return nullptr;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return nullptr;
    return std::unique_ptr<MediaSource>(new FileSource(f));
}

}  // namespace media
