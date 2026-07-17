// media_textconv_device.cc — device（ESP-IDF）侧 GBK→UTF-8 实现。见头文件。
//
// 复用 FatFs 已编入的 CP936（简体中文）双字节码表：ff_oem2uni(oem, 936)，
// oem 为 (hi<<8)|lo 打包的双字节值（ff.h:374，DBCS 定长码页时 oem>=0x100 即
// 双字节输入）。本仓 CONFIG_FATFS_CODEPAGE_936=y 已开，零新增 flash 占用。
#include "media_player/media_textconv.h"

#include "ff.h"

namespace media_textconv {

namespace {

// UTF-16 codepoint -> UTF-8 字节，追加到 out。
void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0) {
        out += '?';
    } else if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

std::string GbkToUtf8(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len);
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = data[i];
        if (b0 < 0x80) {  // ASCII 直通
            out += static_cast<char>(b0);
            i++;
            continue;
        }
        if (i + 1 >= len) {  // 落单高位字节：无法配对，占位后结束
            out += '?';
            break;
        }
        uint8_t b1 = data[i + 1];
        uint16_t oem = static_cast<uint16_t>((b0 << 8) | b1);
        WCHAR uni = ff_oem2uni(oem, 936);
        AppendUtf8(out, uni);
        i += 2;
    }
    return out;
}

}  // namespace media_textconv
