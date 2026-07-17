// media_http_stream — 网络电台 MP3 流字节源（双端各自实现，接口与 media_source 一致）。
//   真机: components/media_player/src/media_http_esp.cc  (esp_http_client 流式)
//   sim : sim/shim/src/media_http_curl.cc               (libcurl 流式)
//
// 电台流实测特性（如 http://lhttp.qtfm.cn/live/<id>/64k.mp3）：chunked 无限 MP3 流，
// 64kbps 立体声，无防盗链。实现约束：不发 Icy-MetaData 头；不设总超时（会杀无限流），
// 只设 socket idle 超时（~2-3s，为跨线程 Abort 的响应性让路，见 media_source.h::Abort）；
// 不发 Connection: close（保持长连）。内部自带断线重连 + 指数退避（1s/2s/4s/.../封顶
// 15s，无限重试直到 Abort 或连续失败达 60s 才放弃），重连期间 Read 阻塞（不返回 0，
// 避免被误判 EOF），彻底失败（60s 耗尽）返回 <0。
#pragma once

#include <functional>
#include <memory>

#include "media_player/media_source.h"

namespace media {

// 打开网络流源。url 支持 http/https（https 走证书包）。失败/首连不上返回 nullptr。
// on_reconnecting：连接掉线开始重连时以 true 调用一次，重连成功恢复数据流时以 false
// 调用一次（用于上层把可见状态短暂切到"缓冲中"）。可能从内部线程调用，须非阻塞；
// 传 nullptr 表示不关心。
std::unique_ptr<MediaSource> OpenHttpStreamSource(
    const char* url, std::function<void(bool reconnecting)> on_reconnecting = nullptr);

}  // namespace media
