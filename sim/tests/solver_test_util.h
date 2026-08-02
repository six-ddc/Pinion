// solver_test_util.h —— card_solver 宿主单测的共享脚手架：
//   - MeasureStub：ASCII=8px / CJK=16px，按 role 字号比例缩放（§2.6 注释）。
//   - JSON 解析/查询小工具，把 Solve 的 layout cJSON 拆成便于断言的结构。
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "pi_card_solver.h"

namespace tu {

using pi_card::solver::kStackGap;

// role → 字号（基准 20）。与 §1.4 role 字号阶梯对齐。
inline int RoleSize(int role) {
    switch (role) {
        case pi_card::solver::kRoleTitle:
            return 30;
        case pi_card::solver::kRoleHeading:
            return 24;
        case pi_card::solver::kRoleEyebrow:
        case pi_card::solver::kRoleKicker:
        case pi_card::solver::kRoleSection:
        case pi_card::solver::kRoleCaption:
            return 14;
        case pi_card::solver::kRoleLabel:
        case pi_card::solver::kRoleValue:
        case pi_card::solver::kRoleNone:
        default:
            return 20;
    }
}

// 等宽近似测量：逐 UTF-8 码点，ASCII 记 8px 基准、非 ASCII（CJK）记 16px 基准，
// 再按 size/20 缩放。mono 不改宽度（stub 里 mono 与正文同等宽）。
inline int MeasureStub(const char* s, int role, bool /*mono*/, void* /*ctx*/) {
    if (!s) return 0;
    int size = RoleSize(role);
    int w = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;) {
        unsigned char b = *p;
        if (b < 0x80) {
            w += 8 * size / 20;
            ++p;
        } else {
            // 多字节起始 + 跳过续字节，整簇按一个 CJK 宽计。
            w += 16 * size / 20;
            ++p;
            while ((*p & 0xC0) == 0x80) ++p;
        }
    }
    return w;
}

// ---- layout 查询工具 ----

struct Placed {
    int gi = 0, ci = 0, row = 0, col = 0, span = 1;
    int x = 0, w = 0;
    std::string align;
    bool truncate = false;
    std::string wrap;  // solver 三态字段（渲染器实际消费）："wrap"|"ellipsis"|"nowrap"
};

struct Grid {
    int ncol = 0;
    int h_hint = 0;
    std::vector<int> track_w;
    std::vector<Placed> cells;
};

struct Layout {
    cJSON* raw = nullptr;
    std::vector<Grid> grids;
    ~Layout() {
        if (raw) cJSON_Delete(raw);
    }
};

inline int JInt(const cJSON* o, const char* k, int def = 0) {
    const cJSON* it = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(it) ? it->valueint : def;
}
inline std::string JStr(const cJSON* o, const char* k) {
    const cJSON* it = cJSON_GetObjectItem(o, k);
    return (it && cJSON_IsString(it)) ? it->valuestring : "";
}
inline bool JBool(const cJSON* o, const char* k) {
    const cJSON* it = cJSON_GetObjectItem(o, k);
    return cJSON_IsTrue(it);
}

// 解析 Solve 输出到便于断言的结构。接管 raw 的所有权（Layout 析构里 Delete）。
inline void Parse(cJSON* raw, Layout& out) {
    out.raw = raw;
    const cJSON* grids = cJSON_GetObjectItem(raw, "grids");
    const cJSON* g = nullptr;
    cJSON_ArrayForEach(g, grids) {
        Grid grid;
        grid.ncol = JInt(g, "ncol");
        grid.h_hint = JInt(g, "h_hint");
        const cJSON* tw = cJSON_GetObjectItem(g, "track_w");
        const cJSON* t = nullptr;
        cJSON_ArrayForEach(t, tw) grid.track_w.push_back(t->valueint);
        const cJSON* cells = cJSON_GetObjectItem(g, "cells");
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cells) {
            Placed p;
            p.gi = JInt(c, "gi");
            p.ci = JInt(c, "ci");
            p.row = JInt(c, "row");
            p.col = JInt(c, "col");
            p.span = JInt(c, "span", 1);
            p.x = JInt(c, "x");
            p.w = JInt(c, "w");
            p.align = JStr(c, "align");
            p.truncate = JBool(c, "truncate");
            p.wrap = JStr(c, "wrap");
            grid.cells.push_back(p);
        }
        out.grids.push_back(std::move(grid));
    }
}

// 解析意图 JSON 串 + 跑 Solve + Parse。root 取信封里的 "root" 数组（若顶层已是数组则直接用）。
// 返回的 cJSON 意图树由 caller 持有；Layout 单独持 raw。
inline cJSON* SolveJson(const char* json, int viewport_w, Layout& out, const cJSON** intent_keep = nullptr) {
    cJSON* intent = cJSON_Parse(json);
    const cJSON* root = cJSON_IsArray(intent) ? intent : cJSON_GetObjectItem(intent, "root");
    const cJSON* data = cJSON_GetObjectItem(intent, "data");
    pi_card::solver::Input in;
    in.root = root;
    in.data = data;
    in.viewport_w = viewport_w;
    in.gap = pi_card::solver::kStackGap;
    in.measure = &MeasureStub;
    in.measure_ctx = nullptr;
    // bind 类型 stub：只有 "str." 前缀的路径报字符串（2），其余报未知（0）——既有用例的
    // bind（battery.*/stock.* 等）保持纯启发式行为不受影响，新用例用 str.* 显式驱动。
    in.bind_kind = [](const char* path, void*) -> int {
        return (path != nullptr && std::strncmp(path, "str.", 4) == 0) ? 2 : 0;
    };
    in.bind_kind_ctx = nullptr;
    cJSON* layout = pi_card::solver::Solve(in);
    Parse(layout, out);
    if (intent_keep) *intent_keep = intent;
    return intent;
}

// 收集某 grid 内属于某 row 的 cell（按 col 排序前的原序）。
inline std::vector<Placed> RowCells(const Grid& g, int row) {
    std::vector<Placed> r;
    for (const auto& c : g.cells)
        if (c.row == row) r.push_back(c);
    return r;
}

inline int MaxRow(const Grid& g) {
    int m = -1;
    for (const auto& c : g.cells) m = c.row > m ? c.row : m;
    return m;
}

}  // namespace tu
