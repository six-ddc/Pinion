// image_decode.cc — EPUB 插图/封面解码（M4）。
// JPEG 走 esp_new_jpeg（RGB888 输出）；PNG 走 libpng 经典 API（palette/gray/16bit/tRNS
// 归一化 + alpha 与 bg888 合成）。解码到全尺寸 RGB888 后盒式滤波等比降采样进 (max_w,
// max_h)（只缩不放）。全程 PSRAM，无 lv_* 调用，在 ebook_worker 线程使用。
//
// 【字节序约定】返回 buffer 是 LVGL9 的 RGB888 内存布局，即每像素 byte[0]=Blue、
// byte[1]=Green、byte[2]=Red（BGR 字节序）——见 lvgl 的
// src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c（dest[x+0]=blue, dest[x+2]=red）。
// 两个解码源（esp_new_jpeg 的 RGB888 / libpng 的 RGB）都是 R,G,B 低到高，故在最终的
// 缩放/拷贝一趟里顺手做 R/B 交换（零额外开销）。bg888 参数按 0xRRGGBB 语义解释，合成在
// RGB 域进行，交换统一由 BoxDownscale 完成。

#include "image_decode.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstring>

#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

#include <png.h>

namespace image_decode {

namespace {

constexpr char TAG[] = "img_decode";

// PSRAM 优先分配（照抄 book_source.cc 的语义）；返回的 buffer 用 heap_caps_free 释放。
void* PsramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

// ---- 大端读取 -------------------------------------------------------------
inline uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint16_t Be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// ---- magic 判定 -----------------------------------------------------------
bool IsPng(const uint8_t* d, size_t n) {
    static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return n >= 8 && std::memcmp(d, kSig, 8) == 0;
}
bool IsJpeg(const uint8_t* d, size_t n) {
    return n >= 2 && d[0] == 0xFF && d[1] == 0xD8;
}

// 原始尺寸 sanity（0 或超上限一律拒绝，防解压炸弹 / 内存尖峰）。
bool DimOk(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (w > kMaxSrcDim || h > kMaxSrcDim) return false;
    if (static_cast<uint64_t>(w) * h > kMaxSrcPixels) return false;
    return true;
}

// ---- 目标尺寸：等比缩进 (max_w, max_h) 盒，只缩不放 ------------------------
void FitBox(uint32_t sw, uint32_t sh, uint32_t max_w, uint32_t max_h, uint32_t* ow, uint32_t* oh) {
    if (max_w == 0) max_w = 1;
    if (max_h == 0) max_h = 1;
    if (sw <= max_w && sh <= max_h) {  // 原图已在盒内：保持原尺寸
        *ow = sw;
        *oh = sh;
        return;
    }
    // 先按宽度贴合，超高再按高度贴合
    uint32_t w = max_w;
    uint32_t h = static_cast<uint32_t>(static_cast<uint64_t>(sh) * max_w / sw);
    if (h == 0 || h > max_h) {
        h = max_h;
        w = static_cast<uint32_t>(static_cast<uint64_t>(sw) * max_h / sh);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    *ow = w;
    *oh = h;
}

// ---- 盒式滤波降采样 + R/B 交换：src RGB888(sw*sh) → dst BGR888(dw*dh)（只缩不放）----
// 输入源为 R,G,B 低到高；输出按 LVGL9 RGB888 内存布局 = B,G,R（见文件头字节序约定），
// R/B 交换在这趟拷贝里顺手完成，零额外开销。目标像素 = 覆盖的源像素矩形块分量均值；
// 块边界按 srcDim/dstDim 整数网格。dst 由调用方分配（dw*dh*3）。dw==sw&&dh==sh 退化为
// 整像素拷贝（仍做 R/B 交换）。
void BoxDownscale(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw,
                  uint32_t dh) {
    for (uint32_t oy = 0; oy < dh; oy++) {
        uint32_t sy0 = static_cast<uint32_t>(static_cast<uint64_t>(oy) * sh / dh);
        uint32_t sy1 = static_cast<uint32_t>(static_cast<uint64_t>(oy + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        for (uint32_t ox = 0; ox < dw; ox++) {
            uint32_t sx0 = static_cast<uint32_t>(static_cast<uint64_t>(ox) * sw / dw);
            uint32_t sx1 = static_cast<uint32_t>(static_cast<uint64_t>(ox + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            // 累加块内 RGB。块像素数 ≤ kMaxSrcPixels，sum ≤ 12.58M*255 < 2^32，u32 不溢出。
            uint32_t sr = 0, sg = 0, sb = 0;
            uint32_t cnt = (sx1 - sx0) * (sy1 - sy0);
            for (uint32_t y = sy0; y < sy1; y++) {
                const uint8_t* row = src + (static_cast<size_t>(y) * sw + sx0) * 3;
                for (uint32_t x = sx0; x < sx1; x++) {
                    sr += row[0];
                    sg += row[1];
                    sb += row[2];
                    row += 3;
                }
            }
            uint8_t* o = dst + (static_cast<size_t>(oy) * dw + ox) * 3;
            o[0] = static_cast<uint8_t>(sb / cnt);  // Blue
            o[1] = static_cast<uint8_t>(sg / cnt);  // Green
            o[2] = static_cast<uint8_t>(sr / cnt);  // Red
        }
    }
}

// ---- JPEG（esp_new_jpeg，非分块 RGB888）-----------------------------------
uint8_t* DecodeJpeg(const uint8_t* data, size_t len, uint16_t max_w, uint16_t max_h,
                    uint16_t* out_w, uint16_t* out_h) {
    // 避免 DEFAULT_JPEG_DEC_CONFIG() 的指派初始化（C++ 下不便），逐字段设置。
    jpeg_dec_config_t config = {};
    config.output_type = JPEG_PIXEL_FORMAT_RGB888;
    config.rotate = JPEG_ROTATE_0D;
    config.block_enable = false;

    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&config, &dec) != JPEG_ERR_OK || dec == nullptr) {
        ESP_LOGW(TAG, "jpeg_dec_open 失败");
        return nullptr;
    }

    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(data);  // 库只读输入，不修改
    io.inbuf_len = static_cast<int>(len);

    jpeg_dec_header_info_t info = {};
    jpeg_error_t jr = jpeg_dec_parse_header(dec, &io, &info);
    if (jr != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg 解析头失败: %d", static_cast<int>(jr));
        jpeg_dec_close(dec);
        return nullptr;
    }
    uint32_t sw = info.width, sh = info.height;
    if (!DimOk(sw, sh)) {
        ESP_LOGW(TAG, "jpeg 尺寸超限 %ux%u", static_cast<unsigned>(sw), static_cast<unsigned>(sh));
        jpeg_dec_close(dec);
        return nullptr;
    }

    // 非分块模式下 RGB888 输出紧凑排布：行距 = sw*3，无块对齐 padding。
    size_t dec_len = static_cast<size_t>(sw) * sh * 3;
    uint8_t* dec_buf = static_cast<uint8_t*>(jpeg_calloc_align(dec_len, 16));
    if (dec_buf == nullptr) {
        ESP_LOGW(TAG, "jpeg 解码 buffer 分配失败 (%u bytes)", static_cast<unsigned>(dec_len));
        jpeg_dec_close(dec);
        return nullptr;
    }
    io.outbuf = dec_buf;
    jr = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    if (jr != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg 解码失败: %d", static_cast<int>(jr));
        jpeg_free_align(dec_buf);
        return nullptr;
    }

    // 缩放/拷贝进 PsramAlloc 目标 buffer（统一用 heap_caps_free 释放）。
    uint32_t dw = 0, dh = 0;
    FitBox(sw, sh, max_w, max_h, &dw, &dh);
    uint8_t* dst = static_cast<uint8_t*>(PsramAlloc(static_cast<size_t>(dw) * dh * 3));
    if (dst == nullptr) {
        ESP_LOGW(TAG, "jpeg 目标 buffer 分配失败");
        jpeg_free_align(dec_buf);
        return nullptr;
    }
    BoxDownscale(dec_buf, sw, sh, dst, dw, dh);
    jpeg_free_align(dec_buf);
    *out_w = static_cast<uint16_t>(dw);
    *out_h = static_cast<uint16_t>(dh);
    return dst;
}

// ---- PNG（libpng 经典 API）------------------------------------------------
struct PngReader {
    const uint8_t* data;
    size_t len;
    size_t pos;
};

void PngReadFn(png_structp png, png_bytep out, png_size_t count) {
    PngReader* r = static_cast<PngReader*>(png_get_io_ptr(png));
    if (r == nullptr || r->pos + count > r->len) {
        png_error(png, "read past end");  // 触发 longjmp
        return;
    }
    std::memcpy(out, r->data + r->pos, count);
    r->pos += count;
}

void PngErrorFn(png_structp png, png_const_charp msg) {
    ESP_LOGW(TAG, "libpng error: %s", msg);
    longjmp(png_jmpbuf(png), 1);
}
void PngWarnFn(png_structp png, png_const_charp msg) {
    (void)png;
    (void)msg;  // 忽略警告
}
png_voidp PngMalloc(png_structp png, png_alloc_size_t size) {
    (void)png;
    return PsramAlloc(size);  // libpng 内部分配也走 PSRAM，减内部 RAM 压力
}
void PngFree(png_structp png, png_voidp ptr) {
    (void)png;
    heap_caps_free(ptr);
}

uint8_t* DecodePng(const uint8_t* data, size_t len, uint16_t max_w, uint16_t max_h, uint32_t bg888,
                   uint16_t* out_w, uint16_t* out_h) {
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, nullptr, PngErrorFn,
                                               PngWarnFn, nullptr, PngMalloc, PngFree);
    if (png == nullptr) {
        ESP_LOGW(TAG, "png_create_read_struct 失败");
        return nullptr;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        ESP_LOGW(TAG, "png_create_info_struct 失败");
        return nullptr;
    }

    // setjmp 之后被赋值、又在错误分支被引用的指针须 volatile，避免寄存器缓存导致
    // longjmp 后取到过期值（libpng 惯例）。
    uint8_t* volatile raw = nullptr;     // 整幅 RGB/RGBA 像素
    png_bytep* volatile rows = nullptr;  // 行指针数组
    if (setjmp(png_jmpbuf(png))) {
        // 任一 libpng 调用（或本函数主动 longjmp）出错跳到这里
        if (rows) heap_caps_free(rows);
        if (raw) heap_caps_free(raw);
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }

    PngReader reader = {data, len, 0};
    png_set_read_fn(png, &reader, PngReadFn);
    png_read_info(png, info);

    png_uint_32 sw = 0, sh = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    png_get_IHDR(png, info, &sw, &sh, &bit_depth, &color_type, &interlace, nullptr, nullptr);
    if (!DimOk(sw, sh)) {
        ESP_LOGW(TAG, "png 尺寸超限 %ux%u", static_cast<unsigned>(sw), static_cast<unsigned>(sh));
        longjmp(png_jmpbuf(png), 1);
    }

    // 归一化到 8bit RGB / RGBA：
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    // 隔行：必须在 png_read_update_info 之前调用，交给 libpng 在 png_read_image 内处理。
    (void)png_set_interlace_handling(png);
    png_read_update_info(png, info);

    int channels = png_get_channels(png, info);
    size_t rowbytes = png_get_rowbytes(png, info);
    if (channels != 3 && channels != 4) {  // 归一化后应为 RGB(3) 或 RGBA(4)
        ESP_LOGW(TAG, "png 非预期通道数 %d", channels);
        longjmp(png_jmpbuf(png), 1);
    }

    // 整幅像素 + 行指针一次读入（png_read_image 自动跑完隔行的多个 pass）。
    raw = static_cast<uint8_t*>(PsramAlloc(rowbytes * sh));
    rows = static_cast<png_bytep*>(PsramAlloc(static_cast<size_t>(sh) * sizeof(png_bytep)));
    if (raw == nullptr || rows == nullptr) {
        ESP_LOGW(TAG, "png 像素 buffer 分配失败");
        longjmp(png_jmpbuf(png), 1);
    }
    for (png_uint_32 y = 0; y < sh; y++) rows[y] = raw + static_cast<size_t>(y) * rowbytes;
    png_read_image(png, rows);

    // 读完即销毁 libpng（后续纯 CPU 运算，不再有 longjmp）。
    png_destroy_read_struct(&png, &info, nullptr);
    heap_caps_free(rows);
    rows = nullptr;

    uint8_t* raw_px = raw;  // 转成普通指针（不再 longjmp，无需 volatile）
    // 含 alpha：与 bg888 合成并原地压成 RGB888（3i ≤ 4i，前向压缩不覆盖未读数据）。
    // 这里仍写 R,G,B（与无 alpha 分支一致），R/B 交换到 BGR 统一在 BoxDownscale 里做。
    if (channels == 4) {
        uint32_t bg_r = (bg888 >> 16) & 0xFF;
        uint32_t bg_g = (bg888 >> 8) & 0xFF;
        uint32_t bg_b = bg888 & 0xFF;
        size_t npx = static_cast<size_t>(sw) * sh;
        for (size_t i = 0; i < npx; i++) {
            const uint8_t* s = raw_px + i * 4;
            uint8_t* o = raw_px + i * 3;
            uint32_t a = s[3];
            uint32_t ia = 255 - a;
            o[0] = static_cast<uint8_t>((s[0] * a + bg_r * ia + 127) / 255);
            o[1] = static_cast<uint8_t>((s[1] * a + bg_g * ia + 127) / 255);
            o[2] = static_cast<uint8_t>((s[2] * a + bg_b * ia + 127) / 255);
        }
    }
    // channels==3：raw_px 已是紧凑 RGB888（rowbytes == sw*3）。

    uint32_t dw = 0, dh = 0;
    FitBox(sw, sh, max_w, max_h, &dw, &dh);
    uint8_t* dst = static_cast<uint8_t*>(PsramAlloc(static_cast<size_t>(dw) * dh * 3));
    if (dst == nullptr) {
        ESP_LOGW(TAG, "png 目标 buffer 分配失败");
        heap_caps_free(raw_px);
        return nullptr;
    }
    BoxDownscale(raw_px, sw, sh, dst, dw, dh);
    heap_caps_free(raw_px);
    *out_w = static_cast<uint16_t>(dw);
    *out_h = static_cast<uint16_t>(dh);
    return dst;
}

}  // namespace

bool ProbeSize(const uint8_t* data, size_t len, uint16_t* w, uint16_t* h) {
    if (data == nullptr || w == nullptr || h == nullptr) return false;

    if (IsPng(data, len)) {
        // 8B 签名 + 4B 段长 + 4B "IHDR" + 4B 宽 + 4B 高 → 宽@16、高@20（大端）。
        if (len < 24) return false;
        uint32_t pw = Be32(data + 16);
        uint32_t ph = Be32(data + 20);
        if (pw == 0 || ph == 0 || pw > 0xFFFF || ph > 0xFFFF) return false;
        *w = static_cast<uint16_t>(pw);
        *h = static_cast<uint16_t>(ph);
        return true;
    }

    if (IsJpeg(data, len)) {
        size_t p = 2;  // 跳过 SOI
        while (p + 3 < len) {
            if (data[p] != 0xFF) {
                p++;
                continue;
            }
            uint8_t m = data[p + 1];
            if (m == 0xFF) {  // 填充字节
                p++;
                continue;
            }
            // 无长度独立 marker：TEM(01)、RSTn(D0-D7)、SOI(D8)、EOI(D9)。
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) {
                p += 2;
                continue;
            }
            uint16_t seg = Be16(data + p + 2);  // 段长（大端，含自身 2 字节）
            if (seg < 2) return false;
            // SOFn：C0-CF 中排除 DHT(C4)、JPG(C8)、DAC(CC)。
            bool is_sof = (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC);
            if (is_sof) {
                if (p + 8 >= len) return false;
                uint32_t ph = Be16(data + p + 5);  // 高（大端）
                uint32_t pw = Be16(data + p + 7);  // 宽（大端）
                if (pw == 0 || ph == 0) return false;
                *w = static_cast<uint16_t>(pw);
                *h = static_cast<uint16_t>(ph);
                return true;
            }
            p += 2 + static_cast<size_t>(seg);
        }
        return false;
    }

    return false;
}

uint8_t* DecodeFit(const uint8_t* data, size_t len, uint16_t max_w, uint16_t max_h, uint32_t bg888,
                   uint16_t* out_w, uint16_t* out_h) {
    if (data == nullptr || out_w == nullptr || out_h == nullptr) return nullptr;
    if (max_w == 0 || max_h == 0) return nullptr;
    if (len < 8) return nullptr;

    if (IsPng(data, len)) return DecodePng(data, len, max_w, max_h, bg888, out_w, out_h);
    if (IsJpeg(data, len)) return DecodeJpeg(data, len, max_w, max_h, out_w, out_h);

    ESP_LOGW(TAG, "不支持的图片格式");
    return nullptr;
}

}  // namespace image_decode
