// K 线缓存的窗口切片 / 增量合并 / 拉取触发判定的纯逻辑，header-only 无外部依赖。
// 只接受 plain 数组指针 + size，与缓存存储解耦，可脱离硬件单测。
#pragma once
#include <cstddef>
#include <cstdint>

// === 离散缩放档位（点击 zoomPill 循环）===
// 仅"从当前时间往前展开"的 K 线视图，不考虑横向平移；档位只控制窗口根数。
struct ZoomPreset {
    uint16_t bars;
    const char* labelDaily;
    const char* labelWeekly;
};
constexpr ZoomPreset kZoomPresets[] = {
    {22,   "1M",  "5M"},
    {60,   "3M",  "1Y"},
    {150,  "6M",  "3Y"},
    {250,  "1Y",  "5Y"},
    {500,  "2Y",  "10Y"},
    {1000, "4Y",  "20Y"},
};
constexpr int kZoomPresetCount = (int)(sizeof(kZoomPresets) / sizeof(kZoomPresets[0]));

inline const ZoomPreset& zoomPreset(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kZoomPresetCount) idx = kZoomPresetCount - 1;
    return kZoomPresets[idx];
}
inline const char* zoomPresetLabel(int idx, bool isWeekly) {
    const ZoomPreset& p = zoomPreset(idx);
    return isWeekly ? p.labelWeekly : p.labelDaily;
}

// 当 head_at_edge=true（已知拉不到更早历史），档位上限 = "cache 能完整覆盖的最大档"
// natural，**再多放一档**：只要 cache 严格多过 natural 档的 bars，就允许停留在
// natural+1。语义是"5Y 撑满后再点应能切到 10Y 标签，哪怕只能显示 7Y 数据"——
// 不让标签被卡死在'已经被超过的档位'上。natural 档恰好等于 cache 时不放行
// （没溢出意义）；natural 已经是最高档时也不再 +1。
inline int maxUsableZoomIdx(size_t cacheCount, bool headAtEdge) {
    if (!headAtEdge) return kZoomPresetCount - 1;
    int natural = -1;
    for (int i = kZoomPresetCount - 1; i >= 0; i--) {
        if ((size_t)kZoomPresets[i].bars <= cacheCount) { natural = i; break; }
    }
    if (natural < 0) return 0;  // cache 比最小档还小：兜底 0
    if (natural + 1 < kZoomPresetCount &&
        cacheCount > (size_t)kZoomPresets[natural].bars) {
        return natural + 1;
    }
    return natural;
}

// 下一个可用档位索引：循环 + 跳过受 head_at_edge 限制的高档位
inline int nextZoomIdx(int currentIdx, size_t cacheCount, bool headAtEdge) {
    int maxIdx = maxUsableZoomIdx(cacheCount, headAtEdge);
    int next = currentIdx + 1;
    if (next > maxIdx) next = 0;
    return next;
}

// 柱/折线渲染阈值：windowSize ≤ 60 走柱形（slot ≥ 6px 看得清），> 60 折线
constexpr size_t kBarRenderThreshold = 60;
inline bool shouldUseBarsForWindow(size_t windowSize) {
    return windowSize > 0 && windowSize <= kBarRenderThreshold;
}

// === 窗口（右锚定，从当前时间往前展开）===
struct KlineWindow {
    size_t start;
    size_t end;     // inclusive；当 size==0 时 start==end==0 仅占位
    size_t size;
};

// 缩放：保持窗口右端锚定在 cache 末尾（最新），左端按 target 展开；cache 不够则缩窗口
inline KlineWindow computeWindowAfterZoom(size_t cacheCount, uint16_t target) {
    KlineWindow w{};
    if (cacheCount == 0 || target == 0) return w;
    size_t effective = (size_t)target < cacheCount ? (size_t)target : cacheCount;
    w.size  = effective;
    w.end   = cacheCount - 1;
    w.start = cacheCount - effective;
    return w;
}

// === 拉取触发判定 ===
struct KlineFetchPlan {
    bool needHistory;     // 主任务是否要发 RANGE 请求
    int  requestCount;    // 请求 count（0 = 不拉）
};

// 历史增量批量大小 + 上限。一次最多拉 1000 根，覆盖最大档位（1000 根 ≈ 4 年日 K）。
constexpr int kHistoryBatchSize  = 100;
constexpr int kHistoryRequestMax = 1000;

// 缩放放大后 cache 不够：缺多少补多少，最少 kHistoryBatchSize，最多 kHistoryRequestMax
inline KlineFetchPlan planZoomFetch(size_t cacheCount, uint16_t target,
                                    bool headAtEdge, bool inFlight) {
    KlineFetchPlan p{};
    if (headAtEdge || inFlight) return p;
    if ((size_t)target <= cacheCount) return p;
    int need = (int)((size_t)target - cacheCount);
    int req = need < kHistoryBatchSize ? kHistoryBatchSize : need;
    if (req > kHistoryRequestMax) req = kHistoryRequestMax;
    p.needHistory  = true;
    p.requestCount = req;
    return p;
}

// === 增量合并（prepend 历史）===
// 输入：
//   cacheTs[0..cacheCount)  — 当前缓存时间戳（升序，cacheTs[0]=最早）
//   cacheCap                — 缓存容量上限（cacheCount + insertCount 不会越过）
//   batchTs[0..batchCount)  — 接口返回的一批，升序
// 决策：取 batch 中所有 < cacheTs[0] 的连续前缀（batch 升序所以一定是前缀）prepend；
// 容量受限时再截断；cache 空时整批接收。
struct KlineMergePlan {
    size_t insertCount;          // 取 batch[0..insertCount) prepend
    bool   batchFullyOverlapped; // batch 非空但全部 ≥ cacheTs[0]，调用方应置 head_at_edge
};
inline KlineMergePlan mergePrependPlan(
    const uint32_t* cacheTs, size_t cacheCount, size_t cacheCap,
    const uint32_t* batchTs, size_t batchCount)
{
    KlineMergePlan m{};
    if (batchCount == 0 || !batchTs) return m;
    if (cacheCap == 0) return m;
    size_t available = cacheCap > cacheCount ? cacheCap - cacheCount : 0;
    if (available == 0) return m;

    if (cacheCount == 0 || !cacheTs) {
        m.insertCount = batchCount < available ? batchCount : available;
        return m;
    }

    uint32_t pivot = cacheTs[0];
    size_t younger = 0;
    while (younger < batchCount && batchTs[younger] < pivot) younger++;
    if (younger == 0) {
        m.batchFullyOverlapped = true;
        return m;
    }
    m.insertCount = younger < available ? younger : available;
    return m;
}
