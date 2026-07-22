// media_hls — HLS(m3u8) 直播电台客户端：playlist 解析（纯函数，宿主可单测）+
// HlsStreamSource 字节源（MediaSource 实现，Read() 吐 TS 解封装后的 ADTS AAC 字节流）。
// 可移植（device/sim 同源）；网络经 media_http_fetch 分端原语，解封装经 media_ts_demux。
//
// 支持范围（国内电台 HLS 实测形态，见 sim/ts_demux_test.cc 的 fixture 说明）：
//   - master playlist 一跳（#EXT-X-STREAM-INF，选最高 BANDWIDTH）→ media playlist；
//   - 直播滑窗：MEDIA-SEQUENCE 追踪、耗尽后按 TARGETDURATION 节奏刷新、落后/超前窗口跳回
//     直播沿；本产品 HLS 仅用于直播电台，#EXT-X-ENDLIST 不按点播 EOF 处理，而是视同网络
//     失败走整链重解析+退避（避免"曲末"自动换台成别的台）；
//   - 不支持特性显式报错：#EXT-X-MAP（fMP4）、#EXT-X-KEY（METHOD≠NONE 的加密）识别即置
//     不支持标志，实例转 fatal（Read 返 -1），不进无声无限拉片；
//   - URL：绝对透传/相对拼接（相对 media playlist 自身 URL），query **逐字节保留不重编码**
//     ——CNR satellitepull 的 wsSession 防盗链 token 挂在 query 上（实测红线）；
//   - 4xx/网络失败：从原始台址整链重解析（token 过期换新会话），指数退避 1s..15s 封顶、
//     连续失败 60s 放弃，语义对齐 media_http_esp。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "media_player/media_source.h"

namespace media {

// —— 纯解析层（无网络，ts_demux_test 单测） ——

struct HlsPlaylist {
    bool is_master = false;
    // master 才有：按出现序的变体（url 已 JoinUrl 成绝对）
    struct Variant {
        std::string url;
        int64_t bandwidth = 0;
    };
    std::vector<Variant> variants;
    // media 才有：
    int64_t media_sequence = 0;             // 首分片序号（#EXT-X-MEDIA-SEQUENCE，缺省 0）
    int target_duration_s = 0;              // #EXT-X-TARGETDURATION
    bool endlist = false;                   // #EXT-X-ENDLIST（本产品视同直播停播，非点播 EOF）
    bool unsupported = false;               // 含 #EXT-X-MAP（fMP4）或加密 #EXT-X-KEY：不支持
    std::vector<std::string> segment_urls;  // 分片 URL（已 JoinUrl 成绝对，序号 = media_sequence + 下标）
};

// 解析 m3u8 文本；base_url 为该 playlist 自身的 URL（相对引用的拼接基准）。
HlsPlaylist ParseM3u8(const std::string& text, const std::string& base_url);

// RFC3986 的实用子集：ref 带 scheme 则透传；以 '//' 开头（协议相对）则接 base 的 scheme；
// 以 '/' 开头则接 base 的 scheme+host；否则接 base 的目录（base 的 query 先剥掉再找最后一个
// '/'）。ref 的 query 原样保留。
std::string JoinUrl(const std::string& base, const std::string& ref);

// URL 的 path 部分（剥 query 后）以 .m3u8 结尾（大小写不敏感）→ 走 HLS 源 + AAC 解码。
bool UrlIsHls(const char* url);

// —— 字节源工厂（media_pump reader 调用；语义同 OpenHttpStreamSource） ——
// 同步完成首次解析（master→media 一跳 + 首个分片定位），失败返回 nullptr。
// on_reconnecting 语义同 media_http_stream.h。
std::unique_ptr<MediaSource> OpenHlsStreamSource(const char* url,
                                                 std::function<void(bool reconnecting)> on_reconnecting);

}  // namespace media
