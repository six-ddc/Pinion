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

// 属性名精确边界匹配：名字前须是行首/':'/','/空白（属性表 ", " 分隔含空格），后须紧跟 '='。
// 否则 find("BANDWIDTH") 会误命中 AVERAGE-BANDWIDTH（前接 '-'，非分隔符，正确排除）。
// 返回属性名后 '=' 的位置（指向 '='），未匹配返回 npos。
size_t FindAttr(const std::string& line, const char* key) {
    const size_t klen = std::strlen(key);
    size_t p = 0;
    for (;;) {
        p = line.find(key, p);
        if (p == std::string::npos) return std::string::npos;
        const char lc = p == 0 ? ':' : line[p - 1];
        const bool left_ok = lc == ':' || lc == ',' || lc == ' ' || lc == '\t';
        const size_t after = p + klen;
        if (left_ok && after < line.size() && line[after] == '=') return after;
        p = after;
    }
}

// 取 #EXT-X-...:ATTR=值 形式里的十进制整数属性（够用：只用于 BANDWIDTH）。
int64_t AttrInt(const std::string& line, const char* key) {
    const size_t eq = FindAttr(line, key);
    if (eq == std::string::npos) return 0;
    return std::strtoll(line.c_str() + eq + 1, nullptr, 10);
}

// 取字符串属性值（到下一个 ',' 或行尾；去掉两侧可能的引号）。用于 #EXT-X-KEY 的 METHOD。
std::string AttrStr(const std::string& line, const char* key) {
    const size_t eq = FindAttr(line, key);
    if (eq == std::string::npos) return "";
    size_t v = eq + 1;
    const bool quoted = v < line.size() && line[v] == '"';
    if (quoted) v++;
    size_t e = v;
    while (e < line.size() && line[e] != (quoted ? '"' : ',')) e++;
    return line.substr(v, e - v);
}

}  // namespace

std::string JoinUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return base;
    if (StartsWith(ref, "http://") || StartsWith(ref, "https://")) return ref;
    // base 的 query 不参与拼接
    std::string b = base.substr(0, base.find('?'));
    const size_t scheme = b.find("://");
    if (scheme == std::string::npos) return ref;  // base 畸形：原样返回 ref
    if (StartsWith(ref, "//")) return b.substr(0, scheme) + ":" + ref;  // 协议相对：接 base 的 scheme
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
            } else if (StartsWith(line, "#EXT-X-MAP")) {
                pl.unsupported = true;  // fMP4 分片：本解封装只吃 TS，不支持
            } else if (StartsWith(line, "#EXT-X-KEY")) {
                std::string method = AttrStr(line, "METHOD");
                if (method != "NONE") pl.unsupported = true;  // 加密分片：不支持
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
            if (abort_.load() || fatal_) return -1;  // abort 或不支持特性等致命错误
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
            // 2) 拉下一片（内部带刷新/重连/退避；失败返回 false = 彻底放弃）
            if (!NextSegment()) return -1;
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
            if (pl.is_master || pl.segment_urls.empty()) {  // 两层 master：不支持
                ESP_LOGW(TAG, "nested master or empty variant playlist, unsupported");
                return false;
            }
        }
        if (CheckUnsupported(pl)) return false;  // #EXT-X-MAP/加密：置 fatal
        if (pl.endlist) {  // 本产品只播直播电台：ENDLIST 视同网络失败，不按点播 EOF
            ESP_LOGW(TAG, "playlist has ENDLIST (treated as live failure, not VOD)");
            return false;
        }
        if (pl.segment_urls.empty()) return false;
        media_url_ = std::move(media_url);
        playlist_ = std::move(pl);
        // 直播起点：倒数 kLiveEdgeBack 片。
        const int64_t last = playlist_.media_sequence + (int64_t)playlist_.segment_urls.size();
        next_seq_ = std::max(playlist_.media_sequence, last - kLiveEdgeBack);
        demux_.Reset();
        // 新会话：清停滞检测，别让重连继承旧的 60s 停滞计时。
        last_window_first_ = -1;
        last_window_end_ = -1;
        window_advance_us_ = 0;
        // 日志一律避开 %lld/%zu：真机 newlib-nano 不认，变参错位会把后续 %s 当指针解引用
        // 直接崩（实测 MTVAL 里全是 URL 文本）。序号打印取低位够诊断用。
        ESP_LOGI(TAG, "hls resolved: %d segs, target %ds, seq %u", (int)playlist_.segment_urls.size(),
                 playlist_.target_duration_s, (unsigned)next_seq_);
        return true;
    }

    // 识别不支持特性（#EXT-X-MAP fMP4 / 加密 #EXT-X-KEY）：命中即 LOGE + 置实例级 fatal。
    bool CheckUnsupported(const HlsPlaylist& pl) {
        if (!pl.unsupported) return false;
        ESP_LOGE(TAG, "unsupported HLS playlist (fMP4 EXT-X-MAP or encrypted EXT-X-KEY)");
        fatal_ = true;
        return true;
    }

    // 确保 playlist_ 覆盖 next_seq_：不够则按 target duration 节奏刷新；落后/超前窗口跳直播沿。
    // 返回 false = 刷新失败（网络层面或直播沿停滞），交给 NextSegment 失败路径。
    bool RefreshUntilHasNext() {
        for (;;) {
            const int64_t first = playlist_.media_sequence;
            const int64_t end = first + (int64_t)playlist_.segment_urls.size();
            // 直播沿停滞兜底：窗口连续刷新不前进累计 ≥60s 即放弃，交给整链重解析。
            const int64_t now_us = esp_timer_get_time();
            if (first > last_window_first_ || end > last_window_end_) {
                last_window_first_ = first;
                last_window_end_ = end;
                window_advance_us_ = now_us;  // 窗口前进：重置停滞计时
            } else if (window_advance_us_ != 0 && (now_us - window_advance_us_) / 1000 >= kGiveUpAfterMs) {
                ESP_LOGW(TAG, "live edge stalled %dms, bail to reconnect",
                         (int)((now_us - window_advance_us_) / 1000));
                return false;
            }
            // 落后滑窗（暂停过久/服务端重启）或超前（服务端序号重置）：跳回直播沿，丢中断的 PES
            if (next_seq_ < first || next_seq_ > end) {
                ESP_LOGW(TAG, "seq %u out of window [%u,%u), rejoin edge", (unsigned)next_seq_,
                         (unsigned)first, (unsigned)end);
                next_seq_ = std::max(first, end - kLiveEdgeBack);
                demux_.Reset();
            }
            if (next_seq_ < end) return true;
            if (playlist_.endlist) {  // 本产品只播直播电台：ENDLIST 视同网络失败
                ESP_LOGW(TAG, "playlist gained ENDLIST (treated as live failure, not VOD)");
                return false;
            }
            // 追上直播沿：睡半个 target（分片轮询 abort）再刷新。下限 1000ms 防 TARGETDURATION
            // 缺失/0 时 busy 轮询打爆服务器。
            const int wait_ms = std::max(1000, playlist_.target_duration_s * 1000 / 2);
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
            if (CheckUnsupported(pl)) return false;  // 刷新后出现不支持特性：置 fatal
            if (pl.segment_urls.empty()) return false;
            playlist_ = std::move(pl);
        }
    }

    // 拉取序号 next_seq_ 的分片 → 解封装进 adts_。任何网络/HTTP 失败走整链重连
    //（退避 + 60s 放弃 + on_reconnecting 上报）。返回 false = 彻底放弃或 abort。
    bool NextSegment() {
        for (;;) {
            if (abort_.load() || fatal_) return false;  // fatal 直接放弃，不进退避
            bool ok = RefreshUntilHasNext();
            if (fatal_) return false;  // 刷新中命中不支持特性
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
                    attempt_ = 0;  // 分片拉取成功：清退避计数，避免恢复后从 15s 起跳
                    next_seq_++;
                    adts_.clear();
                    adts_pos_ = 0;
                    demux_.Feed((const uint8_t*)seg.data(), seg.size(), adts_);
                    if (adts_.empty() && !demux_.found_unsupported()) {
                        ESP_LOGW(TAG, "segment yielded no audio (%uB ts)", (unsigned)seg.size());
                        if (++empty_streak_ >= 3) {  // 连续 3 片零音频：置 fatal 放弃
                            ESP_LOGE(TAG, "3 consecutive empty segments, giving up");
                            fatal_ = true;
                            return false;
                        }
                        continue;  // 空片：直接试下一片
                    }
                    empty_streak_ = 0;  // 任一分片产出音频即清零
                    return true;
                }
                ESP_LOGW(TAG, "segment fetch failed (http %d), seq %u", status, (unsigned)next_seq_);
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
    bool fatal_ = false;                          // 不支持特性/连续空片等致命错误：Read 返 -1
    TsDemux demux_;
    std::vector<uint8_t> adts_;                   // 当前分片解出的 ADTS 缓冲
    size_t adts_pos_ = 0;
    std::atomic<bool> abort_{false};
    int64_t fail_streak_start_us_ = 0;            // 0 = 当前无失败连续段
    int attempt_ = 0;
    int empty_streak_ = 0;                        // 连续零音频分片计数（产出音频即清零）
    int64_t last_window_first_ = -1;              // 上次见到的窗口 [first,end)，用于停滞检测
    int64_t last_window_end_ = -1;
    int64_t window_advance_us_ = 0;               // 窗口最近一次前进的时刻（0 = 未初始化）
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
