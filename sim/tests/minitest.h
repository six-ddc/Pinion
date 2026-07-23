// minitest.h —— 极简宿主单测框架（纯 C++17，仅标准库，仓库离线不引第三方依赖）。
//
// 用法：
//   #include "minitest.h"
//   TEST_CASE("加法") {
//       CHECK(1 + 1 == 2);
//       CHECK_EQ(1 + 1, 2);
//   }
//   int main() { return RUN_ALL_TESTS(); }
//
// - TEST_CASE 在静态初始化期把自己挂进全局链表，无需手动注册。
// - CHECK/CHECK_EQ 失败只计数打印，不中止当前 case（后续断言继续跑）。
// - RUN_ALL_TESTS 跑完打印 "N cases, M checks, K failures"，K>0 时返回 1。
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace minitest {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& AllCases() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { AllCases().push_back({name, fn}); }
};

// 当前 case 的检查计数/失败计数（跑一个 case 前清零，跑完累加进全局）。
inline int& CurChecks() {
    static int n = 0;
    return n;
}
inline int& CurFailures() {
    static int n = 0;
    return n;
}

inline void ReportFail(const char* file, int line, const std::string& msg) {
    ++CurFailures();
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace minitest

#define TEST_CASE(name)                                                            \
    static void MINITEST_FN_(__LINE__)();                                          \
    static ::minitest::Registrar MINITEST_REG_(__LINE__)(name, &MINITEST_FN_(__LINE__)); \
    static void MINITEST_FN_(__LINE__)()

// 两层展开保证 __LINE__ 被求值成数字再拼接，避免同名冲突。
#define MINITEST_CONCAT_(a, b) a##b
#define MINITEST_CONCAT(a, b) MINITEST_CONCAT_(a, b)
#define MINITEST_FN_(line) MINITEST_CONCAT(minitest_case_fn_, line)
#define MINITEST_REG_(line) MINITEST_CONCAT(minitest_case_reg_, line)

#define CHECK(expr)                                                    \
    do {                                                               \
        ++::minitest::CurChecks();                                     \
        if (!(expr)) {                                                 \
            ::minitest::ReportFail(__FILE__, __LINE__, #expr);         \
        }                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        ++::minitest::CurChecks();                                       \
        auto minitest_a_ = (a);                                          \
        auto minitest_b_ = (b);                                          \
        if (!(minitest_a_ == minitest_b_)) {                             \
            std::ostringstream minitest_os_;                             \
            minitest_os_ << #a << " == " << #b << " (got " << minitest_a_ \
                         << " vs " << minitest_b_ << ")";                 \
            ::minitest::ReportFail(__FILE__, __LINE__, minitest_os_.str()); \
        }                                                                 \
    } while (0)

// 遍历执行所有 TEST_CASE，打印汇总，返回适合作为 main() 返回值的退出码。
inline int RUN_ALL_TESTS() {
    int total_checks = 0;
    int total_failures = 0;
    int case_count = 0;
    for (auto& c : ::minitest::AllCases()) {
        ::minitest::CurChecks() = 0;
        ::minitest::CurFailures() = 0;
        std::printf("[ RUN ] %s\n", c.name);
        c.fn();
        if (::minitest::CurFailures() > 0) {
            std::printf("[ FAIL] %s (%d checks, %d failures)\n", c.name, ::minitest::CurChecks(),
                        ::minitest::CurFailures());
        } else {
            std::printf("[ OK  ] %s (%d checks)\n", c.name, ::minitest::CurChecks());
        }
        total_checks += ::minitest::CurChecks();
        total_failures += ::minitest::CurFailures();
        ++case_count;
    }
    std::printf("%d cases, %d checks, %d failures\n", case_count, total_checks, total_failures);
    return total_failures > 0 ? 1 : 0;
}
