// pi_card_stock.cc — 见头文件。

#include "pi_card_stock.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "pi_card_data.h"

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

// ---- Phase4：stock.<symbol>.<field> 动态绑定订阅（LVGL 线程独占）----
// 无控件的报价订阅：DataHub 动态路径被 Acquire 时按 symbol 建订阅，复用本模块的
// 1s timer + worker + kQuotePolicy 节奏；结果不进控件 label 而是推回 DataHub 的
// subject，任意绑定该路径的 label 经 observer 自动刷新。
constexpr int kMaxBindSymbols = 6;  // 同时订阅的 symbol 上限（盘中每个 5s 一拉，防失控）

struct BindSub {
    uint32_t session = 0;       // worker 结果匹配代次
    int refcount = 0;           // 该 symbol 下被 Acquire 的路径数
    uint32_t last_quote_ms = 0;
    bool inflight = false;
    bool valid_once = false;
    StockQuote quote;           // 最近一次有效报价缓存（新路径绑定时立即补种）
};
std::map<std::string, BindSub> g_subs;   // key: symbol
std::set<std::string> g_bound_paths;     // provider 已接受（计入 refcount）的路径——
                                         // 超限被拒的路径不在此表，release 时据此不误退订阅

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

// ---- Phase4：stock.<symbol>.<field> 路径解析与取值格式化 --------------------

const char* const kBindFields[] = {"price", "chg",       "pct",       "open",      "high",
                                   "low",   "last_close", "avg_price", "amplitude", "turnover",
                                   "volume", "amount",    "pe",        "pb",        "float_cap",
                                   "market_cap", "time"};

bool ValidBindField(const char* f, size_t n) {
    for (const char* k : kBindFields) {
        if (std::strlen(k) == n && std::memcmp(k, f, n) == 0) return true;
    }
    return false;
}

// 纯函数（DataHub 的 match 在 agent worker 线程也会调，绝不碰 g_subs 等 LVGL 态）。
// symbol 可含 '.'（usAAPL.OQ）→ field 取最后一个 '.' 之后那段。
bool ParseBindPath(const std::string& path, std::string* sym, std::string* field) {
    constexpr size_t kPfx = 6;  // "stock."
    if (path.rfind("stock.", 0) != 0) return false;
    size_t dot = path.rfind('.');
    if (dot <= kPfx || dot + 1 >= path.size()) return false;
    std::string s = path.substr(kPfx, dot - kPfx);
    if (!ValidBindField(path.c_str() + dot + 1, path.size() - dot - 1)) return false;
    if (!ValidSymbol(s.c_str())) return false;
    if (sym) *sym = std::move(s);
    if (field) *field = path.substr(dot + 1);
    return true;
}

bool MatchBindPath(const std::string& path) { return ParseBindPath(path, nullptr, nullptr); }

// 大数字人性化（成交量/成交额，原始单位 股/元）。
void FormatBigNum(double v, char* buf, size_t cap) {
    if (v >= 1e12) std::snprintf(buf, cap, "%.2f万亿", v / 1e12);
    else if (v >= 1e8) std::snprintf(buf, cap, "%.2f亿", v / 1e8);
    else if (v >= 1e4) std::snprintf(buf, cap, "%.1f万", v / 1e4);
    else std::snprintf(buf, cap, "%.0f", v);
}

// 市值原始单位就是"亿"（币种随市场，A股人民币/HK港元/US美元——LLM 知道自己在展示哪个市场）。
void FormatCapYi(float yi, char* buf, size_t cap) {
    if (yi <= 0) std::snprintf(buf, cap, "--");
    else if (yi >= 1e4f) std::snprintf(buf, cap, "%.2f万亿", yi / 1e4f);
    else std::snprintf(buf, cap, "%.1f亿", yi);
}

// 单字段 → 展示文本。推送侧统一格式化：绑定 label 不需要 fmt，拿到即可读。
void FormatBindField(const StockQuote& q, const char* field, char* buf, size_t cap) {
    auto pos2 = [&](float v) {  // 仅正数有意义的价格类字段，缺失显 "--"
        if (v > 0) std::snprintf(buf, cap, "%.2f", v);
        else std::snprintf(buf, cap, "--");
    };
    if (std::strcmp(field, "price") == 0) pos2(q.current);
    else if (std::strcmp(field, "chg") == 0) std::snprintf(buf, cap, "%+.2f", q.chg);
    else if (std::strcmp(field, "pct") == 0) std::snprintf(buf, cap, "%+.2f%%", q.percent);
    else if (std::strcmp(field, "open") == 0) pos2(q.open);
    else if (std::strcmp(field, "high") == 0) pos2(q.high);
    else if (std::strcmp(field, "low") == 0) pos2(q.low);
    else if (std::strcmp(field, "last_close") == 0) pos2(q.last_close);
    else if (std::strcmp(field, "avg_price") == 0) pos2(q.avg_price);
    else if (std::strcmp(field, "amplitude") == 0) std::snprintf(buf, cap, "%.2f%%", q.amplitude);
    else if (std::strcmp(field, "turnover") == 0) std::snprintf(buf, cap, "%.2f%%", q.turnover_rate);
    else if (std::strcmp(field, "volume") == 0) FormatBigNum(q.volume, buf, cap);
    else if (std::strcmp(field, "amount") == 0) FormatBigNum(q.amount, buf, cap);
    else if (std::strcmp(field, "pe") == 0) pos2(q.pe);
    else if (std::strcmp(field, "pb") == 0) pos2(q.pb);
    else if (std::strcmp(field, "float_cap") == 0) FormatCapYi(q.float_cap_yi, buf, cap);
    else if (std::strcmp(field, "market_cap") == 0) FormatCapYi(q.total_cap_yi, buf, cap);
    else if (std::strcmp(field, "time") == 0) {
        if (ClockReady()) BjClockText(buf, cap);
        else std::snprintf(buf, cap, "--");
    } else {
        std::snprintf(buf, cap, "--");
    }
}

// 一次报价落地 → 推全字段。Push 对未被绑定的路径静默忽略，故不必知道谁绑了什么。
void PushAllFields(const std::string& sym, const StockQuote& q) {
    char buf[32];
    const std::string base = "stock." + sym + ".";
    for (const char* f : kBindFields) {
        FormatBindField(q, f, buf, sizeof(buf));
        pi_card::DataHub::Instance().Push(base + f, buf);
    }
}

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

// bind 订阅的抓取入队（与控件的 SubmitFetch 同构，只是状态记在 BindSub 上）。
void SubmitSubFetch(const std::string& sym, BindSub& sub, uint32_t now) {
    stock_fetch_worker::Request req;
    req.session = sub.session;
    req.kind = Kind::Quote;
    std::strncpy(req.symbol, sym.c_str(), sizeof(req.symbol) - 1);
    if (!stock_fetch_worker::Submit(req)) return;  // 队列满：保持未 in-flight，下轮重试
    sub.inflight = true;
    sub.last_quote_ms = now;
}

void DrainResults() {
    stock_fetch_worker::Result res;
    while (stock_fetch_worker::Poll(res)) {
        StockCtx* c = nullptr;
        for (auto* w : g_widgets) {
            if (w->session == res.session) { c = w; break; }
        }
        if (c == nullptr) {
            // 非图表控件的结果：试并配 bind 订阅（session 也不匹配 = 已退订/已作废）。
            if (res.kind == Kind::Quote) {
                for (auto& [sym, sub] : g_subs) {
                    if (sub.session != res.session) continue;
                    sub.inflight = false;
                    if (res.ok) {
                        sub.quote = *res.quote;
                        sub.valid_once = true;
                        PushAllFields(sym, sub.quote);
                    }
                    break;
                }
            }
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
    // bind 订阅：与控件报价同一 policy（盘中 5s / 闭市 60s / 失败快重试）。没有控件那样的
    // 可见性可查（绑定的是 label，不归本模块管），有绑定即拉——卡片删除即退订，自然止损。
    for (auto& [sym, sub] : g_subs) {
        StockFetchScheduler::State st;
        st.now_ms = now;
        st.clock_ready = clock_ready;
        st.bj_epoch = bj;
        st.in_session = market_hours::inSession(sym.c_str(), bj);
        st.last_fetch_at_ms = sub.last_quote_ms;
        st.valid = sub.valid_once;
        st.in_flight = sub.inflight;
        if (StockFetchScheduler::shouldFetch(st, kQuotePolicy)) SubmitSubFetch(sym, sub, now);
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
    if (g_widgets.empty() && g_subs.empty() && g_timer != nullptr) lv_timer_pause(g_timer);
}

// ---- Phase4：DataHub 动态路径 provider 回调（均 LVGL 线程）------------------

void OnBindAcquire(const std::string& path) {
    std::string sym, field;
    if (!ParseBindPath(path, &sym, &field)) return;
    auto it = g_subs.find(sym);
    if (it == g_subs.end()) {
        if (static_cast<int>(g_subs.size()) >= kMaxBindSymbols) {
            ESP_LOGW(TAG, "bind symbol cap (%d) reached, %s not subscribed", kMaxBindSymbols,
                     sym.c_str());
            pi_card::DataHub::Instance().Push(path, "超限");  // 屏上可见的拒绝反馈
            return;  // 不进 g_bound_paths → release 时不误退别人的订阅
        }
        it = g_subs.try_emplace(sym).first;
        it->second.session = NextSession();
        ESP_LOGI(TAG, "bind sub %s created, %d subs live", sym.c_str(),
                 static_cast<int>(g_subs.size()));
    }
    BindSub& sub = it->second;
    sub.refcount++;
    g_bound_paths.insert(path);
    EnsureModuleStarted();
    if (sub.quote.valid) {  // 同 symbol 已有缓存（第二个字段绑定/复绑）：立即补种该路径
        char buf[32];
        FormatBindField(sub.quote, field.c_str(), buf, sizeof(buf));
        pi_card::DataHub::Instance().Push(path, buf);
    }
    if (!sub.valid_once && !sub.inflight) SubmitSubFetch(sym, sub, lv_tick_get());
}

void OnBindRelease(const std::string& path) {
    if (g_bound_paths.erase(path) == 0) return;  // 超限被拒的路径：无订阅可退
    std::string sym;
    if (!ParseBindPath(path, &sym, nullptr)) return;
    auto it = g_subs.find(sym);
    if (it == g_subs.end()) return;
    if (--it->second.refcount <= 0) {
        g_subs.erase(it);  // 在途结果凭 session 无主 → Drain 侧 FreePayload
        ESP_LOGI(TAG, "bind sub %s dropped, %d subs live", sym.c_str(),
                 static_cast<int>(g_subs.size()));
        if (g_widgets.empty() && g_subs.empty() && g_timer != nullptr) lv_timer_pause(g_timer);
    }
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

// ---------------------------------------------------------------------------
// Phase4：动态绑定 provider 注册（pi_card::Init 调，先于 agent 首次校验）。
// 字段清单单一来源 kBindFields —— hint（校验报错）与 DESC 片段（工具描述）都由它拼出。
namespace {
std::string JoinBindFields() {
    std::string s;
    for (const char* f : kBindFields) {
        if (!s.empty()) s += '|';
        s += f;
    }
    return s;
}
}  // namespace

void RegisterBindProvider() {
    static std::string hint;  // provider 持 const char*，须常驻
    if (hint.empty()) {
        hint = "stock paths are stock.<symbol>.<field>; symbol in Tencent format from the stock "
               "tool (sh/sz+6 digits, hk+5 digits, usTICKER.N/.OQ); field one of " +
               JoinBindFields();
    }
    pi_card::DataHub::DynProvider p;
    p.prefix = "stock.";
    p.hint = hint.c_str();
    p.match = MatchBindPath;
    p.on_first_acquire = OnBindAcquire;
    p.on_last_release = OnBindRelease;
    pi_card::DataHub::Instance().RegisterDynProvider(p);
}

const char* BindPathsDesc() {
    static std::string s;
    if (s.empty()) {
        s = "DYNAMIC stock quote paths: bind \"stock.<symbol>.<field>\" on a label (str, "
            "pre-formatted, no fmt needed; auto-refreshes ~5s while that market is open, sparser "
            "closed). symbol = Tencent format from the stock tool (sh600519/hk00700/usAAPL.OQ); "
            "field: " +
            JoinBindFields() +
            " (pct like \"+1.23%\", market_cap like \"1.57万亿\"; pb is A-share only, shows \"--\" "
            "elsewhere). Values arrive async -- render shows \"--\" first, fills within seconds. "
            "At most " +
            std::to_string(kMaxBindSymbols) + " symbols subscribed at once.";
    }
    return s.c_str();
}

}  // namespace pi_card_stock
