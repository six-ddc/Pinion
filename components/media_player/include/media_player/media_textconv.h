#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// media_textconv —— GBK→UTF-8 转换 facade（Stage E）。
//
// ID3v2 文本帧声明为 encoding=0（"ISO-8859-1"）时，国内 MP3 标签工具常常塞的其实
// 是 GBK 字节（该字节序列既非合法 UTF-8 又不是标准 Latin-1 文本）。media_id3 侦测
// 到这种情况后调这里转 UTF-8；双端各自实现，零新增第三方依赖：
//   device（ESP-IDF）：ff_oem2uni(oem, 936) —— FatFs 已编入的 CP936 表
//     （CONFIG_FATFS_CODEPAGE_936=y 本仓已开），实现见 media_textconv_device.cc。
//   sim（macOS host）：iconv(3) GBK→UTF-8，实现见 sim/shim/src/media_textconv_sim.cc。
// ---------------------------------------------------------------------------
namespace media_textconv {

// data 是判定为 GBK 的原始字节串（偶数长度的高位字节对；调用方已完成启发式判定）。
// 每个字节对独立映射；映射失败的字节对退化为 U+FFFD（编码为 UTF-8 三字节）。
std::string GbkToUtf8(const uint8_t* data, size_t len);

}  // namespace media_textconv
