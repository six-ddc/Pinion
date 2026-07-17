// media_textconv_sim.cc — sim（macOS host）侧 GBK→UTF-8 实现。见 media_textconv.h。
//
// 用 libiconv（macOS 自带，sim/CMakeLists 链 iconv）。设备侧用 FatFs CP936 表，
// 这里换一条独立实现路径，行为应等价（两端都是标准 GBK 码表）。
#include "media_player/media_textconv.h"

#include <iconv.h>

namespace media_textconv {

std::string GbkToUtf8(const uint8_t* data, size_t len) {
    if (len == 0) return {};
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) return {};

    std::string out;
    out.resize(len * 3 + 4);  // GBK 每 2 字节最多膨胀到 UTF-8 3 字节，留余量

    char* in_buf = const_cast<char*>(reinterpret_cast<const char*>(data));
    size_t in_left = len;
    char* out_buf = &out[0];
    size_t out_left = out.size();

    // 容错逐字节跳过：iconv 遇到非法/不可映射序列会停在原地报错，跳 1 字节
    // 重试，避免单个坏字节让后续全部文本转换失败（ID3 tag 可能被工具写坏）。
    while (in_left > 0) {
        size_t r = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
        if (r == static_cast<size_t>(-1)) {
            if (in_left == 0) break;
            in_buf++;
            in_left--;
            if (out_left > 0) {
                *out_buf++ = '?';
                out_left--;
            }
            continue;
        }
        break;
    }
    iconv_close(cd);
    out.resize(out.size() - out_left);
    return out;
}

}  // namespace media_textconv
