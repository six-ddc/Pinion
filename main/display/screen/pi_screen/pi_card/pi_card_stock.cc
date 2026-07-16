// pi_card_stock.cc — 见头文件。

#include "pi_card_stock.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "pi_fonts.h"
#include "pi_theme.h"
#include "screen_util.h"
#include "stock_chart_renderer.h"

#include "stock/market_hours.h"
#include "stock/market_schedule.h"
#include "stock/quote_parse.h"
#include "stock/stock_api.h"
#include "stock/stock_fetch_scheduler.h"
#include "stock/stock_fetch_worker.h"

#include <ctime>

namespace pi_card_stock {
namespace {

constexpr char TAG[] = "pi_card_stock";
using pi_theme::Tok;
using stock_fetch_worker::Kind;

constexpr int kMaxLive = 3;         // 全屏同时存活的 stock_chart 上限（校验期拒绝）
constexpr int kDefaultW = 600;      // chat 卡内容可用宽 ≈606px
constexpr int kDefaultH = 260;
constexpr int kMinW = 240, kMaxW = 656;
constexpr int kMinH = 120, kMaxH = 400;

// 抓取节奏：chat 卡不必 Claw4 详情页的 1s——报价盘中 5s；图表沿用 Claw4 口径。
const StockFetchScheduler::Policy kQuotePolicy{5000, 60000, 10000, 15000};
const StockFetchScheduler::Policy kChartMinutePolicy{10000, 60000, 10000, 15000};
const StockFetchScheduler::Policy kChartKlinePolicy{30000, 120000, 30000, 30000};

const char* kModeNames[CHART_MODE_COUNT] = {"分时", "五日", "日K", "周K"};

struct StockCtx {
    uint32_t session = 0;  // worker 结果匹配代次；切模式/删除时作废
    std::string symbol;
    std::string name;
    ChartMode mode = CHART_MIN_1D;

    lv_obj_t* root = nullptr;
    lv_obj_t* holder = nullptr;  // canvas 容器（浮动 label 的定位参照 + 点击目标）
    lv_obj_t* lbl_name = nullptr;
    lv_obj_t* lbl_price = nullptr;
    lv_obj_t* lbl_pct = nullptr;
    lv_obj_t* lbl_mode = nullptr;
    lv_obj_t* lbl_time = nullptr;
    lv_obj_t* lbl_loading = nullptr;
    stock_chart_renderer::Target target;

    void* canvas_buf = nullptr;

    StockQuote quote;             // 最近一次有效报价（valid=false 表示还没有）
    ChartSeries* series = nullptr;  // 最近一次有效图序列（拥有；主题切换重绘用）

    uint32_t last_quote_ms = 0;   // 0 = 从未拉取（调度器视为立即就绪）
    uint32_t last_chart_ms = 0;
    bool quote_inflight = false;
    bool chart_inflight = false;
    bool quote_valid_once = false;
    bool chart_valid_once = false;
};

std::vector<StockCtx*> g_widgets;          // LVGL 线程独占
std::atomic<int> g_live{0};                // worker 线程校验用的存活计数
std::atomic<uint32_t> g_next_session{1};
lv_timer_t* g_timer = nullptr;
int g_theme_listener = -1;

uint32_t NextSession() { return g_next_session.fetch_add(1); }

// ---- 小工具 ----------------------------------------------------------------

const char* GetStr(const cJSON* n, const char* k, const char* dflt = nullptr) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(n, k);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : dflt;
}

int GetInt(const cJSON* n, const char* k, int dflt) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(n, k);
    return cJSON_IsNumber(it) ? static_cast<int>(it->valuedouble) : dflt;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ModeOf(const char* s, ChartMode& out) {
    if (s == nullptr || std::strcmp(s, "min") == 0) { out = CHART_MIN_1D; return true; }
    if (std::strcmp(s, "5d") == 0) { out = CHART_MIN_5D; return true; }
    if (std::strcmp(s, "day") == 0) { out = CHART_KLINE_D; return true; }
    if (std::strcmp(s, "week") == 0) { out = CHART_KLINE_W; return true; }
    return false;
}

bool AllDigits(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool ValidSymbol(const char* s) {
    using quote_parse::Market;
    if (s == nullptr) return false;
    Market m = quote_parse::marketOf(s);
    size_t n = std::strlen(s);
    switch (m) {
        case Market::A_SH:
        case Market::A_SZ:
            return n == 8 && AllDigits(s + 2, 6);
        case Market::HK:
            return n == 7 && AllDigits(s + 2, 5);
        case Market::US:
            return n >= 3 && n <= 16;  // usAAPL.OQ / usVEEV.N / usTSLA
        default:
            return false;
    }
}

// 北京时间 HH:MM（不依赖本地 TZ 设置）。
void BjClockText(char* buf, size_t cap) {
    time_t bj = MarketSchedule::utcToBjEpoch(time(nullptr));
    std::snprintf(buf, cap, "%02d:%02d", static_cast<int>((bj % 86400) / 3600), static_cast<int>((bj % 3600) / 60));
}

bool ClockReady() { return time(nullptr) > 1600000000; }  // SNTP 未同步时是 1970 起点

// ---- 渲染应用 ---------------------------------------------------------------

void ApplyQuote(StockCtx* c) {
    char buf[32];
    if (!c->quote.valid) return;
    std::snprintf(buf, sizeof(buf), "%.2f", c->quote.current);
    lv_label_set_text(c->lbl_price, buf);
    std::snprintf(buf, sizeof(buf), "%+.2f  %+.2f%%", c->quote.chg, c->quote.percent);
    lv_label_set_text(c->lbl_pct, buf);
    lv_obj_set_style_text_color(c->lbl_pct, stock_chart_renderer::PctColor(c->quote.percent), LV_PART_MAIN);
    BjClockText(buf, sizeof(buf));
    lv_label_set_text(c->lbl_time, buf);
}

void ApplyChart(StockCtx* c) {
    if (c->series == nullptr) return;
    lv_obj_add_flag(c->lbl_loading, LV_OBJ_FLAG_HIDDEN);
    stock_chart_renderer::Render(c->target, *c->series);
}

void EnterLoading(StockCtx* c) {
    stock_chart_renderer::Clear(c->target);
    lv_obj_remove_flag(c->lbl_loading, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(c->lbl_loading, "加载中…");
}

// ---- worker 交互 ------------------------------------------------------------

void SubmitFetch(StockCtx* c, Kind kind, uint32_t now) {
    stock_fetch_worker::Request req;
    req.session = c->session;
    req.kind = kind;
    req.mode = c->mode;
    std::strncpy(req.symbol, c->symbol.c_str(), sizeof(req.symbol) - 1);
    if (!stock_fetch_worker::Submit(req)) return;  // 队列满：保持未 in-flight，下轮重试
    if (kind == Kind::Quote) {
        c->quote_inflight = true;
        c->last_quote_ms = now;
    } else {
        c->chart_inflight = true;
        c->last_chart_ms = now;
    }
}

void DrainResults() {
    stock_fetch_worker::Result res;
    while (stock_fetch_worker::Poll(res)) {
        StockCtx* c = nullptr;
        for (auto* w : g_widgets) {
            if (w->session == res.session) { c = w; break; }
        }
        if (c == nullptr) {  // 控件已删除或已切模式：作废
            stock_fetch_worker::FreePayload(res);
            continue;
        }
        if (res.kind == Kind::Quote) {
            c->quote_inflight = false;
            if (res.ok) {
                std::string keep_name = c->quote.name;
                c->quote = *res.quote;
                if (c->quote.name.empty()) c->quote.name = keep_name;
                c->quote_valid_once = true;
                ApplyQuote(c);
            }
        } else {
            c->chart_inflight = false;
            if (res.ok) {
                delete c->series;
                c->series = res.series;
                res.series = nullptr;
                c->chart_valid_once = true;
                ApplyChart(c);
            } else if (!c->chart_valid_once) {
                lv_label_set_text(c->lbl_loading, "暂无数据");
            }
        }
        stock_fetch_worker::FreePayload(res);
    }
}

void ScheduleFetches(uint32_t now) {
    bool clock_ready = ClockReady();
    time_t bj = MarketSchedule::utcToBjEpoch(time(nullptr));
    for (auto* c : g_widgets) {
        if (!lv_obj_is_visible(c->holder)) continue;  // 滚出视口/屏隐藏：不拉取
        StockFetchScheduler::State st;
        st.now_ms = now;
        st.clock_ready = clock_ready;
        st.bj_epoch = bj;
        st.in_session = market_hours::inSession(c->symbol.c_str(), bj);

        st.last_fetch_at_ms = c->last_quote_ms;
        st.valid = c->quote_valid_once;
        st.in_flight = c->quote_inflight;
        if (StockFetchScheduler::shouldFetch(st, kQuotePolicy)) SubmitFetch(c, Kind::Quote, now);

        const bool kline = (c->mode == CHART_KLINE_D || c->mode == CHART_KLINE_W);
        st.last_fetch_at_ms = c->last_chart_ms;
        st.valid = c->chart_valid_once;
        st.in_flight = c->chart_inflight;
        if (StockFetchScheduler::shouldFetch(st, kline ? kChartKlinePolicy : kChartMinutePolicy)) {
            SubmitFetch(c, Kind::Chart, now);
        }
    }
}

void TimerCb(lv_timer_t*) {
    DrainResults();
    ScheduleFetches(lv_tick_get());
}

void OnThemeChanged() {
    for (auto* c : g_widgets) {
        if (c->series != nullptr) {
            stock_chart_renderer::Render(c->target, *c->series);
        } else {
            stock_chart_renderer::Clear(c->target);
        }
        if (c->quote.valid) ApplyQuote(c);
    }
}

void EnsureModuleStarted() {
    if (g_timer == nullptr) {
        g_timer = lv_timer_create(TimerCb, 1000, nullptr);
    }
    lv_timer_resume(g_timer);
    if (g_theme_listener < 0) g_theme_listener = pi_theme::AddListener(OnThemeChanged);
}

// ---- 交互 -------------------------------------------------------------------

void OnChartClicked(lv_event_t* e) {
    auto* c = static_cast<StockCtx*>(lv_event_get_user_data(e));
    c->mode = static_cast<ChartMode>((c->mode + 1) % CHART_MODE_COUNT);
    c->session = NextSession();  // 作废在途结果
    c->quote_inflight = false;
    c->chart_inflight = false;
    c->last_chart_ms = 0;  // 调度器视为"从未拉取"→ 下一 tick 立即抓新模式
    c->chart_valid_once = false;
    delete c->series;
    c->series = nullptr;
    lv_label_set_text(c->lbl_mode, kModeNames[c->mode]);
    EnterLoading(c);
    SubmitFetch(c, Kind::Chart, lv_tick_get());  // 不等 tick，立即入队
}

void OnHolderDeleted(lv_event_t* e) {
    auto* c = static_cast<StockCtx*>(lv_event_get_user_data(e));
    for (auto it = g_widgets.begin(); it != g_widgets.end(); ++it) {
        if (*it == c) { g_widgets.erase(it); break; }
    }
    g_live.store(static_cast<int>(g_widgets.size()));
    ESP_LOGI(TAG, "widget %s deleted, %d live", c->symbol.c_str(), g_live.load());
    heap_caps_free(c->canvas_buf);
    delete c->series;
    delete c;
    if (g_widgets.empty() && g_timer != nullptr) lv_timer_pause(g_timer);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, Tok tone) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    pi_theme::ApplyText(lbl, tone);
    return lbl;
}

}  // namespace

bool ValidateNode(const cJSON* node, std::string& err) {
    const char* symbol = GetStr(node, "symbol");
    if (symbol == nullptr || !ValidSymbol(symbol)) {
        err = std::string("stock_chart symbol '") + (symbol ? symbol : "") +
              "' invalid; use Tencent format from the stock tool: sh/sz+6 digits, hk+5 digits, or usTICKER.N/.OQ";
        return false;
    }
    ChartMode mode;
    if (!ModeOf(GetStr(node, "mode"), mode)) {
        err = "stock_chart mode must be one of min|5d|day|week";
        return false;
    }
    if (g_live.load() >= kMaxLive) {
        err = "at most " + std::to_string(kMaxLive) +
              " stock_chart widgets can be alive; ui_close an old card first";
        return false;
    }
    return true;
}

lv_obj_t* Create(lv_obj_t* parent, const cJSON* node) {
    const char* symbol = GetStr(node, "symbol", "");
    const char* name = GetStr(node, "name");
    const int cw = Clamp(GetInt(node, "w", kDefaultW), kMinW, kMaxW);
    const int ch = Clamp(GetInt(node, "h", kDefaultH), kMinH, kMaxH);

    void* buf = heap_caps_aligned_alloc(64, static_cast<size_t>(cw) * ch * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) buf = heap_caps_malloc(static_cast<size_t>(cw) * ch * 2, MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "canvas buf OOM (%dx%d)", cw, ch);
        return nullptr;
    }

    auto* c = new StockCtx();
    c->session = NextSession();
    c->symbol = symbol;
    c->name = (name != nullptr) ? name : "";
    ModeOf(GetStr(node, "mode"), c->mode);
    c->canvas_buf = buf;

    // 根：竖排，宽随卡片内容区
    lv_obj_t* root = lv_obj_create(parent);
    screen_strip_obj_chrome(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_width(root, LV_PCT(100));
    lv_obj_set_height(root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 8, LV_PART_MAIN);
    c->root = root;

    // 头行：名称 + 代码 | 现价 + 涨跌
    lv_obj_t* head = lv_obj_create(root);
    screen_strip_obj_chrome(head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(head, 10, LV_PART_MAIN);

    // 名称必须用完整常用字集字体（24_4 是 UI chrome 静态子集，股票名会缺字成豆腐块）
    c->lbl_name = MakeLabel(head, &font_puhui_30_4, Tok::Tx);
    lv_label_set_text(c->lbl_name, (c->name.empty()) ? stock_api::DisplayCode(symbol).c_str() : c->name.c_str());
    lv_obj_t* code = MakeLabel(head, &font_pi_mono_14, Tok::Faint);
    lv_label_set_text(code, stock_api::DisplayCode(symbol).c_str());
    lv_obj_set_style_pad_bottom(code, 3, LV_PART_MAIN);  // 与名称基线对齐

    lv_obj_t* gap = lv_obj_create(head);
    screen_strip_obj_chrome(gap);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_height(gap, 1);
    lv_obj_set_flex_grow(gap, 1);

    c->lbl_price = MakeLabel(head, &font_pi_mono_20, Tok::Tx);
    lv_label_set_text(c->lbl_price, "--");
    c->lbl_pct = MakeLabel(head, &font_pi_mono_14, Tok::Dim);
    lv_obj_set_style_pad_bottom(c->lbl_pct, 3, LV_PART_MAIN);

    // 画布区：holder 固定尺寸，canvas + 浮动坐标 label + 加载态
    lv_obj_t* holder = lv_obj_create(root);
    screen_strip_obj_chrome(holder);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(holder, cw, ch);
    lv_obj_add_flag(holder, LV_OBJ_FLAG_CLICKABLE);
    c->holder = holder;

    lv_obj_t* canvas = lv_canvas_create(holder);
    lv_obj_set_pos(canvas, 0, 0);
    lv_canvas_set_buffer(canvas, buf, cw, ch, LV_COLOR_FORMAT_RGB565);

    c->target.canvas = canvas;
    c->target.w = cw;
    c->target.h = ch;
    c->target.max_price = MakeLabel(holder, &font_pi_mono_14, Tok::Dim);
    lv_obj_align(c->target.max_price, LV_ALIGN_TOP_LEFT, 4, 2);
    c->target.min_price = MakeLabel(holder, &font_pi_mono_14, Tok::Dim);
    lv_obj_align(c->target.min_price, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    c->target.max_pct = MakeLabel(holder, &font_pi_mono_14, Tok::Dim);
    lv_obj_align(c->target.max_pct, LV_ALIGN_TOP_RIGHT, -4, 2);
    c->target.min_pct = MakeLabel(holder, &font_pi_mono_14, Tok::Dim);
    lv_obj_align(c->target.min_pct, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    c->lbl_loading = MakeLabel(holder, &font_puhui_20_4, Tok::Dim);
    lv_obj_center(c->lbl_loading);
    EnterLoading(c);

    // 脚行：模式名 + 更新时间 + 轻提示
    lv_obj_t* foot = lv_obj_create(root);
    screen_strip_obj_chrome(foot);
    lv_obj_remove_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(foot, 10, LV_PART_MAIN);

    c->lbl_mode = MakeLabel(foot, &font_puhui_20_4, Tok::Dim);
    lv_label_set_text(c->lbl_mode, kModeNames[c->mode]);
    c->lbl_time = MakeLabel(foot, &font_pi_mono_14, Tok::Faint);
    lv_obj_t* hint = MakeLabel(foot, &font_puhui_20_4, Tok::Faint);
    lv_label_set_text(hint, "点图切周期");

    lv_obj_add_event_cb(holder, OnChartClicked, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(holder, OnHolderDeleted, LV_EVENT_DELETE, c);

    g_widgets.push_back(c);
    g_live.store(static_cast<int>(g_widgets.size()));
    EnsureModuleStarted();

    // 首帧：立即入队报价 + 图表（不等第一个 tick）
    uint32_t now = lv_tick_get();
    SubmitFetch(c, Kind::Quote, now);
    SubmitFetch(c, Kind::Chart, now);
    return root;
}

}  // namespace pi_card_stock
