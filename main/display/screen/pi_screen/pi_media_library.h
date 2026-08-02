#pragma once

#include <string>
#include <vector>

#include "media_player/media_player.h"

// ---------------------------------------------------------------------------
// pi_media_library —— SD 曲库的共享扫描/检索。
//
// pi_media（播放器 UI / 续播）与 pi_card_media（LLM 工具）共用，消灭两处各自为政
// 的目录扫描与 ID3 工具复制。核心语义：**本地音乐的播放列表永远是全曲库**（递归
// Music/ + Podcasts/，按「父目录路径 → 文件名」排序，目录成组连播），点哪首从哪首
// 起播——与电台"点一台扩全台"同构。
//
// ID3 策略：ScanLibrary **不读 ID3 不探时长**（几百首逐个开文件要数秒），条目先用
// 文件名/目录名兜底；真实歌名两条回填路：
//   * 播放路径：media_controller 的 OnTrackStarted 惰性补（播到哪首补哪首）；
//   * 搜索路径：RunSearch 逐首 ApplyId3（首次全量慢、进程内缓存后秒回）。
// meta 缓存以 (path → 文件大小) 为有效性判据：Web 后台同名覆盖上传会变 size，自动
// 失效重读。缓存只进不出（≤kLibraryMax 条、每条百字节级，PSRAM 无压力）。
//
// 线程：任意线程可调（缓存内部互斥）。ScanLibrary 是纯 readdir/stat 遍历（无每文件
// ID3），几百首量级在 LVGL 线程同步调用可接受。
// ---------------------------------------------------------------------------
namespace pi_media_library {

// 曲目上限（防病态大库拖死内存/UI；超限按遍历序截断并 ESP_LOGW）。
constexpr size_t kLibraryMax = 500;

// 全量扫描曲库（见文件头注释）。SD 未挂载/无音乐返回空。命中 meta 缓存的条目直接
// 带真实歌名（meta_filled=true），其余 title=去扩展名文件名、subtitle=直接父目录名。
std::vector<media::MediaItem> ScanLibrary();

// items 中 path_or_url == path 的条目下标；找不到返回 -1。
int IndexOfPath(const std::vector<media::MediaItem>& items, const std::string& path);

// 曲库是否至少有一首歌（早退式扫描不建列表——快捷面板「音乐」可用性判定）。
bool HasAnyTrack();

// 为单个条目补 ID3 + 时长（缓存命中免 I/O；否则 media::FillItemMetaFromId3 后回写
// 缓存）。已 meta_filled 的条目 no-op。
void ApplyId3(media::MediaItem& item);

// 文件名去扩展名 / 直接父目录名（原 pi_media.cc 与 pi_card_media.cc 的两份复制收归）。
std::string BaseNoExt(const std::string& path);
std::string ParentDirName(const std::string& path);

}  // namespace pi_media_library
