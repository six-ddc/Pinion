// minute_parse.h
// 腾讯财经分时接口响应里"行字符串 → 价格 / 时间戳"的纯解析。
// header-only、无外部依赖、可脱离硬件单测。
//
// minute / 5day 响应里每分钟一行字符串，格式 "HHMM price [cumVolume] [cumAmount]"，
// 至少前两段（HHMM + price）。本头只解析单行 → POD；JSON 数组拆解 + ChartSeries
// 数组装填由 stock_api.cc 完成。
//
// downsampleParallelArrays 把"5 日分时 ~1955 点 → 目标 200 点"的等距取样 + 强制保末点
// 抽出来，host 可测；和 ChartSeries 内部布局解耦。

#ifndef API_MINUTE_PARSE_H
#define API_MINUTE_PARSE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace minute_parse {

// "YYYYMMDD" + "HHMM" → local-time epoch（mktime 走系统时区；host 测请固定 TZ=UTC）。
// 任一参数缺失 / 长度不足 → 返回 0。
inline uint32_t hhmmToEpoch(const char* date_yyyymmdd, const char* hhmm) {
    if (!date_yyyymmdd || std::strlen(date_yyyymmdd) < 8) return 0;
    if (!hhmm || std::strlen(hhmm) < 4) return 0;
    std::tm t{};
    t.tm_year = (date_yyyymmdd[0]-'0')*1000 + (date_yyyymmdd[1]-'0')*100 +
                (date_yyyymmdd[2]-'0')*10   + (date_yyyymmdd[3]-'0') - 1900;
    t.tm_mon  = (date_yyyymmdd[4]-'0')*10   + (date_yyyymmdd[5]-'0') - 1;
    t.tm_mday = (date_yyyymmdd[6]-'0')*10   + (date_yyyymmdd[7]-'0');
    t.tm_hour = (hhmm[0]-'0')*10 + (hhmm[1]-'0');
    t.tm_min  = (hhmm[2]-'0')*10 + (hhmm[3]-'0');
    t.tm_isdst = -1;
    return static_cast<uint32_t>(std::mktime(&t));
}

// 解析单行 "HHMM p [...]"：成功填 outHhmm[5]（NUL）+ *outPrice，返回 true。
// price <= 0 / line 太短 / nullptr → false。
inline bool parseMinuteLine(const char* line, char outHhmm[5], float* outPrice) {
    if (!line || !outHhmm || !outPrice) return false;
    if (std::strlen(line) < 7) return false;   // 至少 "HHMM p"
    std::memcpy(outHhmm, line, 4);
    outHhmm[4] = '\0';
    const char* p = line + 4;
    while (*p == ' ') p++;
    float price = std::strtof(p, nullptr);
    if (!(price > 0)) return false;
    *outPrice = price;
    return true;
}

// 5 日分时常规 ~1955 点 → 目标 max。等距步长取样 + 强制保末点。
// 对若干 parallel arrays 同步取样，调用方决定哪些字段参与（不强制 ChartSeries 内部布局）。
//
// arrays: 数组指针的列表（每个对应一个 float 数据列）。
// arrCount: 列数。
// timestamps: 可选时间戳列（uint32_t）；nullptr → 不动。
// count: 当前点数 outparam（in/out）。capacity: 数组容量上限。
//
// 行为：
//   - *count <= targetMax → 立即返回，不改动
//   - 否则按 ceil(count/targetMax) 为步长，取索引 0/step/2step/... 到末尾
//   - 若取样后末点不等于原末点 → 追加原末点（容量不足时覆盖末位）
inline void downsampleParallelArrays(float** arrays, size_t arrCount,
                                     uint32_t* timestamps,
                                     size_t* count, size_t capacity,
                                     size_t targetMax) {
    if (!count || !arrays || arrCount == 0 || capacity == 0) return;
    size_t n = *count;
    if (n == 0 || n <= targetMax || targetMax == 0) return;

    size_t step = (n + targetMax - 1) / targetMax;
    size_t outN = 0;
    for (size_t i = 0; i < n; i += step) {
        for (size_t k = 0; k < arrCount; k++) {
            arrays[k][outN] = arrays[k][i];
        }
        if (timestamps) timestamps[outN] = timestamps[i];
        outN++;
    }

    // 强制保末点（用 array[0] 作为"代表列"判定 last 是否已经取到）
    bool lastIncluded = (outN > 0 && arrays[0][outN - 1] == arrays[0][n - 1]);
    if (!lastIncluded) {
        if (outN < capacity) {
            for (size_t k = 0; k < arrCount; k++) arrays[k][outN] = arrays[k][n - 1];
            if (timestamps) timestamps[outN] = timestamps[n - 1];
            outN++;
        } else if (outN > 0) {
            for (size_t k = 0; k < arrCount; k++) arrays[k][outN - 1] = arrays[k][n - 1];
            if (timestamps) timestamps[outN - 1] = timestamps[n - 1];
        }
    }
    *count = outN;
}

// 同步反转 parallel arrays（用于 5 日分时把"今天→5 天前"转成"5 天前→今天"）。
inline void reverseParallelArrays(float** arrays, size_t arrCount,
                                   uint32_t* timestamps, size_t count) {
    if (count < 2 || !arrays || arrCount == 0) return;
    for (size_t i = 0, j = count - 1; i < j; i++, j--) {
        for (size_t k = 0; k < arrCount; k++) {
            float tmp = arrays[k][i];
            arrays[k][i] = arrays[k][j];
            arrays[k][j] = tmp;
        }
        if (timestamps) {
            uint32_t ts = timestamps[i];
            timestamps[i] = timestamps[j];
            timestamps[j] = ts;
        }
    }
}

}  // namespace minute_parse

#endif  // API_MINUTE_PARSE_H
