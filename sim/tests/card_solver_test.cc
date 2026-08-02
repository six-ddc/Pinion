// card_solver_test.cc —— CARD V2 布局求解器（docs/CARD_V2.md §2）宿主单测。
// 覆盖 §7.1 的 S1-S8 不变量、fmt 代表串、cells 折行、rows 分宽、choice 冲突、side:end 等。
#include "minitest.h"
#include "solver_test_util.h"

using namespace tu;

// ---------------------------------------------------------------------------
// 通用不变量扫描：S1(不重叠) / S2(不越界、SPAN_ALL 满宽) 对任意 layout 成立。
// ---------------------------------------------------------------------------
static void CheckS1S2(const Layout& lo, int vw) {
    for (const auto& g : lo.grids) {
        int maxr = MaxRow(g);
        for (int r = 0; r <= maxr; ++r) {
            auto row = RowCells(g, r);
            // S2：每 cell 落在 [0, vw] 内。
            for (const auto& c : row) {
                CHECK(c.x >= 0);
                CHECK(c.x + c.w <= vw);
                CHECK(c.w >= 0);
            }
            // S1：同行两两不交。
            for (size_t i = 0; i < row.size(); ++i)
                for (size_t j = i + 1; j < row.size(); ++j) {
                    const auto& a = row[i];
                    const auto& b = row[j];
                    bool disjoint = (a.x + a.w <= b.x) || (b.x + b.w <= a.x);
                    CHECK(disjoint);
                }
        }
    }
}

// ===========================================================================
// fmt 代表串（§2.2）
// ===========================================================================
TEST_CASE("S: fmt 代表串宽度 —— %d / %d%% / dBm / .1f") {
    // %d → "88888"（5×8=40）；%d%% → "888%"（4×8=32）。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"label","role":"value","bind":"a.b","fmt":"%d"},
          {"type":"label","role":"value","bind":"a.b","fmt":"%d%%"}]}
    ]})",
                              532, lo);
    // 两个数值 label 在同一行（都 INLINE end），宽度按代表串。
    auto row = RowCells(lo.grids[0], 0);
    // 找出各自宽度：第一个 "%d"→40，第二个 "%d%%"→32。
    int w0 = -1, w1 = -1;
    for (const auto& c : row) {
        if (c.ci == 0) w0 = c.w;
        if (c.ci == 1) w1 = c.w;
    }
    CHECK_EQ(w0, 40);
    CHECK_EQ(w1, 32);
    // 数值 label 右对齐、不截断（S6）。
    for (const auto& c : row) {
        CHECK_EQ(c.align, std::string("end"));
        CHECK(!c.truncate);
    }
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("S: %s / bind_data 字符串 label 走文本列 64px 兜底") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"data":{"name":"x"},"root":[
        {"cells":[{"type":"label","bind_data":"name"}]}
    ]})",
                              532, lo);
    // 字符串 label 不塌陷到 0：min 兜底 64；单个可拉 label 独占行会吸满整行（左对齐文本列）。
    auto& c = lo.grids[0].cells[0];
    CHECK(c.w >= 64);
    CHECK_EQ(c.align, std::string("start"));
    cJSON_Delete(intent);
}

// ===========================================================================
// S1-S8 逐条
// ===========================================================================
TEST_CASE("S1/S2: 音量控制排不重叠不越界，slider 吃满剩余") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"icon","icon":"volume"},
          {"type":"slider","bind":"audio.volume"},
          {"type":"label","role":"value","bind":"audio.volume","fmt":"%d%%"}]}
    ]})",
                              532, lo);
    CheckS1S2(lo, 532);
    auto row = RowCells(lo.grids[0], 0);
    CHECK_EQ((int)row.size(), 3);
    // slider 是最宽的 cell 且 ≥160（S4）。
    int slider_w = 0;
    for (const auto& c : row)
        if (c.ci == 1) slider_w = c.w;
    CHECK(slider_w >= 160);
    // value 靠最右（end 锚定）。
    int value_right = 0;
    for (const auto& c : row)
        if (c.ci == 2) value_right = c.x + c.w;
    CHECK_EQ(value_right, 532);
    cJSON_Delete(intent);
}

TEST_CASE("S4: slider 最小 160、交互行高 ≥ 44") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"slider","bind":"a.b"}]}
    ]})",
                              200, lo);  // 窄视口也须 ≥160
    CHECK(lo.grids[0].cells[0].w >= 160);
    CHECK(lo.grids[0].h_hint >= 44);
    cJSON_Delete(intent);
}

TEST_CASE("S5: 同一 grid 内所有 arc 等径") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"arc","value":30}]},
        {"cells":[{"type":"arc","value":60}]}
    ]})",
                              532, lo);
    // 两个 arc 分属两行（同 grid? 这里是两个 grid 块）——改成同一 cells 内验证等径。
    Layout lo2;
    cJSON* intent2 = SolveJson(R"({"root":[
        {"cells":[{"type":"arc","value":30},{"type":"arc","value":60}]}
    ]})",
                               532, lo2);
    int w0 = lo2.grids[0].cells[0].w;
    int w1 = lo2.grids[0].cells[1].w;
    CHECK_EQ(w0, w1);
    cJSON_Delete(intent);
    cJSON_Delete(intent2);
}

TEST_CASE("S6: num 列 end + 不截断 + mono；文本列可截断") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"项目"},{"title":"今日","num":true}],
         "rows":[
           [{"type":"label","text":"很长很长很长很长很长很长很长的名称"},{"type":"label","text":"24"}]
         ]}
    ]})",
                              160, lo);  // 窄视口逼文本列压缩到内容宽以下 → 截断
    // 找 num 列 cell（col=1）与文本列 cell（col=0），跳过表头行。
    for (const auto& g : lo.grids) {
        for (const auto& c : g.cells) {
            if (c.ci < 0) continue;  // 合成表头/divider
        }
    }
    // 表头会自动生成，最后一行才是数据行。定位数据行 num cell。
    const Grid& g = lo.grids[0];
    int maxr = MaxRow(g);
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col == 1) {  // num 列
            CHECK_EQ(c.align, std::string("end"));
            CHECK(!c.truncate);
        }
        if (c.col == 0) {  // 文本列，内容超轨道 → 截断
            CHECK_EQ(c.align, std::string("start"));
            CHECK(c.truncate);
        }
    }
    CheckS1S2(lo, 160);
    cJSON_Delete(intent);
}

TEST_CASE("S7: 单个不可拉 cell 独占行居中") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"icon","icon":"star"}]}
    ]})",
                              532, lo);
    auto& c = lo.grids[0].cells[0];
    CHECK_EQ(c.align, std::string("center"));
    CHECK_EQ(c.x, (532 - c.w) / 2);
    cJSON_Delete(intent);
}

TEST_CASE("S8: cells 折行确定性 —— 8 个不等宽 button 折成多行均分") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"button","text":"新建"},
          {"type":"button","text":"打开文件"},
          {"type":"button","text":"保存"},
          {"type":"button","text":"另存为副本"},
          {"type":"button","text":"删"},
          {"type":"button","text":"导出全部记录"},
          {"type":"button","text":"设置"},
          {"type":"button","text":"关闭"}]}
    ]})",
                              532, lo);
    CheckS1S2(lo, 532);
    const Grid& g = lo.grids[0];
    int maxr = MaxRow(g);
    CHECK(maxr >= 1);  // 至少折成两行
    // 每行填满整行（Σw + Σgap == vw），且剩余空间在各 button 间平摊（等宽差 = min_w 差）。
    for (int r = 0; r <= maxr; ++r) {
        auto row = RowCells(g, r);
        if (row.size() < 2) continue;
        int sum = 0;
        for (const auto& c : row) sum += c.w;
        int used = sum + kStackGap * ((int)row.size() - 1);
        CHECK_EQ(used, 532);  // 均分填满整行
    }
    cJSON_Delete(intent);
}

TEST_CASE("cells: 混入 label 后 button 退回内容宽（非均分）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"label","role":"label","text":"操作"},
          {"type":"button","text":"确认"},
          {"type":"button","text":"取消"}]}
    ]})",
                              532, lo);
    CheckS1S2(lo, 532);
    // button "确认"/"取消"：text 2CJK=32，pref=32+32=64，min=max(72,64)=72 → 内容宽 72，不均分。
    auto row = RowCells(lo.grids[0], 0);
    for (const auto& c : row) {
        if (c.ci == 1 || c.ci == 2) CHECK_EQ(c.w, 72);
    }
    cJSON_Delete(intent);
}

// ===========================================================================
// choice 冲突消解（R4）
// ===========================================================================
TEST_CASE("R4: cells 里 choice 前后夹 inline → choice 独占行，前后各自成行") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"label","role":"label","text":"前"},
          {"type":"choice","options":["A","B","C"]},
          {"type":"label","role":"label","text":"后"}]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    // 三个 cell 应在三个不同 row。
    int r_pre = -1, r_choice = -1, r_post = -1;
    for (const auto& c : g.cells) {
        if (c.ci == 0) r_pre = c.row;
        if (c.ci == 1) {
            r_choice = c.row;
            CHECK_EQ(c.w, 532);  // choice SPAN_ALL 满宽
        }
        if (c.ci == 2) r_post = c.row;
    }
    CHECK(r_pre != r_choice);
    CHECK(r_choice != r_post);
    CHECK(r_pre != r_post);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("R4: rows 多列行内 choice = FILL（填自己那一列，不满整行）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"项"},{}],
         "rows":[
           [{"type":"label","role":"label","text":"模式"},
            {"type":"choice","options":["省电","均衡","性能"]}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    int maxr = MaxRow(g);
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col == 1) {
            CHECK(c.w < 532);          // 不满整行
            CHECK_EQ(c.w, g.track_w[1]);  // 填满第 1 列轨道
        }
    }
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

// ===========================================================================
// side:"end"（R7）
// ===========================================================================
TEST_CASE("R7: 单个 side:end → 右靠") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"icon","icon":"wifi"},
          {"type":"label","role":"label","text":"网络"},
          {"type":"switch","checked":true,"side":"end"}]}
    ]})",
                              532, lo);
    auto row = RowCells(lo.grids[0], 0);
    for (const auto& c : row) {
        if (c.ci == 2) CHECK_EQ(c.x + c.w, 532);  // switch 贴右
        if (c.ci == 0) CHECK_EQ(c.x, 0);          // icon 贴左
    }
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("R7: 多个 side:end 顺序右靠；与 FILL 共存") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"slider","bind":"a.b"},
          {"type":"switch","side":"end"},
          {"type":"switch","side":"end"}]}
    ]})",
                              532, lo);
    auto row = RowCells(lo.grids[0], 0);
    int x1 = -1, w1 = 0, x2 = -1, w2 = 0, slider_r = 0;
    for (const auto& c : row) {
        if (c.ci == 0) slider_r = c.x + c.w;
        if (c.ci == 1) {
            x1 = c.x;
            w1 = c.w;
        }
        if (c.ci == 2) {
            x2 = c.x;
            w2 = c.w;
        }
    }
    // 两个 end switch 顺序排在右侧，最后一个贴右边缘。
    CHECK(x1 < x2);
    CHECK_EQ(x2 + w2, 532);
    // slider（FILL）右边缘不越过第一个 end cell。
    CHECK(slider_r <= x1);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

// ===========================================================================
// rows：列类型混排 / rem<0 压缩 / 自动 num 推断 / cols 表头装饰
// ===========================================================================
TEST_CASE("rows: fixed/num/stretch 混排，Σtrack + gap 不越界（S3）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"项"},{"title":"值","num":true}],
         "rows":[
           [{"type":"icon","icon":"cpu"},{"type":"label","text":"42"}],
           [{"type":"label","role":"label","text":"内存占用率"},{"type":"label","text":"88"}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    int sum = 0;
    for (int t : g.track_w) sum += t;
    int used = sum + kStackGap * (g.ncol - 1);
    CHECK(used <= 532);
    CHECK_EQ(g.ncol, 2);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("rows: rem<0 压缩 stretch 列，不压数值列") {
    // 极窄视口 + 宽数值列 → stretch 文本列被压，num 列保内容宽。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"名称"},{"title":"数值","num":true}],
         "rows":[
           [{"type":"label","text":"某个很长很长很长很长很长的名称文本"},
            {"type":"label","role":"value","fmt":"%d"}]
         ]}
    ]})",
                              160, lo);
    const Grid& g = lo.grids[0];
    // num 列（col 1）宽度 == 代表串 "88888" = 40（未被压）。
    CHECK_EQ(g.track_w[1], 40);
    int sum = g.track_w[0] + g.track_w[1] + kStackGap;
    CHECK(sum <= 160);
    CHECK(g.track_w[0] >= 0);
    CheckS1S2(lo, 160);
    cJSON_Delete(intent);
}

TEST_CASE("rows: 缺 cols 时数值列自动推断（role=value → 右对齐）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"rows":[
           [{"type":"label","role":"label","text":"温度"},{"type":"label","role":"value","fmt":"%d"}],
           [{"type":"label","role":"label","text":"湿度"},{"type":"label","role":"value","fmt":"%d"}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    CHECK_EQ(g.ncol, 2);
    // 无 cols → 无表头行；col 1 全 role=value → 推断 num → 右对齐。
    for (const auto& c : g.cells) {
        if (c.col == 1) {
            CHECK_EQ(c.align, std::string("end"));
            CHECK(!c.truncate);
        }
    }
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("rows: cols 表头自动生成 section 行 + divider（§2.5）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"项目"},{"title":"今日","num":true},{"title":"昨日","num":true}],
         "rows":[
           [{"type":"label","text":"温度"},{"type":"label","text":"24"},{"type":"label","text":"22"}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    // 行 0 = section 表头（3 个 title label），行 1 = divider（span 满宽），行 2 = 数据。
    auto r0 = RowCells(g, 0);
    CHECK_EQ((int)r0.size(), 3);
    auto r1 = RowCells(g, 1);
    CHECK_EQ((int)r1.size(), 1);
    CHECK_EQ(r1[0].w, 532);  // divider 满宽
    CHECK(MaxRow(g) >= 2);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("rows: 单列 button 菜单 → 每 button 满宽独占行") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"rows":[
           [{"type":"button","text":"新建对话"}],
           [{"type":"button","text":"导出记录"}],
           [{"type":"button","variant":"ghost","text":"关闭"}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    CHECK_EQ(g.ncol, 1);
    for (const auto& c : g.cells) CHECK_EQ(c.w, 532);  // 全宽按钮
    CHECK_EQ(MaxRow(g), 2);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("bind_rows: 动态展开 data 数组为行") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"data":{"tracks":["歌一","歌二","歌三"]},"root":[
        {"item":[{"type":"button","text":"{item}"}],"bind_rows":"tracks","max":8}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    // 3 个元素 → 3 行单 button。
    CHECK_EQ(MaxRow(g), 2);
    CHECK_EQ(g.ncol, 1);
    for (const auto& c : g.cells) CHECK_EQ(c.w, 532);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

// ===========================================================================
// F1: rows 列内混 slider/switch —— 逐 cell 判断 stretch，不因列类型是 FILL 就整列拉伸
// ===========================================================================
TEST_CASE("F1: rows 同列混 slider(FILL)+switch(固定) —— switch 保持52px右对齐, slider 仍撑满列轨道") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"rows":[
           [{"type":"label","role":"label","text":"亮度"},{"type":"slider","bind":"bri"}],
           [{"type":"label","role":"label","text":"静音"},{"type":"switch","checked":false}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    int slider_w = -1, switch_w = -1;
    std::string switch_align;
    for (const auto& c : g.cells) {
        if (c.col != 1) continue;
        if (c.row == 0) slider_w = c.w;
        if (c.row == 1) {
            switch_w = c.w;
            switch_align = c.align;
        }
    }
    // switch 不被拉伸成整列宽（不是 g.track_w[1]，而是自己的契约宽 52）。
    CHECK_EQ(switch_w, 52);
    CHECK_EQ(switch_align, std::string("end"));
    // 回归防护：slider 在同一列里仍然 FILL 撑满列轨道（不因 F1 修复退化）。
    CHECK_EQ(slider_w, g.track_w[1]);
    CHECK(slider_w >= 160);
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("F1: switch 落位在自己列轨道内右对齐，不越界不侵入相邻列") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"rows":[
           [{"type":"label","role":"label","text":"亮度调节这一行"},{"type":"slider","bind":"bri"}],
           [{"type":"label","role":"label","text":"静音"},{"type":"switch","checked":false}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    for (const auto& c : g.cells) {
        if (c.col == 1 && c.row == 1) {
            // switch 右边缘贴自己列轨道的右边缘（不是整卡右边缘，除非列轨道本身贴右）。
            CHECK_EQ(c.x + c.w, g.track_w[0] + kStackGap + g.track_w[1]);
        }
    }
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

// ===========================================================================
// F2: cols 全部 title 为空/缺失 —— 不生成 section 表头 + divider（cols 只传 num 元数据）
// ===========================================================================
TEST_CASE("F2: cols 无 title(仅 num 元数据) —— 不生成多余表头行/divider") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{},{"num":true}],
         "rows":[
           [{"type":"label","text":"温度"},{"type":"label","text":"24"}],
           [{"type":"label","text":"湿度"},{"type":"label","text":"60"}]
         ]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    // 无表头/divider：MaxRow 应等于数据行数-1（2 行数据 → MaxRow==1），不多出 2 行。
    CHECK_EQ(MaxRow(g), 1);
    // col 1 仍因 cols[1].num==true 被推断为数值列（右对齐）。
    for (const auto& c : RowCells(g, 0)) {
        if (c.col == 1) {
            CHECK_EQ(c.align, std::string("end"));
            CHECK(!c.truncate);
        }
    }
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

// ===========================================================================
// F6-1: side:"end" 在 SPAN_ALL 分支不再被吃掉（cells 单叶子独占整行的 caption/标题类）。
// 回归对象：16_stock_bind.json 底部 caption 带 side:end（role=caption → IsBlockRole →
// SPAN_ALL），过去只走 c.align（恒 start），side_end 被忽略。
// ===========================================================================
TEST_CASE("F6: SPAN_ALL 叶子 side:end → align 变 end（caption 独占行右靠）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"label","role":"caption","tone":"dim","text":"12:30 更新","side":"end"}]}
    ]})",
                              532, lo);
    auto& c = lo.grids[0].cells[0];
    CHECK_EQ(c.w, 532);      // SPAN_ALL 满宽不变
    CHECK_EQ(c.x, 0);
    CHECK_EQ(c.align, std::string("end"));  // F6 修复前恒为 "start"
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

TEST_CASE("F6: 16_stock_bind.json 回归 —— 末尾 caption(side:end) 独占行 align==end") {
    // 镜像语料 sim/tests/corpus/16_stock_bind.json 末尾的 grid 块。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"label","role":"caption","tone":"dim","bind":"stock.sh600519.time","side":"end"}]}
    ]})",
                              532, lo);
    auto& c = lo.grids[0].cells[0];
    CHECK_EQ(c.align, std::string("end"));
    CHECK_EQ(c.x + c.w, 532);  // 满宽容器右边缘 == viewport 右边缘（对齐口径双重印证）
    cJSON_Delete(intent);
}

// ===========================================================================
// F6-2: side:"end" 在 all-growable（均分）行里不再被忽略——退回自己内容宽、锚右，
// 不参与均分膨胀；其余 growable cell 在剩余空间里正常均分。
// ===========================================================================
TEST_CASE("F6: all-growable 行里 side:end 的 button 退回内容宽并锚右，不参与均分") {
    Layout lo;
    // 三个等价 growable button，其中最后一个带 side:end——若仍走旧的"三等分"逻辑，它会被
    // 拉伸到跟另外两个一样宽；修复后它应退回自己的内容宽（比均分窄）并贴在行尾。
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"button","text":"A"},
          {"type":"button","text":"B"},
          {"type":"button","text":"C","side":"end"}]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    auto row = RowCells(g, 0);
    int wA = -1, wB = -1, wC = -1, xC = -1;
    for (const auto& c : row) {
        if (c.ci == 0) wA = c.w;
        if (c.ci == 1) wB = c.w;
        if (c.ci == 2) {
            wC = c.w;
            xC = c.x;
        }
    }
    CHECK(wA > 0);
    CHECK_EQ(wA, wB);      // 非 end 的两个仍彼此均分相等
    CHECK(wC < wA);        // end 的退回内容宽，比均分窄（单字符按钮 pref 远小于三等分）
    CHECK_EQ(xC + wC, 532);  // 锚右贴边
    CheckS1S2(lo, 532);
    cJSON_Delete(intent);
}

// ===========================================================================
// F3: role:"value" 非数值静态文本不再被强制授予数值特权（不截断+右对齐+mono）。
// 回归对象：23_grid_auto.json 的 "Pinion Terminal"（role=value 纯品牌名文本）。
// 注意 text 必须长到能在下面 80px 视口里溢出文本列，否则 truncate 断言无从成立。
// ===========================================================================
TEST_CASE("F3: role:value 但文本非数值样式（品牌名）→ 按文本列处理，不强制右对齐不截断") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"字段"},{"title":"值"}],
         "rows":[[{"type":"label","text":"固件"},
                  {"type":"label","role":"value","text":"Pinion Terminal"}]]}
    ]})",
                              80, lo);  // 极窄视口压缩 TEXT 列到内容宽以下，逼出截断判定
    const Grid& g = lo.grids[0];
    int maxr = MaxRow(g);
    bool found = false;
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col != 1) continue;
        found = true;
        // 非数值文本：左对齐（普通文本列默认），允许截断（单行，不逼多行 WRAP）。
        CHECK_EQ(c.align, std::string("start"));
        CHECK(c.truncate);
    }
    CHECK(found);
    CheckS1S2(lo, 200);
    cJSON_Delete(intent);
}

TEST_CASE("F3: role:value + 数值样式静态文本（如 42%）仍走数值列右对齐不截断") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"字段"},{"title":"值"}],
         "rows":[[{"type":"label","text":"电量"},
                  {"type":"label","role":"value","text":"42%"}]]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    int maxr = MaxRow(g);
    bool found = false;
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col != 1) continue;
        found = true;
        CHECK_EQ(c.align, std::string("end"));
        CHECK(!c.truncate);
    }
    CHECK(found);
    cJSON_Delete(intent);
}

// ===========================================================================
// F4: 文本列 DOT 截断判定不能被 pref_w 的 clamp≤400 污染——轨道宽落在
// (400, 真实测量宽) 区间时必须仍判定截断。
// ===========================================================================
TEST_CASE("F4: 轨道宽落在 (400,真实测量宽) 之间 —— 截断判定用未 clamp 的真实宽") {
    // 30 个 CJK 字符，MeasureStub 按 16px/字（kRoleNone size20）计，真实宽 480 > clamp 400。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"名称"}],
         "rows":[[{"type":"label",
                   "text":"很长很长很长很长很长很长很长很长很长很长很长很长很长很长很长的名称文本超长测试超长"}]]}
    ]})",
                              450, lo);  // 单列 stretch 吃满整行 → track==450，落在 (400,480) 之间
    const Grid& g = lo.grids[0];
    CHECK_EQ(g.track_w[0], 450);  // 未越过修复前后都成立的前置条件：track 确实落在 (400,480)
    int maxr = MaxRow(g);
    bool found = false;
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col != 0) continue;
        found = true;
        CHECK(c.truncate);  // 修复前：pref_w(clamp 400) > w(450) 为 false → 漏判不截断
    }
    CHECK(found);
    CheckS1S2(lo, 450);
    cJSON_Delete(intent);
}

// ===========================================================================
// G: 文本排布三态（wrap 字段）—— 修复"单行钳制误伤独占行正文"回归引入的交叉点契约。
// 回归对象：01_confirm.json/04_wrap_stress.json（cells 里独占一行的正文被误判单行截断丢
// 内容）、25_grid_tall.json（cols.num 强制数值列里非数值契约文本轨道宽算窄断词换行）。
// ===========================================================================
TEST_CASE("G-WRAP: cells 独占一行的正文 label → wrap=wrap，不截断，允许折行长高") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"label","role":"label",
                   "text":"这是一段很长很长很长很长很长很长很长很长很长的说明文字用来验证独占整行时允许折行显示完整"}]}
    ]})",
                              300, lo);
    auto& c = lo.grids[0].cells[0];
    CHECK_EQ(c.wrap, std::string("wrap"));
    CHECK(!c.truncate);
    CheckS1S2(lo, 300);
    cJSON_Delete(intent);
}

TEST_CASE("G-ELLIPSIS: cells 里 label 跟 button 挤同一行 → wrap=ellipsis 单行截断（不独占）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[
          {"type":"label","role":"label","text":"操作说明文字"},
          {"type":"button","text":"确认"}]}
    ]})",
                              200, lo);
    const Grid& g = lo.grids[0];
    bool found = false;
    for (const auto& c : g.cells) {
        if (c.ci == 0) {  // label：跟按钮同排，不再独占整行
            found = true;
            CHECK_EQ(c.wrap, std::string("ellipsis"));
        }
    }
    CHECK(found);
    CheckS1S2(lo, 200);
    cJSON_Delete(intent);
}

TEST_CASE("G-ELLIPSIS: rows 文本列恒 ellipsis（即使这个 cell 当下没超轨道，同列其它行可能超）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"名称"},{"title":"值"}],
         "rows":[[{"type":"label","text":"短"},{"type":"label","text":"x"}]]}
    ]})",
                              532, lo);
    const Grid& g = lo.grids[0];
    int maxr = MaxRow(g);
    bool found = false;
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col == 0) {
            found = true;
            CHECK_EQ(c.wrap, std::string("ellipsis"));
            CHECK(!c.truncate);  // 精确判定仍是"没超宽不截断"，但 wrap 恒 ellipsis（钳高）
        }
    }
    CHECK(found);
    cJSON_Delete(intent);
}

TEST_CASE("G-NOWRAP: cols.num 强制数值列，非数值契约文本 cell 轨道宽用真实测量宽（25_grid_tall 回归防护）") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"KEY"},{"title":"VAL","num":true}],
         "rows":[
           [{"type":"label","text":"行01"},{"type":"label","role":"value","text":"v01"}],
           [{"type":"label","text":"行02"},{"type":"label","role":"value","text":"v22"}]
         ]}
    ]})",
                              300, lo);
    const Grid& g = lo.grids[0];
    // VAL 列（col=1，role:value 但非数值契约——无 mono/无 bind/"v01" 不满足 TextLooksNumeric）
    // 轨道宽须 ≥ 真实测量宽，否则断词换行（历史回归：用 clamp 过的 pref_w 而不是 natural_w）。
    int expect_min = MeasureStub("v22", pi_card::solver::kRoleValue, false, nullptr);
    CHECK(g.track_w[1] >= expect_min);
    int maxr = MaxRow(g);
    bool found = false;
    for (const auto& c : RowCells(g, maxr)) {
        if (c.col == 1) {
            found = true;
            CHECK_EQ(c.wrap, std::string("nowrap"));
            CHECK(!c.truncate);
        }
    }
    CHECK(found);
    CheckS1S2(lo, 300);
    cJSON_Delete(intent);
}

TEST_CASE("H1: 全数值列表格（无 stretch 列）剩余宽度摊给所有列，表格铺满整幅") {
    // 真机行情卡实录：四列全是数值 bind → 全 NUM 轨、无 stretch 列，修前剩余宽度悬空、
    // 表格停在自然宽靠左（右半空）。修后 Σtrack + gap×(ncol-1) == vw。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"名称"},{"title":"现价","num":true},{"title":"涨跌","num":true},{"title":"涨跌幅","num":true}],
         "rows":[
           [{"type":"label","role":"value","bind":"stock.a.price","text":"茅台"},
            {"type":"label","role":"value","bind":"stock.a.price"},
            {"type":"label","role":"value","bind":"stock.a.chg"},
            {"type":"label","role":"value","bind":"stock.a.pct"}],
           [{"type":"label","role":"value","bind":"stock.b.price","text":"腾讯"},
            {"type":"label","role":"value","bind":"stock.b.price"},
            {"type":"label","role":"value","bind":"stock.b.chg"},
            {"type":"label","role":"value","bind":"stock.b.pct"}]
         ]}
    ]})",
                              600, lo);
    const Grid& g = lo.grids[0];
    int sum = 0;
    for (int t : g.track_w) sum += t;
    CHECK_EQ(sum + kStackGap * (static_cast<int>(g.track_w.size()) - 1), 600);
    CheckS1S2(lo, 600);
    cJSON_Delete(intent);
}

TEST_CASE("H2: 数值 bind 无 fmt → 代表串兜底加宽到 '88888.88'；静态数值 text 直接量真身") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"K"},{"title":"V","num":true}],
         "rows":[
           [{"type":"label","text":"现价"},{"type":"label","role":"value","bind":"stock.a.price"}],
           [{"type":"label","text":"指数"},{"type":"label","role":"value","text":"3832.26","mono":true}]
         ]}
    ]})",
                              600, lo);
    const Grid& g = lo.grids[0];
    // 无 fmt 数值 bind：轨道 ≥ "88888.88" 代表串宽（修前只有 "88888"，活值 7 字符被左裁）。
    int rep_w = MeasureStub("88888.88", pi_card::solver::kRoleValue, true, nullptr);
    // 静态数值 text "3832.26"：轨道 ≥ 真身测量宽（修前量的是 5 字符代表串）。
    int text_w = MeasureStub("3832.26", pi_card::solver::kRoleValue, true, nullptr);
    int need = rep_w > text_w ? rep_w : text_w;
    CHECK(g.track_w[1] >= need);
    CheckS1S2(lo, 600);
    cJSON_Delete(intent);
}

TEST_CASE("H3: 表头标题参与定轨——窄数值列的 title 不再折行") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{"title":"名"},{"title":"涨跌幅百分比","num":true}],
         "rows":[
           [{"type":"label","text":"甲"},{"type":"label","role":"value","text":"1","mono":true}]
         ]}
    ]})",
                              600, lo);
    const Grid& g = lo.grids[0];
    int title_w = MeasureStub("涨跌幅百分比", pi_card::solver::kRoleSection, true, nullptr);
    CHECK(g.track_w[1] >= title_w);
    CheckS1S2(lo, 600);
    cJSON_Delete(intent);
}

TEST_CASE("H4: 独占行的 side:end 控件右锚定，不再被 S7 默认居中吞掉") {
    // 标题/眉标恒 SPAN_ALL 独占行，模型想放"标题右上角"的角标按钮只能落下一行——side:end
    // 是它仅存的靠右意图（真机行情卡刷新钮实录：修前孤零零居中）。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cells":[{"type":"label","role":"eyebrow","text":"行情"},
                  {"type":"label","role":"title","text":"自选股"},
                  {"type":"button","icon":"refresh-cw","variant":"ghost","side":"end"}]}
    ]})",
                              600, lo);
    const Grid& g = lo.grids[0];
    bool found = false;
    for (const auto& c : g.cells) {
        if (c.row == 2) {  // row0=eyebrow row1=title row2=button
            found = true;
            CHECK_EQ(c.align, std::string("end"));
            CHECK_EQ(c.x + c.w, 600);
        }
    }
    CHECK(found);
    // 对照：无 side:end 的独占行控件维持 S7 居中。
    Layout lo2;
    cJSON* intent2 = SolveJson(R"({"root":[
        {"cells":[{"type":"switch","checked":true}]}
    ]})",
                               600, lo2);
    bool found2 = false;
    for (const auto& c : lo2.grids[0].cells) {
        // switch 契约 align 默认 end，但独占行无 side:end 声明 → S7 居中
        found2 = true;
        CHECK_EQ(c.align, std::string("center"));
    }
    CHECK(found2);
    cJSON_Delete(intent);
    cJSON_Delete(intent2);
}

TEST_CASE("H5: cols.num 摁在 %s 字符串列上被否决——该列降级文本列吸收剩余，值不再硬裁") {
    // 真机「连接详情」实录：KV 表 cols:[{},{num:true}]，值列全是 fmt %s 的字符串 bind，
    // 修前值列被摁成 64px 固定数值轨（NOWRAP）→ "CHINA MOBILE" 硬裁。
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{},{"num":true}],
         "rows":[
           [{"type":"label","text":"SSID"},{"type":"label","bind":"net.ssid","fmt":"%s"}],
           [{"type":"label","text":"运营商"},{"type":"label","bind":"net.operator","fmt":"%s"}]
         ]}
    ]})",
                              600, lo);
    const Grid& g = lo.grids[0];
    int sum = 0;
    for (int t : g.track_w) sum += t;
    CHECK_EQ(sum + kStackGap, 600);       // 两列铺满
    CHECK(g.track_w[1] > 200);            // 值列吃到了剩余宽度（不再是 64px 兜底轨）
    for (const auto& c : g.cells) {
        if (c.col == 1 && c.ci >= 0) {
            CHECK_EQ(c.wrap, std::string("ellipsis"));       // 几何降级：文本列语义
            CHECK_EQ(c.align, std::string("end"));           // 对齐保住 num 声明意图（与表头一致）
        }
    }
    CheckS1S2(lo, 600);
    cJSON_Delete(intent);
}

TEST_CASE("H6: bind_kind 注入的字符串路径（无 fmt 无 mono 线索）也走文本列，mono 不再误判") {
    Layout lo;
    cJSON* intent = SolveJson(R"({"root":[
        {"cols":[{},{"num":true}],
         "rows":[
           [{"type":"label","text":"IP"},{"type":"label","bind":"str.ip","mono":true}],
           [{"type":"label","text":"信号"},{"type":"label","bind":"battery.level","fmt":"%d"}]
         ]}
    ]})",
                              600, lo);
    const Grid& g = lo.grids[0];
    // str.ip：类型提示=字符串 → 即使挂了 mono 也按文本列（列被否决降级，吸收剩余）。
    int sum = 0;
    for (int t : g.track_w) sum += t;
    CHECK_EQ(sum + kStackGap, 600);
    CHECK(g.track_w[1] > 200);
    // 对照：数值 bind（未知类型走启发式）在无字符串证据的表里仍是数值列——单独一张表验证。
    Layout lo2;
    cJSON* intent2 = SolveJson(R"({"root":[
        {"cols":[{},{"num":true}],
         "rows":[[{"type":"label","text":"电量"},{"type":"label","bind":"battery.level","fmt":"%d%%","mono":true}]]}
    ]})",
                               600, lo2);
    bool nowrap_found = false;
    for (const auto& c : lo2.grids[0].cells) {
        if (c.col == 1 && c.ci >= 0) {
            nowrap_found = true;
            CHECK_EQ(c.wrap, std::string("nowrap"));
        }
    }
    CHECK(nowrap_found);
    cJSON_Delete(intent);
    cJSON_Delete(intent2);
}

int main() {
    return RUN_ALL_TESTS();
}
