// radio_stations.h — 内置网络电台表（蜻蜓 FM MP3 直播流）。
//
// 每台 = 名称 + 分组 + 流 URL。URL 一律用 http（省 TLS 握手/内存；蜻蜓 lhttp 网关
// 明文可用）。表内每一台都经 curl 实测通过（HTTP 200 + content-type audio/mpeg），
// 见 Stage B 交付说明。header-only constexpr 表，device/sim 同源，无 .cc、无链接项。
//
// 用途：media 工具 mode:"radio" 列表 / mode:"play" 起播；station 的稳定标识是它在
// 本表中的下标（kRadioStations 的 index），play 用 station_indices 引用它。
#pragma once

#include <cstddef>

namespace media {

struct RadioStation {
    const char* name;   // 电台名（展示 + 模糊匹配）
    const char* genre;  // 分组：新闻 / 交通 / 音乐 / 综合
    const char* url;    // MP3 直播流 URL（http，蜻蜓 lhttp 网关）
};

// 32 台，覆盖 央广/主要省市 的 新闻·交通·音乐·综合。全部实测通过。
inline constexpr RadioStation kRadioStations[] = {
    // —— 新闻 ——
    {"中国之声", "新闻", "http://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"央广国际新闻", "新闻", "http://lhttp.qtfm.cn/live/20500172/64k.mp3"},
    {"北京新闻广播", "新闻", "http://lhttp.qtfm.cn/live/339/64k.mp3"},
    {"上海新闻广播", "新闻", "http://lhttp.qtfm.cn/live/270/64k.mp3"},
    {"广东新闻广播", "新闻", "http://lhttp.qtfm.cn/live/1254/64k.mp3"},
    {"深圳新闻广播", "新闻", "http://lhttp.qtfm.cn/live/1270/64k.mp3"},
    {"四川新闻广播", "新闻", "http://lhttp.qtfm.cn/live/4906/64k.mp3"},
    {"武汉新闻广播", "新闻", "http://lhttp.qtfm.cn/live/20198/64k.mp3"},
    {"第一财经", "新闻", "http://lhttp.qtfm.cn/live/276/64k.mp3"},
    {"上海东广新闻资讯", "新闻", "http://lhttp.qtfm.cn/live/275/64k.mp3"},
    // —— 交通 ——
    {"北京交通广播", "交通", "http://lhttp.qtfm.cn/live/336/64k.mp3"},
    {"广东交通之声", "交通", "http://lhttp.qtfm.cn/live/1262/64k.mp3"},
    {"上海交通广播", "交通", "http://lhttp.qtfm.cn/live/266/64k.mp3"},
    {"深圳交通广播", "交通", "http://lhttp.qtfm.cn/live/1272/64k.mp3"},
    {"四川交通广播", "交通", "http://lhttp.qtfm.cn/live/4886/64k.mp3"},
    {"安徽交通广播", "交通", "http://lhttp.qtfm.cn/live/1949/64k.mp3"},
    {"湖北楚天交通广播", "交通", "http://lhttp.qtfm.cn/live/1291/64k.mp3"},
    {"广州经济交通广播", "交通", "http://lhttp.qtfm.cn/live/4955/64k.mp3"},
    // —— 音乐 ——
    {"北京音乐广播", "音乐", "http://lhttp.qtfm.cn/live/332/64k.mp3"},
    {"上海音乐广播", "音乐", "http://lhttp.qtfm.cn/live/273/64k.mp3"},
    {"广东音乐之声", "音乐", "http://lhttp.qtfm.cn/live/1260/64k.mp3"},
    {"深圳音乐广播", "音乐", "http://lhttp.qtfm.cn/live/1271/64k.mp3"},
    {"四川音乐广播", "音乐", "http://lhttp.qtfm.cn/live/1110/64k.mp3"},
    {"清晨音乐台", "音乐", "http://lhttp.qtfm.cn/live/4915/64k.mp3"},
    {"上海动感101", "音乐", "http://lhttp.qtfm.cn/live/274/64k.mp3"},
    {"上海经典音乐广播", "音乐", "http://lhttp.qtfm.cn/live/267/64k.mp3"},
    {"厦门音乐广播", "音乐", "http://lhttp.qtfm.cn/live/1739/64k.mp3"},
    {"安徽音乐广播", "音乐", "http://lhttp.qtfm.cn/live/1947/64k.mp3"},
    // —— 综合 ——
    {"广东珠江经济台", "综合", "http://lhttp.qtfm.cn/live/1259/64k.mp3"},
    {"北京文艺广播", "综合", "http://lhttp.qtfm.cn/live/333/64k.mp3"},
    {"上海戏剧曲艺广播", "综合", "http://lhttp.qtfm.cn/live/269/64k.mp3"},
    {"厦门闽南之声", "综合", "http://lhttp.qtfm.cn/live/1740/64k.mp3"},
};

inline constexpr size_t kRadioStationCount = sizeof(kRadioStations) / sizeof(kRadioStations[0]);

}  // namespace media
