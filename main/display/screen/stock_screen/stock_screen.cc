// stock_screen.cc — 列表 / 详情图表 / Web 设置三视图宿主 + 抓取节奏调度。

#include "stock_screen.h"

#include "beijing_clock.h"
#include "home_screen/home_screen.h"
#include "list_refresh_queue.h"
#include "market_schedule.h"
#include "stock_config_server.h"
#include "stock_detail_view.h"
#include "stock_fetch_worker.h"
#include "stock_list_view.h"
#include "stock_settings_view.h"
#include "stock_store.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <string>

namespace {

constexpr char TAG[] = "stock_ui";
constexpr int32_t kPanelW = 720;
constexpr uint32_t kColorBg = 0x0E1116;

constexpr uint32_t kPaceTickMs = 250;
constexpr uint32_t kMarketStatIntervalMs = 60000;
constexpr uint32_t kPreNtpQuoteMs = 2000;

enum class View { List, Detail, Settings };

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_list_root = nullptr;
lv_obj_t* s_detail_root = nullptr;
lv_obj_t* s_settings_root = nullptr;
lv_timer_t* s_pace_timer = nullptr;
View s_view = View::List;

// list pacing 状态
uint32_t s_last_quote_ms = 0;
uint32_t s_last_market_ms = 0;
uint32_t s_spark_last_step = 0;
size_t s_spark_cursor = 0;
ListRefreshQueue::ItemState s_spark_items[stock_store::kMaxStocks];
bool s_any_market_open = false;
bool s_net_ready_prev = false;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

int FindStoreIdx(const char* symbol) {
    if (symbol == nullptr) return -1;
    for (size_t i = 0; i < stock_store::Count(); i++) {
        if (stock_store::SymbolAt(i) == symbol) return static_cast<int>(i);
    }
    return -1;
}

// ---- worker 回调（worker 线程、持 LVGL 锁、session 已校验）------------------
void OnQuoteBatch(const StockQuote* quotes, size_t n, bool ok, const char* err) {
    if (!ok) {
        ESP_LOGW(TAG, "quote batch failed: %s", err);
        return;
    }
    if (s_view == View::List) {
        for (size_t i = 0; i < n; i++) {
            if (quotes[i].valid) stock_list_view::ApplyQuote(i, quotes[i]);
        }
    } else if (s_view == View::Detail && n >= 1) {
        stock_detail_view::ApplyQuote(quotes[0]);
    }
}

void OnChart(const ChartSeries* chart, bool ok, const char* err, bool is_range) {
    if (chart == nullptr) return;
    if (s_view == View::Detail) {
        if (ok) stock_detail_view::ApplyChart(*chart, is_range);
        else ESP_LOGW(TAG, "chart failed %s: %s", chart->symbol.c_str(), err);
        return;
    }
    // 列表 spark
    int idx = FindStoreIdx(chart->symbol.c_str());
    if (idx >= 0 && static_cast<size_t>(idx) < stock_store::kMaxStocks) {
        s_spark_items[idx].valid = ok;
    }
    if (ok) stock_list_view::ApplySpark(chart->symbol.c_str(), *chart);
    else ESP_LOGW(TAG, "spark failed %s: %s", chart->symbol.c_str(), err);
}

void OnMarketStat(const MarketStatus* stat, bool ok, const char* err) {
    if (!ok || stat == nullptr) {
        ESP_LOGW(TAG, "marketStat failed: %s", err);
        return;
    }
    s_any_market_open = stat->sh_open || stat->sz_open || stat->hk_open || stat->us_open;
    stock_detail_view::ApplyMarketStat(*stat);
    if (s_view == View::List) stock_list_view::ApplyMarketStat(*stat);
}

// ---- list pacing ------------------------------------------------------------
void PaceQuotes(uint32_t now) {
    if (stock_fetch::InFlight(stock_fetch::FETCH_QUOTE_BATCH)) return;
    uint32_t delay;
    if (beijing_clock::Valid()) {
        MarketSchedule::PollPolicy policy{1000, 60000};
        delay = MarketSchedule::nextPollDelayMs(beijing_clock::NowEpochS(), policy,
                                                s_any_market_open);
    } else {
        delay = kPreNtpQuoteMs;
    }
    if (s_last_quote_ms != 0 && (now - s_last_quote_ms) < delay) return;

    size_t cnt = stock_store::Count();
    if (cnt == 0) return;
    std::string syms[stock_store::kMaxStocks];
    for (size_t i = 0; i < cnt; i++) syms[i] = stock_store::SymbolAt(i);
    if (stock_fetch::EnqueueQuoteBatch(syms, cnt)) s_last_quote_ms = now;
}

void PaceSpark(uint32_t now) {
    size_t cnt = stock_store::Count();
    if (cnt == 0) return;
    ListRefreshQueue::State st;
    st.now_ms = now;
    st.last_step_at_ms = s_spark_last_step;
    st.cursor = s_spark_cursor;
    st.count = cnt;
    st.in_flight = stock_fetch::InFlight(stock_fetch::FETCH_CHART);
    st.items = s_spark_items;
    ListRefreshQueue::Policy policy;
    ListRefreshQueue::Decision d = ListRefreshQueue::planNext(st, policy);
    s_spark_cursor = d.next_cursor;
    if (!d.should_fetch) return;
    const std::string& sym = stock_store::SymbolAt(d.index);
    if (stock_fetch::EnqueueChart(sym.c_str(), CHART_MIN_1D)) {
        s_spark_items[d.index].last_attempt_at_ms = now;
        s_spark_last_step = now;
    }
}

void PaceMarketStat(uint32_t now) {
    if (stock_fetch::InFlight(stock_fetch::FETCH_MARKET_STAT)) return;
    if (s_last_market_ms != 0 && (now - s_last_market_ms) < kMarketStatIntervalMs) return;
    if (stock_fetch::EnqueueMarketStat()) s_last_market_ms = now;
}

void OnPaceTick(lv_timer_t* /*t*/) {
    uint32_t now = NowMs();

    // 网络就绪门控：未就绪不入队（避免 connect 阻塞卡死 in-flight），显示加载态。
    bool ready = stock_fetch::NetworkReady();
    if (ready && !s_net_ready_prev) {
        // 刚就绪 → 复位计时器立即拉取，无需等重试间隔或用户重进
        s_last_quote_ms = 0;
        s_last_market_ms = 0;
        s_spark_last_step = 0;
        for (auto& it : s_spark_items) it.last_attempt_at_ms = 0;
    }
    s_net_ready_prev = ready;

    if (s_view == View::List) stock_list_view::SetBanner(ready ? "" : "连接网络中…");
    if (s_view == View::Detail) stock_detail_view::SetNetworkReady(ready);

    if (!ready) return;  // 未就绪：跳过所有拉取，等就绪

    if (s_view == View::List) {
        PaceQuotes(now);
        PaceSpark(now);
    } else if (s_view == View::Detail) {
        stock_detail_view::Tick(now);
    } else {
        return;  // Settings：worker 空闲，让配置服务器独占网络
    }
    PaceMarketStat(now);
}

// ---- 导航 -------------------------------------------------------------------
void GoHome() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) lv_obj_delete_async(old_scr);
}

void ShowList() {
    s_view = View::List;
    stock_detail_view::Hide();
    stock_settings_view::Hide();
    if (s_list_root) lv_obj_remove_flag(s_list_root, LV_OBJ_FLAG_HIDDEN);
    s_last_quote_ms = 0;  // 回列表时立即刷新一次报价
}

void ShowDetail() {
    s_view = View::Detail;
    if (s_list_root) lv_obj_add_flag(s_list_root, LV_OBJ_FLAG_HIDDEN);
    stock_settings_view::Hide();
    stock_detail_view::Show();
}

void ShowSettings() {
    s_view = View::Settings;
    if (s_list_root) lv_obj_add_flag(s_list_root, LV_OBJ_FLAG_HIDDEN);
    stock_detail_view::Hide();
    stock_settings_view::Show();
}

void BackToListFromSettings() {
    // 配置有改动 → reload NVS + 重建列表行
    if (stock_config_server::ConsumeDirty()) {
        stock_store::Reload();
        stock_list_view::Rebuild();
        for (auto& it : s_spark_items) it = ListRefreshQueue::ItemState{};
        s_spark_cursor = 0;
    }
    ShowList();
}

void OnSwipeBack() {
    if (s_view == View::Detail) {
        ShowList();
        return;
    }
    if (s_view == View::Settings) {
        BackToListFromSettings();
        return;
    }
    GoHome();
}

void OnRowClick(size_t idx) {
    stock_store::SetCurrentIdx(idx);
    ShowDetail();
}

void OnDetailBack() { ShowList(); }
void OnSettingsBack() { BackToListFromSettings(); }
void OnGear() { ShowSettings(); }

// ---- 生命周期 ---------------------------------------------------------------
void OnScreenLoaded(lv_event_t* /*e*/) {
    stock_store::Reload();

    stock_fetch::Callbacks cb;
    cb.on_quote_batch = OnQuoteBatch;
    cb.on_chart = OnChart;
    cb.on_market_stat = OnMarketStat;
    stock_fetch::Begin(cb);

    s_view = View::List;
    s_last_quote_ms = 0;
    s_last_market_ms = 0;
    s_spark_last_step = 0;
    s_spark_cursor = 0;
    s_net_ready_prev = false;
    for (auto& it : s_spark_items) it = ListRefreshQueue::ItemState{};

    stock_list_view::Rebuild();
    ShowList();

    stock_fetch::EnqueueMarketStat();
    s_last_market_ms = NowMs();

    if (s_pace_timer == nullptr) {
        s_pace_timer = lv_timer_create(OnPaceTick, kPaceTickMs, nullptr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    stock_fetch::BumpSession();
    if (s_pace_timer != nullptr) {
        lv_timer_delete(s_pace_timer);
        s_pace_timer = nullptr;
    }
    stock_settings_view::Reset();
    stock_detail_view::Reset();
    stock_list_view::Reset();
    s_screen = nullptr;
    s_list_root = nullptr;
    s_detail_root = nullptr;
    s_settings_root = nullptr;
}

}  // namespace

lv_obj_t* StockScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelW);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    stock_list_view::Callbacks lcb;
    lcb.on_row_click = OnRowClick;
    lcb.on_gear = OnGear;
    lcb.on_back = OnSwipeBack;
    s_list_root = stock_list_view::Build(scr, lcb);

    stock_detail_view::Callbacks dcb;
    dcb.on_back = OnDetailBack;
    s_detail_root = stock_detail_view::Build(scr, dcb);

    stock_settings_view::Callbacks scb;
    scb.on_back = OnSettingsBack;
    s_settings_root = stock_settings_view::Build(scr, scb);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

void StockScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    ESP_LOGI(TAG, "%s: stock_screen", event == SCREEN_LIFECYCLE_LOAD ? "load" : "unload");
}
