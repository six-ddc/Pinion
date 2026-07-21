// radio_stations.h — 内置网络电台表（全 HLS/m3u8，AAC-LC 192–258kbps）。
//
// 每台 = 名称 + 分组 + m3u8 URL（media_pump 按 .m3u8 后缀路由到 HLS 源 + AAC 解码，
// 见 media_hls.h；master→variant 一跳与防盗链 token 均已支持）。三个网关：
//   - ngcdn0xx.cnr.cn      央广直连（单层 media playlist，10s 分片，192k）
//   - sk.cri.cn            国广 CRI（258k）
//   - satellitepull.cnr.cn 省市台卫星拉流（master+token 会话，3s 分片，194k）
// 旧蜻蜓 lhttp 64kbps MP3 表已整体废弃（源质量太差，编码毛刺固有）。
// 表内每一台都经实测通过（playlist→分片→ffprobe AAC 核验，2026-07-22）。
// header-only constexpr 表，device/sim 同源，无 .cc、无链接项。
//
// 用途：media 工具 mode:"radio" 列表 / mode:"play" 起播；station 的稳定标识是它在
// 本表中的下标（kRadioStations 的 index），play 用 station_indices 引用它。
#pragma once

#include <cstddef>

namespace media {

struct RadioStation {
    const char* name;   // 电台名（展示 + 模糊匹配）
    const char* genre;  // 分组：新闻 / 交通 / 音乐 / 综合
    const char* url;    // HLS m3u8 URL
};

// 32 台，覆盖 央广·CRI/主要省市 的 新闻·交通·音乐·综合。全部实测通过。
inline constexpr RadioStation kRadioStations[] = {
    // —— 新闻 ——
    {"中国之声", "新闻", "https://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"经济之声", "新闻", "https://ngcdn002.cnr.cn/live/jjzs/index.m3u8"},
    {"环球资讯广播", "新闻", "https://sk.cri.cn/905.m3u8"},
    {"北京新闻广播", "新闻", "https://satellitepull.cnr.cn/live/wxbjxwgb/playlist.m3u8"},
    {"上海新闻广播", "新闻", "https://satellitepull.cnr.cn/live/wx32shrmgb/playlist.m3u8"},
    {"江苏新闻广播", "新闻", "https://satellitepull.cnr.cn/live/wx32jsxwgb/playlist.m3u8"},
    {"重庆新闻广播", "新闻", "https://satellitepull.cnr.cn/live/wxcqxwgb/playlist.m3u8"},
    {"第一财经广播", "新闻", "https://satellitepull.cnr.cn/live/wx32dycjgb/playlist.m3u8"},
    // —— 交通 ——
    {"江苏交通广播", "交通", "https://satellitepull.cnr.cn/live/wx32jsjtgb/playlist.m3u8"},
    {"浙江交通之声", "交通", "https://satellitepull.cnr.cn/live/wxzjjtgb/playlist.m3u8"},
    {"羊城交通广播", "交通", "https://satellitepull.cnr.cn/live/wxgdycjtt/playlist.m3u8"},
    {"深圳交通频率", "交通", "https://satellitepull.cnr.cn/live/wxszjjpl/playlist.m3u8"},
    {"四川交通广播", "交通", "https://satellitepull.cnr.cn/live/wxscjtgb/playlist.m3u8"},
    {"山东交通广播", "交通", "https://satellitepull.cnr.cn/live/wxsdjtgb/playlist.m3u8"},
    {"湖南交通广播", "交通", "https://satellitepull.cnr.cn/live/wx32hunjtgb/playlist.m3u8"},
    {"安徽交通广播", "交通", "https://satellitepull.cnr.cn/live/wxahjtgb/playlist.m3u8"},
    {"福建交通广播", "交通", "https://satellitepull.cnr.cn/live/wx32fjdnjtgb/playlist.m3u8"},
    {"江西交通广播", "交通", "https://satellitepull.cnr.cn/live/wx32jiangxjtgb/playlist.m3u8"},
    // —— 音乐 ——
    {"深圳飞扬971", "音乐", "https://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8"},
    {"广东音乐之声", "音乐", "https://satellitepull.cnr.cn/live/wxgdyyzs/playlist.m3u8"},
    {"江苏音乐广播", "音乐", "https://satellitepull.cnr.cn/live/wx32jsyygb/playlist.m3u8"},
    {"山东音乐广播", "音乐", "https://satellitepull.cnr.cn/live/wxsdyygb/playlist.m3u8"},
    {"重庆音乐广播", "音乐", "https://satellitepull.cnr.cn/live/wxcqyygb/playlist.m3u8"},
    {"陕西音乐广播", "音乐", "https://satellitepull.cnr.cn/live/wxsxxyygb/playlist.m3u8"},
    {"安徽音乐广播", "音乐", "https://satellitepull.cnr.cn/live/wxahyygb/playlist.m3u8"},
    {"湖南潇湘之声", "音乐", "https://satellitepull.cnr.cn/live/wx32hunyygb/playlist.m3u8"},
    {"浙江悦动之音", "音乐", "https://satellitepull.cnr.cn/live/wxzj968/playlist.m3u8"},
    // —— 综合 ——
    {"上海东方广播", "综合", "https://satellitepull.cnr.cn/live/wx32dfgbdt/playlist.m3u8"},
    {"浙江之声", "综合", "https://satellitepull.cnr.cn/live/wxzjzs/playlist.m3u8"},
    {"湖北之声", "综合", "https://satellitepull.cnr.cn/live/wx32hubzsgb/playlist.m3u8"},
    {"湖南金鹰之声", "综合", "https://satellitepull.cnr.cn/live/wx32955/playlist.m3u8"},
    {"珠江经济台", "综合", "https://satellitepull.cnr.cn/live/wxgdzjjjt/playlist.m3u8"},
};

inline constexpr size_t kRadioStationCount = sizeof(kRadioStations) / sizeof(kRadioStations[0]);

}  // namespace media
