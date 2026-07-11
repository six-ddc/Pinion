// 日期字符串 → epoch 的纯 C 实现，header-only 无外部依赖
#pragma once
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>

// 输入 "YYYY-MM-DD"（长度需 ≥ 10）→ 该日 15:00（收盘时刻近似）的 epoch（本地时区）
// 非法/短字符串/空指针返回 0
inline uint32_t dateStrToEpoch(const char* yyyy_mm_dd) {
    if (!yyyy_mm_dd || strlen(yyyy_mm_dd) < 10) return 0;
    struct tm t = {};
    t.tm_year = atoi(yyyy_mm_dd) - 1900;
    t.tm_mon  = atoi(yyyy_mm_dd + 5) - 1;
    t.tm_mday = atoi(yyyy_mm_dd + 8);
    t.tm_hour = 15;  // 收盘时刻近似
    t.tm_isdst = -1;
    return (uint32_t)mktime(&t);
}
