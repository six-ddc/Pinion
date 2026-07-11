// image_decode.h
// EPUB 插图/封面解码：JPEG（esp_new_jpeg，输出 RGB888）/ PNG（libpng 经典 API，
// alpha 与给定底色合成为 RGB888）→ 盒式滤波等比降采样进 (max_w, max_h)（只缩不放）。
// 全程 PSRAM，无 lv_* 调用；在 ebook_worker 线程使用。GIF/SVG/WebP 不支持（返回失败，
// 上层渲染 placeholder）。

#ifndef IMAGE_DECODE_H
#define IMAGE_DECODE_H

#include <cstddef>
#include <cstdint>

namespace image_decode {

// 原始尺寸 sanity 上限（超过直接拒绝，防解压炸弹 / 内存尖峰）。
constexpr uint32_t kMaxSrcDim = 4096;                       // 单边
constexpr uint32_t kMaxSrcPixels = 4096u * 3072u;           // 总像素（全尺寸 RGB888 ≈ 36MB 上限的 1/… 取安全值）

// 仅探测尺寸（PNG IHDR / JPEG SOFn 扫描），不解码。失败返回 false。
bool ProbeSize(const uint8_t* data, size_t len, uint16_t* w, uint16_t* h);

// 解码并适配：输出 RGB888 像素 buf（PSRAM，调用方 heap_caps_free），*out_w/*out_h 为
// 实际输出尺寸（≤ max_w/max_h，等比；原图小于盒时保持原尺寸）。bg888 = PNG alpha
// 合成底色（0xRRGGBB）。失败返回 nullptr。
uint8_t* DecodeFit(const uint8_t* data, size_t len, uint16_t max_w, uint16_t max_h,
                   uint32_t bg888, uint16_t* out_w, uint16_t* out_h);

}  // namespace image_decode

#endif  // IMAGE_DECODE_H
