// media_hls — 实现（范围/取向见头文件）。
#include "media_hls.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

#include "esp_log.h"
#include "esp_timer.h"
#include "media_http_fetch.h"
#include "media_ts_demux.h"

namespace media {
namespace {

const char* TAG = "media_hls";

constexpr size_t kPlaylistMaxBytes = 256 * 1024;      // playlist 文本上限（实测数 KB）
constexpr size_t kSegmentMaxBytes = 4 * 1024 * 1024;  // 单分片上限（10s 320kbps 也才 ~400KB）
constexpr int kLiveEdgeBack = 3;                      // 直播起点：倒数第 3 片（HLS 惯例，抗滑窗追尾）
constexpr int kBackoffBaseMs = 1000;                  // 指数退避：1s/2s/4s/8s/封顶 15s
constexpr int kBackoffMaxMs = 15000;
constexpr int kGiveUpAfterMs = 60000;                 // 连续失败累计超此时长放弃（对齐 media_http_esp）
constexpr int kPollSliceMs = 200;                     // 各类等待的分片粒度（abort 响应性）

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// 取 #EXT-X-KEY:...,ATTR=值 形式里的十进制整数属性（够用：只用于 BANDWIDTH）。
int64_t AttrInt(const std::string& line, const char* key) {
    size_t p = line.find(key);
    if (p == std::string::npos) return 0;
    p += std::strlen(key);
    if (p >= line.size() || line[p] != '=') return 0;
    return std::strtoll(line.c_str() + p + 1, nullptr, 10);
}

}  // namespace

std::string JoinUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return base;
    if (StartsWith(ref, "http://") || StartsWith(ref, "https://")) return ref;
    // base 的 query 不参与拼接
    std::string b = base.substr(0, base.find('?'));
    const size_t scheme = b.find("://");
    if (scheme == std::string::npos) return ref;  // base 畸形：原样返回 ref
    if (ref[0] == '/') {
        const size_t host_end = b.find('/', scheme + 3);
        return (host_end == std::string::npos ? b : b.substr(0, host_end)) + ref;
    }
    const size_t last_slash = b.rfind('/');
    if (last_slash <= scheme + 2) return b + "/" + ref;  // 无路径：host 后直接接
    return b.substr(0, last_slash + 1) + ref;
}

bool UrlIsHls(const char* url) {
    if (url == nullptr) return false;
    std::string path(url);
    path = path.substr(0, path.find('?'));
    if (path.size() < 5) return false;
    std::string tail = path.substr(path.size() - 5);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
    return tail == ".m3u8";
}

HlsPlaylist ParseM3u8(const std::string& text, const std::string& base_url) {
    HlsPlaylist pl;
    bool next_is_variant = false;
    int64_t pending_bandwidth = 0;
    bool next_is_segment = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (StartsWith(line, "#EXT-X-STREAM-INF:")) {
                pl.is_master = true;
                next_is_variant = true;
                pending_bandwidth = AttrInt(line, "BANDWIDTH");
            } else if (StartsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
                pl.media_sequence = std::strtoll(line.c_str() + 22, nullptr, 10);
            } else if (StartsWith(line, "#EXT-X-TARGETDURATION:")) {
                pl.target_duration_s = (int)std::strtol(line.c_str() + 22, nullptr, 10);
            } else if (StartsWith(line, "#EXT-X-ENDLIST")) {
                pl.endlist = true;
            } else if (StartsWith(line, "#EXTINF:")) {
                next_is_segment = true;
            }
            continue;
        }
        // 非注释行：URI（归属取决于前一条 tag）
        if (next_is_variant) {
            next_is_variant = false;
            pl.variants.push_back({JoinUrl(base_url, line), pending_bandwidth});
        } else if (next_is_segment) {
            next_is_segment = false;
            pl.segment_urls.push_back(JoinUrl(base_url, line));
        }
    }
    return pl;
}

namespace {

// —— HLS 直播字节源：Read() 驱动的状态机（解析/拉片/解封装全在 reader 线程内） ——
class HlsStreamSource : public MediaSource {
 public:
    HlsStreamSource(const char* url, std::function<void(bool)> on_reconnecting)
        : station_url_(url ? url : ""), on_reconnecting_(std::move(on_reconnecting)) {}
    ~HlsStreamSource() override { Close(); }

    bool Open() { return Resolve(); }

    int Read(uint8_t* buf, size_t max) override {
        if (buf == nullptr || max == 0) return -1;
        for (;;) {
            if (abort_.load()) return -1;
            // 1) 缓冲有货直接吐
            if (adts_pos_ < adts_.size()) {
                const size_t n = std::min(max, adts_.size() - adts_pos_);
                std::memcpy(buf, adts_.data() + adts_pos_, n);
                adts_pos_ += n;
                return (int)n;
            }
            if (demux_.found_unsupported()) {
                ESP_LOGE(TAG, "unsupported audio stream_type 0x%02X in TS", demux_.unsupported_stream_type());
                return -1;
            }
            if (ended_) return 0;  // ENDLIST 且已吐完：曲末 EOF
            // 2) 拉下一片（内部带刷新/重连/退避；失败返回 false = 彻底放弃）
            if (!NextSegment()) return abort_.load() || !ended_ ? -1 : 0;
        }
    }

    void Abort() override { abort_ = true; }
    void Close() override { abort_ = true; }
    bool IsStream() const override { return true; }

 private:
    // 整链解析：台址 →（master 则选最高带宽变体）→ media playlist → 定位直播起点。
    // 成功后 media_url_/playlist_/next_seq_ 就绪。失败返回 false（不含退避，由调用方裹）。
    bool Resolve() {
        int status = 0;
        std::string text;
        if (!HttpGetToString(station_url_.c_str(), kPlaylistMaxBytes, abort_, &status, &text) || status != 200) {
            ESP_LOGW(TAG, "playlist fetch failed (http %d): %s", status, station_url_.c_str());
            return false;
        }
        HlsPlaylist pl = ParseM3u8(text, station_url_);
        std::string media_url = station_url_;
        if (pl.is_master) {
            if (pl.variants.empty()) return false;
            const auto best = std::max_element(pl.variants.begin(), pl.variants.end(),
                                               [](const auto& a, const auto& b) { return a.bandwidth < b.bandwidth; });
            media_url = best->url;
            if (!HttpGetToString(media_url.c_str(), kPlaylistMaxBytes, abort_, &status, &text) || status != 200) {
                ESP_LOGW(TAG, "variant fetch failed (http %d)", status);
                return false;
            }
            pl = ParseM3u8(text, media_url);
            if (pl.is_master || pl.segment_urls.empty()) return false;  // 两层 master：不支持
        }
        if (pl.segment_urls.empty()) return false;
        media_url_ = std::move(media_url);
        playlist_ = std::move(pl);
        // 直播起点：倒数 kLiveEdgeBack 片；点播（ENDLIST）从头播。
        const int64_t last = playlist_.media_sequence + (int64_t)playlist_.segment_urls.size();
        next_seq_ = playlist_.endlist
                        ? playlist_.media_sequence
                        : std::max(playlist_.media_sequence, last - kLiveEdgeBack);
        demux_.Reset();
        ESP_LOGI(TAG, "hls resolved: %d segs, target %ds, seq %lld%s", (int)playlist_.segment_urls.size(),
                 playlist_.target_duration_s, (long long)next_seq_, playlist_.endlist ? " (vod)" : "");
        return true;
    }

    // 确保 playlist_ 覆盖 next_seq_：不够则按 target duration 节奏刷新；落后滑窗则跳直播沿。
    // 只在直播（非 endlist）时刷新。返回 false = 刷新失败（网络层面）。
    bool RefreshUntilHasNext() {
        for (;;) {
            const int64_t first = playlist_.media_sequence;
            const int64_t end = first + (int64_t)playlist_.segment_urls.size();
            if (next_seq_ < first) {  // 落后滑窗（暂停过久/服务端重启）：跳回直播沿，丢中断的 PES
                ESP_LOGW(TAG, "behind live window (seq %lld < %lld), rejoin edge", (long long)next_seq_,
                         (long long)first);
                next_seq_ = std::max(first, end - kLiveEdgeBack);
                demux_.Reset();
            }
            if (next_seq_ < end) return true;
            if (playlist_.endlist) {
                ended_ = true;
                return true;
            }
            // 追上直播沿：睡半个 target（分片轮询 abort）再刷新
            const int wait_ms = std::max(1, playlist_.target_duration_s * 1000 / 2);
            for (int waited = 0; waited < wait_ms; waited += kPollSliceMs) {
                if (abort_.load()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollSliceMs));
            }
            int status = 0;
            std::string text;
            if (!HttpGetToString(media_url_.c_str(), kPlaylistMaxBytes, abort_, &status, &text) || status != 200) {
                ESP_LOGW(TAG, "playlist refresh failed (http %d)", status);
                return false;
            }
            HlsPlaylist pl = ParseM3u8(text, media_url_);
            if (pl.segment_urls.empty()) return false;
            playlist_ = std::move(pl);
        }
    }

    // 拉取序号 next_seq_ 的分片 → 解封装进 adts_。任何网络/HTTP 失败走整链重连
    //（退避 + 60s 放弃 + on_reconnecting 上报）。返回 false = 彻底放弃或 abort。
    bool NextSegment() {
        for (;;) {
            if (abort_.load()) return false;
            bool ok = RefreshUntilHasNext();
            if (ok && ended_) return true;  // ENDLIST 榨干：Read 返回 0
            if (ok) {
                const size_t idx = (size_t)(next_seq_ - playlist_.media_sequence);
                const std::string& seg_url = playlist_.segment_urls[idx];
                int status = 0;
                std::string seg;
                ok = HttpGetToString(seg_url.c_str(), kSegmentMaxBytes, abort_, &status, &seg) && status == 200;
                if (ok) {
                    if (fail_streak_start_us_ != 0) {  // 从重连中恢复
                        fail_streak_start_us_ = 0;
                        if (on_reconnecting_) on_reconnecting_(false);
                    }
                    next_seq_++;
                    adts_.clear();
                    adts_pos_ = 0;
                    demux_.Feed((const uint8_t*)seg.data(), seg.size(), adts_);
                    if (adts_.empty() && !demux_.found_unsupported()) {
                        ESP_LOGW(TAG, "segment yielded no audio (%zuB ts)", seg.size());
                        continue;  // 空片：直接试下一片
                    }
                    return true;
                }
                ESP_LOGW(TAG, "segment fetch failed (http %d), seq %lld", status, (long long)next_seq_);
            }
            // —— 失败：退避 + 整链重解析（token 过期换新会话）——
            if (fail_streak_start_us_ == 0) {
                fail_streak_start_us_ = esp_timer_get_time();
                if (on_reconnecting_) on_reconnecting_(true);
            }
            const int64_t elapsed_ms = (esp_timer_get_time() - fail_streak_start_us_) / 1000;
            if (elapsed_ms > kGiveUpAfterMs) {
                ESP_LOGE(TAG, "hls reconnect giving up after %dms", (int)elapsed_ms);
                return false;
            }
            int backoff = kBackoffBaseMs << std::min(attempt_, 4);
            if (backoff > kBackoffMaxMs) backoff = kBackoffMaxMs;
            for (int waited = 0; waited < backoff; waited += kPollSliceMs) {
                if (abort_.load()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollSliceMs));
            }
            attempt_++;
            if (Resolve()) attempt_ = 0;  // 重解析成功：下一轮拉新 next_seq_
        }
    }

    std::string station_url_;                     // 原始台址（重连整链解析的起点）
    std::function<void(bool)> on_reconnecting_;
    std::string media_url_;                       // media playlist URL（刷新/相对拼接基准，含 token）
    HlsPlaylist playlist_;
    int64_t next_seq_ = 0;                        // 下一个要拉的分片序号
    bool ended_ = false;                          // ENDLIST 且已榨干
    TsDemux demux_;
    std::vector<uint8_t> adts_;                   // 当前分片解出的 ADTS 缓冲
    size_t adts_pos_ = 0;
    std::atomic<bool> abort_{false};
    int64_t fail_streak_start_us_ = 0;            // 0 = 当前无失败连续段
    int attempt_ = 0;
};

}  // namespace

std::unique_ptr<MediaSource> OpenHlsStreamSource(const char* url,
                                                 std::function<void(bool reconnecting)> on_reconnecting) {
    if (url == nullptr || url[0] == '\0') return nullptr;
    auto src = std::unique_ptr<HlsStreamSource>(new HlsStreamSource(url, std::move(on_reconnecting)));
    if (!src->Open()) return nullptr;
    return src;
}

}  // namespace media
