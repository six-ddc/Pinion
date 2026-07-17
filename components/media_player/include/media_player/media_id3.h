#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// media_id3 —— 最小 ID3v2 流式解析器（Stage E）。
//
// 只 fseek/fread tag 区（10 字节头 + 声明的 tag_size），不载入整个 MP3；文本帧
// 体过大时只读前 4KB 用于解码、其余用 fseek 跳过。支持 ID3v2.3（frame size 为
// 普通 32 位大端数）与 ID3v2.4（frame size 为 syncsafe 7-bit 编码）；v2.2（3 字符
// 帧 id、无独立版本）不支持，直接返回空结果。
//
// 安全契约：每个 fread/fseek 的返回值与偏移量都做边界校验——畸形/被截断/被
// 篡改的 tag 只会让解析提前结束（返回已解出的部分或全空），绝不越界读写、
// 绝不死循环、绝不抛异常。校验方式：帧头 4 字节 id 必须是大写字母/数字；帧
// 声明的 size 必须 <= tag 区剩余字节；每轮循环至少消耗 10 字节帧头，结构上
// 保证有限步终止。
//
// 平台无关（纯 fopen/fread/fseek + media_textconv 转码 facade），components/
// media_player 不引入任何 UI 依赖。
// ---------------------------------------------------------------------------
namespace media_id3 {

struct Tags {
    std::string title;   // TIT2，UTF-8
    std::string album;   // TALB，UTF-8
    std::string artist;  // TPE1，UTF-8
    bool has_any = false;  // 至少命中一个上述帧（区别于"文件无 ID3 头"）
};

// 读取 path 的 ID3v2 文本标签。无 ID3 头 / 解析失败 / 只有 v2.2 头都返回
// has_any=false 的空 Tags（调用方据此回退文件名/目录名，不当错误处理）。
Tags ReadTags(const std::string& path);

// 读取内嵌 APIC 封面的原始编码字节（JPEG/PNG 原始码流，未解码）。找不到
// APIC / 尺寸超过 kMaxCoverBytes（4MB，防御性上限）/ 解析失败均返回 nullptr。
// 成功时返回 malloc 的缓冲区（调用方 free），*out_size 为字节数，*out_mime
// 为 APIC 帧里声明的 MIME（不可信，PeekImageSize 用魔数自行判定真实格式）。
uint8_t* ReadCover(const std::string& path, size_t* out_size, std::string* out_mime);

// 只扫 JPEG/PNG 头部 marker 取像素尺寸，不解码像素。失败（未知格式/数据
// 不足/损坏）返回 false。JPEG 只识别 baseline/progressive SOF（不含无损/
// 算术编码变体——覆盖绝大多数真实封面）。
bool PeekImageSize(const uint8_t* data, size_t len, int* out_w, int* out_h);

}  // namespace media_id3
