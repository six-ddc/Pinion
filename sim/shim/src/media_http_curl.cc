// media_http_curl — 网络电台 MP3 流字节源的主机实现（libcurl 流式）。
// 独立 bg 线程跑 curl_easy_perform，WRITEFUNCTION 把字节喂进内部环；Read 从环阻塞取出。
// bg 线程负责断线重连 + 指数退避（1s/2s/4s/.../封顶 15s，重连期间 Read 阻塞不返回 0）；
// 连续失败累计超 60s 才彻底放弃（Read 返回 <0）。与真机 media_http_esp.cc 接口/语义一致
//（不发 Icy-MetaData；只设 socket idle 超时；不设总超时）。
//
// 跨线程 Abort（Stage D 硬化）：abort_ 原子标志 + cv 通知——Read() 的等待谓词、WriteCb
// 的背压等待、Run() 的退避睡眠都轮询它，故 Abort() 本身极快（<1ms，只是置位+notify）；
// 真正的响应延迟落在 curl_easy_perform 内部：其 XferCb（NOPROGRESS 关闭后 libcurl 按传输
// 活动频繁回调，通常远高于 1Hz）观察到 abort_ 后返回非零值主动中止 perform，实测响应在
// 数百毫秒量级，满足 Stop <3s 的验收要求。
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "esp_log.h"
#include "media_player/media_http_stream.h"

namespace media {
namespace {

const char* kTag = "media_http_curl";
constexpr size_t kInternalCap = 256 * 1024;  // 内部环上限：满则 WriteCb 阻塞 → TCP 背压
constexpr int kBackoffBaseMs = 1000;         // 指数退避基数：1s/2s/4s/8s/封顶 15s
constexpr int kBackoffMaxMs = 15000;
constexpr int kGiveUpAfterMs = 60000;  // 连续重连失败累计超过这个时长才放弃（此前无限重试）
constexpr int kBackoffPollMs = 50;     // 退避睡眠分段粒度，让 abort_ 能及时打断

class CurlStreamSource : public MediaSource {
 public:
    CurlStreamSource(const char* url, std::function<void(bool)> on_reconnecting)
        : url_(url ? url : ""), on_reconnecting_(std::move(on_reconnecting)) {}
    ~CurlStreamSource() override { Close(); }

    void Start() { thr_ = std::thread([this] { Run(); }); }

    // 等首个字节或彻底失败（首连健康检查）。返回 false = 首连失败。
    bool WaitFirst(int timeout_ms) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [&] { return !ring_.empty() || failed_.load(); });
        return !failed_.load();
    }

    int Read(uint8_t* buf, size_t max) override {
        if (buf == nullptr || max == 0) return -1;
        std::unique_lock<std::mutex> lk(m_);
        // 阻塞至有数据 / 彻底失败 / 中止（无限流不返回 0）
        cv_.wait(lk, [&] { return !ring_.empty() || failed_.load() || abort_.load(); });
        if (!ring_.empty()) {
            size_t n = std::min(max, ring_.size());
            std::copy(ring_.begin(), ring_.begin() + n, buf);
            ring_.erase(ring_.begin(), ring_.begin() + n);
            lk.unlock();
            cv_.notify_all();  // 唤醒可能阻塞的 WriteCb
            return (int)n;
        }
        return -1;  // failed / abort
    }

    // 跨线程调用：仅置标志 + notify，不阻塞。真正的 curl 线程退出由 Close() 里的
    // join 等待（Stop 路径不调 Close，只调 Abort，join 交给 reader 线程自己随后收尾）。
    void Abort() override {
        abort_ = true;
        cv_.notify_all();
    }

    void Close() override {
        Abort();
        if (thr_.joinable()) thr_.join();
    }

    bool IsStream() const override { return true; }

 private:
    static size_t WriteCb(char* data, size_t size, size_t nmemb, void* user) {
        auto* self = static_cast<CurlStreamSource*>(user);
        size_t n = size * nmemb;
        std::unique_lock<std::mutex> lk(self->m_);
        self->cv_.wait(lk, [&] { return self->ring_.size() < kInternalCap || self->abort_.load(); });
        if (self->abort_) return 0;  // 中止传输
        self->ring_.insert(self->ring_.end(), data, data + n);
        self->bytes_this_conn_ += n;
        // 若之前处于"重连中"上报过 true，这里配对上报 false（去重：exchange 只有真正从
        // true 翻到 false 时才回调一次）。
        if (self->reconnecting_.exchange(false) && self->on_reconnecting_) {
            self->on_reconnecting_(false);
        }
        lk.unlock();
        self->cv_.notify_all();
        return n;
    }

    static int XferCb(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* self = static_cast<CurlStreamSource*>(user);
        return self->abort_ ? 1 : 0;  // 非零 → 中止 perform
    }

    void Run() {
        int64_t fail_streak_start_ms = 0;
        int attempt = 0;
        while (!abort_) {
            bytes_this_conn_ = 0;
            CURL* c = curl_easy_init();
            if (c != nullptr) {
                curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
                curl_easy_setopt(c, CURLOPT_USERAGENT, "Pinion/1.0");
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
                // socket idle 超时（不设总超时，别杀无限流）——curl 侧 Abort 的响应性靠
                // XferCb 轮询而非本超时，故沿用较宽松的 10s 判定真正失联。
                curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
                curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 10L);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteCb);
                curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
                curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, XferCb);
                curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
                curl_easy_perform(c);  // 阻塞直到流断/出错/中止
                curl_easy_cleanup(c);
            }
            if (abort_) break;
            // 连接结束（不论是否曾收到过数据）都进入退避重连：曾收到数据的一次重置失败计
            // 时/尝试计数（更轻的问题，退避从 1s 起），全程都上报 reconnecting=true 让上层
            // 可见态切"缓冲中"，与真机 media_http_esp.cc 的重连语义对齐。
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
            if (bytes_this_conn_ > 0) {
                fail_streak_start_ms = now_ms;
                attempt = 0;
            } else if (fail_streak_start_ms == 0) {
                fail_streak_start_ms = now_ms;
            }
            // 注意：不再用 ever_connected_ 门控——从未连接成功过的 URL（首连即失败）
            // 也要上报 reconnecting=true。之前门控只在"曾经连上过又断开"时才报，配合
            // media_pump.cc 的旧版"源一打开就报 Playing"会让从未连上的坏 URL 长达
            // ~60s 显示 Playing；现在 Playing 已改为首帧真正喂出才触发（见 OnPlaybackFlowing），
            // 这里放开门控是为了让 OnReconnecting 的语义本身也诚实（不依赖上层的另一层保护）。
            if (!reconnecting_.exchange(true) && on_reconnecting_) {
                on_reconnecting_(true);
            }
            if (now_ms - fail_streak_start_ms > kGiveUpAfterMs) {
                failed_ = true;
                cv_.notify_all();
                break;
            }
            int backoff = kBackoffBaseMs << attempt;
            if (backoff > kBackoffMaxMs) backoff = kBackoffMaxMs;
            ESP_LOGW(kTag, "reconnect attempt %d in %dms (streak %dms/%dms)", attempt + 1, backoff,
                     (int)(now_ms - fail_streak_start_ms), kGiveUpAfterMs);
            attempt++;
            for (int w = 0; w < backoff && !abort_; w += kBackoffPollMs)
                std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffPollMs));
        }
    }

    std::string url_;
    std::function<void(bool)> on_reconnecting_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<uint8_t> ring_;
    std::atomic<bool> abort_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> reconnecting_{false};  // 当前是否已上报过 reconnecting=true（去重）
    size_t bytes_this_conn_ = 0;
    std::thread thr_;
};

}  // namespace

std::unique_ptr<MediaSource> OpenHttpStreamSource(const char* url,
                                                  std::function<void(bool reconnecting)> on_reconnecting) {
    if (url == nullptr || url[0] == '\0') return nullptr;
    auto src = std::unique_ptr<CurlStreamSource>(new CurlStreamSource(url, std::move(on_reconnecting)));
    src->Start();
    if (!src->WaitFirst(8000)) return nullptr;  // 首连失败
    return src;
}

}  // namespace media
