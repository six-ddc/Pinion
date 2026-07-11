// 行情图布局/坐标的纯计算函数，header-only 无外部依赖，可脱离硬件单测
#pragma once
#include <cstddef>

// MetalioClaw4 720x720 详情画布：canvas 688x500，左边距 16。
// tab 移入标题行后画布放大到整块图表区（原 420 → 500）。
constexpr int kChartW = 688;
constexpr int kChartH = 500;
constexpr int kChartX = 16;
constexpr int kChartYOffset = 0;

// K 线柱子布局：定间距 + 整组居中。渲染和 hover 共用，避免对不齐。
struct KlineLayout {
    int slotW;
    int bodyW;
    int leftMargin;
    int rightMargin;
    int startWickX;   // 第 0 根的 wickX
};

inline KlineLayout computeKlineLayout(size_t count, int chartW = kChartW) {
    KlineLayout L{};
    if (count == 0) return L;
    L.slotW = (int)(chartW / (int)count);
    if (L.slotW < 2) L.slotW = 2;
    L.bodyW = L.slotW > 1 ? L.slotW - 1 : 2;
    if (L.bodyW < 2) L.bodyW = 2;
    if (L.bodyW > 16) L.bodyW = 16;
    L.leftMargin = L.bodyW / 2;
    L.rightMargin = L.bodyW - 1 - L.leftMargin;
    int contentW = (int)(count - 1) * L.slotW + L.bodyW;
    L.startWickX = L.leftMargin + (chartW - contentW) / 2;
    if (L.startWickX < L.leftMargin) L.startWickX = L.leftMargin;
    return L;
}

// 触摸 x（chart 内坐标）→ K 线柱子 idx，做最近邻舍入
inline int klineHoverIdx(int relX, const KlineLayout& L, size_t count) {
    if (L.slotW <= 0 || count == 0) return 0;
    int idx = (relX - L.startWickX + L.slotW / 2) / L.slotW;
    if (idx < 0) idx = 0;
    if (idx >= (int)count) idx = (int)count - 1;
    return idx;
}

// 价格 → chart 内 y 坐标。yMax 在 y=0，yMin 在 y=chartH-1
inline int priceToChartY(float price, float yMin, float yMax, int chartH = kChartH) {
    if (!(yMax > yMin)) return chartH / 2;
    float t = (price - yMin) / (yMax - yMin);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (int)((1.0f - t) * (float)(chartH - 1) + 0.5f);
}

// 价格 → lv_chart 整数值。lv_coord_t 默认是 int16_t（LV_USE_LARGE_COORD=0），
// 直接喂 (int32_t)(price*100) 在 price>327.67 时会环绕——给 yMax/yMin 之类的
// 边界值用一次就把整张图翻倒。统一映射到 [0, kChartYScale]，再 set_range(0, kChartYScale)
// 渲染，整数完全避开 int16 边界、精度还比 ×100 高（10000 步覆盖整段可视区间）。
constexpr int32_t kChartYScale = 10000;
inline int32_t priceToChartIntY(float price, float yMin, float yMax) {
    if (!(yMax > yMin)) return kChartYScale / 2;
    float t = (price - yMin) / (yMax - yMin);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (int32_t)(t * (float)kChartYScale + 0.5f);
}

// ---------------------------------------------------------------------------
// 行情图 Y 轴范围（dataLo/dataHi 是真实数据极值；yMin/yMax 是带留白后的轴上下界）
// ---------------------------------------------------------------------------

// K 线 Y 范围：sweep [lows[0..n) ∪ highs[0..n)] + 上下各 5% pad（最低 0.1% × dataLo）
struct KlineYRange {
    float dataLo;   // 原始最低（底部状态栏 H/L/Amp 用）
    float dataHi;   // 原始最高（底部状态栏 H/L/Amp 用）
    float yMin;     // 加 pad 后的 chart 下边界
    float yMax;     // 加 pad 后的 chart 上边界
    bool  valid;
};
inline KlineYRange computeKlineYRange(const float* lows, const float* highs, size_t n) {
    KlineYRange R{};
    if (n == 0 || !lows || !highs) return R;
    R.dataLo = lows[0]; R.dataHi = highs[0];
    for (size_t i = 1; i < n; i++) {
        if (lows[i]  < R.dataLo) R.dataLo = lows[i];
        if (highs[i] > R.dataHi) R.dataHi = highs[i];
    }
    float pad = (R.dataHi - R.dataLo) * 0.05f;
    float minPad = 0.001f * R.dataLo;
    if (pad < minPad) pad = minPad;
    R.yMin = R.dataLo - pad;
    R.yMax = R.dataHi + pad;
    R.valid = true;
    return R;
}

// 分时 Y 范围：上/下偏离独立计算，ref 不强制居中，但被 clamp 到中间 80% 区间
// （白线相对位置 ∈ [0.1, 0.9]），让单边行情把曲线撑满剩余 90% 空间
// dataLo/dataHi 是 points 的 raw min/max，不含 ref；yMin/yMax 含 ref
struct MinuteYRange {
    float dataLo;
    float dataHi;
    float yMin;
    float yMax;
    bool  valid;
};
inline MinuteYRange computeMinuteYRange(const float* points, size_t n, float ref) {
    MinuteYRange R{};
    if (n == 0 || !points || !(ref > 0)) return R;
    R.dataLo = points[0]; R.dataHi = points[0];
    for (size_t i = 1; i < n; i++) {
        if (points[i] < R.dataLo) R.dataLo = points[i];
        if (points[i] > R.dataHi) R.dataHi = points[i];
    }
    float upDev   = (R.dataHi > ref) ? (R.dataHi - ref) : 0.0f;
    float downDev = (R.dataLo < ref) ? (ref - R.dataLo) : 0.0f;
    float minDev = ref * 0.001f;
    if (upDev   < minDev) upDev   = minDev;
    if (downDev < minDev) downDev = minDev;
    upDev   *= 1.05f;
    downDev *= 1.05f;
    // p = ref 距 yMin 的比例（白线相对位置）。自然取 downDev/total 让数据正好填满；
    // clamp 到 [0.1, 0.9]：单边行情时把白线压到边缘 10% 处，反推 total 保证另一侧装得下
    constexpr float kMinRefPos = 0.1f;
    constexpr float kMaxRefPos = 0.9f;
    float total = upDev + downDev;
    float p = downDev / total;
    if (p < kMinRefPos) {
        p = kMinRefPos;
        total = upDev / (1.0f - p);
    } else if (p > kMaxRefPos) {
        p = kMaxRefPos;
        total = downDev / p;
    }
    R.yMin = ref - total * p;
    R.yMax = ref + total * (1.0f - p);
    R.valid = true;
    return R;
}

// 分时折线点的双色系列分配：交叉点同时画进 UP 和 DOWN，其他只进一边
enum class MinutePointSeries : int {
    UP_ONLY   = 0,
    DOWN_ONLY = 1,
    BOTH      = 2,
};
inline MinutePointSeries classifyMinutePoint(float cur, float prev, float ref, bool isFirst) {
    bool up = cur >= ref;
    if (isFirst) return up ? MinutePointSeries::UP_ONLY : MinutePointSeries::DOWN_ONLY;
    bool prevUp = prev >= ref;
    if (up != prevUp) return MinutePointSeries::BOTH;
    return up ? MinutePointSeries::UP_ONLY : MinutePointSeries::DOWN_ONLY;
}

// ---------------------------------------------------------------------------
// 单根 K 线蜡烛的几何（不实际绘制；交给调用方走 lv_canvas_draw_*）
// ---------------------------------------------------------------------------
struct CandleGeom {
    bool valid;        // 输入价格至少有一个非正 → 跳过
    bool inCanvas;     // body 部分越出 chart 宽度 → 跳过（避免和邻居视觉重叠）
    int  wickX;
    int  wickYTop;     // priceToChartY(high) — 数值更小（屏幕更靠上）
    int  wickYBottom;  // priceToChartY(low)  — 数值更大（屏幕更靠下）
    int  bodyX;
    int  bodyW;
    int  bodyTop;      // 实体矩形 y 起点
    int  bodyH;        // 实体矩形高（>= 2，open==close 也至少 2 px）
    bool up;           // close >= open（含等价为阳）
};
inline CandleGeom computeCandleGeom(float open, float close, float high, float low,
                                    float yMin, float yMax,
                                    const KlineLayout& L, size_t i,
                                    int chartW = kChartW, int chartH = kChartH) {
    CandleGeom G{};
    G.valid = (open > 0) && (close > 0) && (high > 0) && (low > 0);
    if (!G.valid) return G;

    G.wickX = L.startWickX + (int)i * L.slotW;
    G.inCanvas = (G.wickX - L.leftMargin >= 0) && (G.wickX + L.rightMargin < chartW);
    if (!G.inCanvas) return G;

    int yOpen  = priceToChartY(open,  yMin, yMax, chartH);
    int yClose = priceToChartY(close, yMin, yMax, chartH);
    G.wickYTop    = priceToChartY(high, yMin, yMax, chartH);
    G.wickYBottom = priceToChartY(low,  yMin, yMax, chartH);
    G.up = close >= open;
    G.bodyTop  = yOpen < yClose ? yOpen : yClose;
    int bodyBottom = yOpen > yClose ? yOpen : yClose;
    G.bodyH = bodyBottom - G.bodyTop + 1;
    if (G.bodyH < 2) G.bodyH = 2;
    G.bodyX = G.wickX - L.leftMargin;
    G.bodyW = L.bodyW;
    return G;
}
