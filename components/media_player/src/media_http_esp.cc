// media_http_esp — 网络电台 MP3 流字节源的真机实现（esp_http_client 流式）。
// 序列 open → fetch_headers → read 循环，与 stock_http_esp.cc / pi-c transport_esp_http 同范式，
// 但这是**无限流**：不设总超时（只设 socket idle 超时），内置断线重连 + 指数退避。
// 仅 device 编译（sim 用 media_http_curl.cc）。
//
// 跨线程 Abort（Stage D 硬化）：esp_http_client 没有真正的跨线程中断原语，
// esp_http_client_read() 阻塞期间无法从另一线程唤醒它——只能靠把 socket idle 超时
// （cfg.timeout_ms）收紧到 kSocketTimeoutMs，让阻塞的读定期自己超时返回，Read() 的重试
// 循环再观察 abort_requested_ 及时退出。这把 Stop/切曲的最坏响应延迟从原先的
// "整条重连退避链跑完"缩到"至多一个 socket 超时"，需要 Abort() 与该超时配合看待。
#include <atomic>
#include <cstring>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "media_player/media_http_stream.h"

namespace media {
namespace {

const char* TAG = "media_http";

// 只设 socket idle 超时（不设总超时，别杀无限流）；同时是 Abort() 的响应性上界
//（见文件头注释），故收紧到 2.5s——比原先的 10s 更快让阻塞的读返回来观察 stop。
constexpr int kSocketTimeoutMs = 2500;
constexpr int kBackoffBaseMs = 1000;   // 指数退避基数：1s/2s/4s/8s/封顶 15s
constexpr int kBackoffMaxMs = 15000;
constexpr int kGiveUpAfterMs = 60000;  // 连续重连失败超过这个累计时长才放弃转 Error（此前无限重试）
constexpr int kBackoffPollMs = 200;    // 退避睡眠分段粒度，让 abort_requested_ 能及时打断

class HttpStreamSource : public MediaSource {
 public:
    HttpStreamSource(const char* url, std::function<void(bool)> on_reconnecting)
        : url_(url ? url : ""), on_reconnecting_(std::move(on_reconnecting)) {}
    ~HttpStreamSource() override { Close(); }

    // 首次连接：成功返回 true。构造后由工厂调用一次。
    bool Open() { return Connect(); }

    int Read(uint8_t* buf, size_t max) override {
        if (buf == nullptr || max == 0) return -1;
        for (;;) {
            if (abort_requested_) return -1;
            if (client_ == nullptr) {
                if (!Reconnect()) return -1;  // 彻底失败/被 abort
            }
            if (abort_requested_) return -1;
            int n = esp_http_client_read(client_, (char*)buf, (int)max);
            if (n > 0) {
                fail_streak_start_us_ = 0;  // 有数据即认为连接健康，重置失败计时
                return n;
            }
            if (abort_requested_) return -1;
            // n<=0：连接掉了/idle 超时。无限流不该正常 EOF，一律当断线重连。
            ESP_LOGW(TAG, "stream read returned %d, reconnecting", n);
            Disconnect();
            if (!Reconnect()) return -1;
            // 重连成功后回到循环继续读（Read 在重连期间阻塞，不返回 0）
        }
    }

    // 跨线程调用：仅置标志，不做任何可能阻塞的操作。Read()/Reconnect() 的轮询在有界
    // 延迟内（至多一个 kSocketTimeoutMs 或一段 kBackoffPollMs 退避片）观察到并返回。
    void Abort() override { abort_requested_ = true; }

    void Close() override {
        abort_requested_ = true;
        Disconnect();
    }
    bool IsStream() const override { return true; }

 private:
    bool Connect() {
        esp_http_client_config_t cfg = {};
        cfg.url = url_.c_str();
        cfg.method = HTTP_METHOD_GET;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;  // https 用；http 不生效
        cfg.timeout_ms = kSocketTimeoutMs;
        cfg.buffer_size = 2048;
        client_ = esp_http_client_init(&cfg);
        if (client_ == nullptr) return false;
        // 不发 Icy-MetaData（不要元数据穿插进音频流）；不发 Connection: close（保持长连）。
        esp_http_client_set_header(client_, "User-Agent", "Pinion/1.0");
        esp_http_client_set_header(client_, "Icy-MetaData", "0");
        if (esp_http_client_open(client_, 0) != ESP_OK) {
            Disconnect();
            return false;
        }
        if (esp_http_client_fetch_headers(client_) < 0) {
            Disconnect();
            return false;
        }
        int status = esp_http_client_get_status_code(client_);
        if (status != 200) {
            ESP_LOGW(TAG, "stream HTTP %d", status);
            Disconnect();
            return false;
        }
        ESP_LOGI(TAG, "stream connected: %s", url_.c_str());
        return true;
    }

    void Disconnect() {
        if (client_ != nullptr) {
            esp_http_client_close(client_);
            esp_http_client_cleanup(client_);
            client_ = nullptr;
        }
    }

    // 指数退避重连：1s/2s/4s/8s/封顶 15s，无限重试直到 abort 或连续失败累计超
    // kGiveUpAfterMs（60s）才放弃（返回 false，Read 据此返回 <0 → 上层转 Error）。
    // 重连期间报 reconnecting=true；重连成功报 false（宿主据此把可见态切 Loading/Playing）。
    bool Reconnect() {
        if (fail_streak_start_us_ == 0) fail_streak_start_us_ = esp_timer_get_time();
        if (on_reconnecting_) on_reconnecting_(true);
        int attempt = 0;
        while (!abort_requested_) {
            int64_t elapsed_ms = (esp_timer_get_time() - fail_streak_start_us_) / 1000;
            if (elapsed_ms > kGiveUpAfterMs) {
                ESP_LOGE(TAG, "stream reconnect giving up after %dms", (int)elapsed_ms);
                return false;
            }
            int backoff = kBackoffBaseMs << attempt;
            if (backoff > kBackoffMaxMs) backoff = kBackoffMaxMs;
            for (int waited = 0; waited < backoff && !abort_requested_; waited += kBackoffPollMs) {
                int slice = backoff - waited < kBackoffPollMs ? backoff - waited : kBackoffPollMs;
                vTaskDelay(pdMS_TO_TICKS(slice));
            }
            if (abort_requested_) return false;
            attempt++;
            if (Connect()) {
                fail_streak_start_us_ = 0;
                if (on_reconnecting_) on_reconnecting_(false);
                return true;
            }
        }
        return false;
    }

    std::string url_;
    std::function<void(bool)> on_reconnecting_;
    esp_http_client_handle_t client_ = nullptr;
    std::atomic<bool> abort_requested_{false};
    int64_t fail_streak_start_us_ = 0;  // 0 = 当前无失败连续段
};

}  // namespace

std::unique_ptr<MediaSource> OpenHttpStreamSource(const char* url,
                                                  std::function<void(bool reconnecting)> on_reconnecting) {
    if (url == nullptr || url[0] == '\0') return nullptr;
    auto src = std::unique_ptr<HttpStreamSource>(new HttpStreamSource(url, std::move(on_reconnecting)));
    if (!src->Open()) return nullptr;
    return src;
}

}  // namespace media
