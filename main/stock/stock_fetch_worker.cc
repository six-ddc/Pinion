// stock_fetch_worker.cc — 见头文件。

#include "stock_fetch_worker.h"

#include "stock_api.h"

#include "esp_log.h"
#include "metalio_hal/network.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstring>
#include <string>

namespace stock_fetch_worker {
namespace {

constexpr char TAG[] = "stock_worker";
constexpr int kQueueDepth = 8;
constexpr uint32_t kTaskStack = 8 * 1024;
constexpr UBaseType_t kTaskPrio = 3;

QueueHandle_t g_req_q = nullptr;
QueueHandle_t g_res_q = nullptr;

void SetErr(Result& res, const std::string& err) {
    std::strncpy(res.err, err.empty() ? "fetch failed" : err.c_str(), sizeof(res.err) - 1);
    res.err[sizeof(res.err) - 1] = '\0';
}

void WorkerTask(void*) {
    Request req;
    for (;;) {
        if (xQueueReceive(g_req_q, &req, portMAX_DELAY) != pdTRUE) continue;
        Result res;
        res.session = req.session;
        res.kind = req.kind;
        res.mode = req.mode;
        std::memcpy(res.symbol, req.symbol, sizeof(res.symbol));
        std::string err;
        // boot 竞态根修：NVS 里 pin 的股票卡在 RehydratePin（load pi_screen）时就会提交
        // 请求，而 lwip/esp_netif 要到之后的 network::StartAsync() 才初始化——未就绪时
        // esp_http_client 里的 getaddrinfo 直接命中 lwip assert（tcpip_send_msg_wait_sem
        // Invalid mbox）→ panic 重启 → rehydrate 再崩，真机开机死循环。IsConnected() 是
        // 纯状态读（StartAsync 前恒 false，状态栏 NetTick 同窗口反复调用已验证安全），
        // 未连网一律拒绝本轮；stock_chart 的自适应调度稍后自会重试。
        if (!mhal::network::IsConnected()) {
            res.ok = false;
            SetErr(res, "network not ready");
        } else if (req.kind == Kind::Quote) {
            auto* q = new StockQuote();
            std::string sym(req.symbol);
            res.ok = stock_api::FetchQuoteBatch(&sym, 1, q, err) && q->valid;
            if (res.ok) {
                res.quote = q;
            } else {
                delete q;
                SetErr(res, err);
            }
        } else {
            auto* s = new ChartSeries();
            res.ok = stock_api::FetchChart(req.symbol, req.mode, *s, err) && s->valid;
            if (res.ok) {
                res.series = s;
            } else {
                delete s;
                SetErr(res, err);
            }
        }
        if (xQueueSend(g_res_q, &res, 0) != pdTRUE) {
            // 结果队列满（消费端一整轮没排空，异常场景）：丢弃并释放，绝不阻塞 worker。
            ESP_LOGW(TAG, "result queue full, drop %s", res.symbol);
            FreePayload(res);
        }
    }
}

}  // namespace

void EnsureStarted() {
    if (g_req_q != nullptr) return;
    g_req_q = xQueueCreate(kQueueDepth, sizeof(Request));
    g_res_q = xQueueCreate(kQueueDepth, sizeof(Result));
    xTaskCreate(WorkerTask, "stock_fetch", kTaskStack, nullptr, kTaskPrio, nullptr);
}

bool Submit(const Request& r) {
    EnsureStarted();
    return xQueueSend(g_req_q, &r, 0) == pdTRUE;
}

bool Poll(Result& out) {
    if (g_res_q == nullptr) return false;
    return xQueueReceive(g_res_q, &out, 0) == pdTRUE;
}

void FreePayload(Result& r) {
    delete r.quote;
    r.quote = nullptr;
    delete r.series;
    r.series = nullptr;
}

}  // namespace stock_fetch_worker
