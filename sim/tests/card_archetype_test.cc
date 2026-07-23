// card_archetype_test.cc —— 把 docs/CARD_V2.md §1.5 的四张完整 archetype JSON 原样喂 Solve，
// 断言全部 S 不变量 + 关键几何点（设备控制卡 slider 吃满剩余、表格卡 num 列右对齐等）。
#include <cmath>

#include "minitest.h"
#include "solver_test_util.h"

using namespace tu;

static void CheckS1S2(const Layout& lo, int vw) {
    for (const auto& g : lo.grids) {
        int maxr = MaxRow(g);
        for (int r = 0; r <= maxr; ++r) {
            auto row = RowCells(g, r);
            for (const auto& c : row) {
                CHECK(c.x >= 0);
                CHECK(c.x + c.w <= vw);
            }
            for (size_t i = 0; i < row.size(); ++i)
                for (size_t j = i + 1; j < row.size(); ++j) {
                    const auto& a = row[i];
                    const auto& b = row[j];
                    CHECK((a.x + a.w <= b.x) || (b.x + b.w <= a.x));
                }
        }
    }
}

static void CheckS3(const Layout& lo, int vw) {
    for (const auto& g : lo.grids) {
        if (g.track_w.empty()) continue;  // cells 形态不走共享轨道
        int sum = 0;
        for (int t : g.track_w) sum += t;
        int used = sum + kStackGap * ((int)g.track_w.size() - 1);
        CHECK(used <= vw);
    }
}

// ① 设备控制（cells 头部 + 控制排 + divider + 底部按钮）
TEST_CASE("archetype ①: 设备控制卡") {
    const char* J = R"({"display":"overlay","root":[
      {"cells":[
        {"type":"label","role":"eyebrow","text":"PI CONTROL"},
        {"type":"label","role":"title","text":"设备控制"}]},
      {"cells":[
        {"type":"icon","icon":"volume"},
        {"type":"slider","bind":"audio.volume"},
        {"type":"label","role":"value","bind":"audio.volume","fmt":"%d%%"}]},
      {"cells":[
        {"type":"icon","icon":"sun"},
        {"type":"slider","bind":"display.brightness"},
        {"type":"label","role":"value","bind":"display.brightness","fmt":"%d%%"}]},
      {"cells":[
        {"type":"icon","icon":"battery"},
        {"type":"bar","bind":"battery.level"},
        {"type":"label","role":"value","tone":"dim","bind":"battery.level","fmt":"%d%%"}]},
      {"cells":[{"type":"divider"}]},
      {"cells":[
        {"type":"icon","icon":"wifi","tone":"ok"},
        {"type":"label","role":"label","text":"网络"},
        {"type":"switch","checked":true,"side":"end"}]},
      {"cells":[
        {"type":"button","variant":"ghost","text":"取消","on_click":[{"do":"close"}]},
        {"type":"button","variant":"primary","text":"确认",
         "on_click":[{"do":"report","text":"确认"},{"do":"close"}]}]}
    ]})";
    Layout lo;
    cJSON* intent = SolveJson(J, 532, lo);
    CheckS1S2(lo, 532);
    CHECK_EQ((int)lo.grids.size(), 7);
    // 头部：eyebrow / title 各占一行。
    CHECK_EQ(MaxRow(lo.grids[0]), 1);
    // 音量排：slider 吃满剩余（最宽），value 贴右。
    {
        auto row = RowCells(lo.grids[1], 0);
        int slider_w = 0, value_r = 0, icon_x = 999;
        for (const auto& c : row) {
            if (c.ci == 0) icon_x = c.x;
            if (c.ci == 1) slider_w = c.w;
            if (c.ci == 2) value_r = c.x + c.w;
        }
        CHECK_EQ(icon_x, 0);
        CHECK(slider_w >= 160);
        CHECK_EQ(value_r, 532);
    }
    // divider 独占一行满宽。
    CHECK_EQ(lo.grids[4].cells[0].w, 532);
    // 网络排：switch side:end 贴右。
    {
        auto row = RowCells(lo.grids[5], 0);
        for (const auto& c : row)
            if (c.ci == 2) CHECK_EQ(c.x + c.w, 532);
    }
    // 底部两 button 均分整行（全 growable）。
    {
        auto row = RowCells(lo.grids[6], 0);
        CHECK_EQ((int)row.size(), 2);
        CHECK(std::abs(row[0].w - row[1].w) <= 1);
        CHECK(row[0].x == 0);
        CHECK(row[1].x + row[1].w == 532);
    }
    cJSON_Delete(intent);
}

// ② 表格（rows + cols 表头 + divider + num 右对齐）
TEST_CASE("archetype ②: 表格卡") {
    const char* J = R"({"display":"overlay","root":[
      {"cells":[{"type":"label","role":"title","text":"网格表格"}]},
      {"cols":[{"title":"项目"},{"title":"今日","num":true},{"title":"昨日","num":true}],
       "rows":[
        [{"type":"label","text":"温度"},{"type":"label","text":"24"},{"type":"label","text":"22"}],
        [{"type":"label","text":"湿度"},{"type":"label","text":"60"},{"type":"label","text":"55"}],
        [{"type":"label","text":"气压"},{"type":"label","text":"1013"},{"type":"label","text":"1009"}]
      ]}
    ]})";
    Layout lo;
    cJSON* intent = SolveJson(J, 532, lo);
    CheckS1S2(lo, 532);
    CheckS3(lo, 532);
    const Grid& tbl = lo.grids[1];
    CHECK_EQ(tbl.ncol, 3);
    // 表头行(0) + divider(1) + 3 数据行 → MaxRow == 4。
    CHECK_EQ(MaxRow(tbl), 4);
    // num 列（col 1、2）右对齐不截断。
    for (const auto& c : tbl.cells) {
        if (c.ci < 0) continue;
        if (c.col == 1 || c.col == 2) {
            CHECK_EQ(c.align, std::string("end"));
            CHECK(!c.truncate);
        }
    }
    cJSON_Delete(intent);
}

// ③ 表单（rows：每行 label + 控件，共享轨道）
TEST_CASE("archetype ③: 表单卡") {
    const char* J = R"({"display":"overlay","root":[
      {"cells":[
        {"type":"label","role":"eyebrow","text":"FORM"},
        {"type":"label","role":"title","text":"偏好设置"}]},
      {"cols":[{"title":"项"},{}],
       "rows":[
        [{"type":"label","role":"label","text":"亮度"},
         {"type":"slider","id":"bri","min":0,"max":100,"value":60}],
        [{"type":"label","role":"label","text":"静音"},
         {"type":"switch","id":"mute","checked":false}],
        [{"type":"label","role":"label","text":"模式"},
         {"type":"choice","id":"mode","options":["省电","均衡","性能"],"value":1}]
      ]},
      {"cells":[{"type":"button","variant":"primary","text":"保存",
        "on_click":[{"do":"report","text":"已保存"},{"do":"close"}]}]}
    ]})";
    Layout lo;
    cJSON* intent = SolveJson(J, 532, lo);
    CheckS1S2(lo, 532);
    CheckS3(lo, 532);
    const Grid& form = lo.grids[1];
    CHECK_EQ(form.ncol, 2);
    // 控件列（col 1）吃剩余：slider/choice 是真正 stretch 的控件，填满第 1 列轨道；
    // switch（F1 修复）逐 cell 判断 stretch==false，保持自己 52px 契约宽 + own align(end)，
    // 不因所在列是 FILL 列就被整列拉伸。表头(row0)+divider(row1) 之后，数据行依原序是
    // row2=slider、row3=switch、row4=choice。
    for (const auto& c : form.cells) {
        if (c.col != 1 || c.ci < 0) continue;
        if (c.row == 3) {
            CHECK_EQ(c.w, 52);
            CHECK_EQ(c.align, std::string("end"));
        } else {
            CHECK_EQ(c.w, form.track_w[1]);
        }
    }
    CHECK(form.track_w[1] >= 160);
    // label 列（col 0）比控件列窄（内容宽）。
    CHECK(form.track_w[0] < form.track_w[1]);
    cJSON_Delete(intent);
}

// ④ 菜单（rows：每行单 button）
TEST_CASE("archetype ④: 菜单卡") {
    const char* J = R"({"display":"overlay","root":[
      {"cells":[
        {"type":"label","role":"eyebrow","text":"SELECT"},
        {"type":"label","role":"title","text":"选择操作"}]},
      {"rows":[
        [{"type":"button","text":"新建对话","on_click":[{"do":"report","text":"新建对话"},{"do":"close"}]}],
        [{"type":"button","text":"导出记录","on_click":[{"do":"report","text":"导出记录"},{"do":"close"}]}],
        [{"type":"button","text":"清空历史","on_click":[{"do":"report","text":"清空历史"},{"do":"close"}]}],
        [{"type":"button","variant":"ghost","text":"关闭","on_click":[{"do":"close"}]}]
      ]}
    ]})";
    Layout lo;
    cJSON* intent = SolveJson(J, 532, lo);
    CheckS1S2(lo, 532);
    const Grid& menu = lo.grids[1];
    CHECK_EQ(menu.ncol, 1);
    CHECK_EQ(MaxRow(menu), 3);
    for (const auto& c : menu.cells) CHECK_EQ(c.w, 532);  // 全宽菜单项
    cJSON_Delete(intent);
}
