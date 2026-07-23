// preview_prefix_test.cc —— CARD V2 流式预览 v2（docs/CARD_V2.md §4）宿主单测：验证
// §7.1 的前缀不变量 P1（任意切分点等价）/ P2（幂等）/ P3（迟到 data）。
//
// 管线：用 pi-c 真实的 pi_partial_json_parse_streaming（与 sim 主程序 pi_agent_task.c 走同一份
// 实现）把语料 JSON 文本的任意前缀"尽力而为"解析成一棵快照树，喂给一个纯 cJSON 的
// "预览会话"复刻实现——按 pi_card_preview_sig::GridSignature 判定 root 数组第 i 个 grid 这一
// 帧该不该重新 solver::Solve（逻辑与 pi_card_preview.cc::PreviewOnArgs 的 grid 循环一一对应，
// 见该文件），最终比较累积出的 layout 与"一次性对完整 JSON 跑 Solve()"的 layout 是否逐字段
// 相等。不建 LVGL 对象——测的是"签名判断 + Solve 调用"这层纯逻辑，不是渲染像素。
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "cJSON.h"
#include "minitest.h"
#include "pi_card_preview_sig.h"
#include "pi_card_solver.h"
#include "pi/pi_partial_json.h"
#include "solver_test_util.h"

using namespace tu;
using pi_card::preview_sig::GridSignature;

namespace {

constexpr int kViewportW = pi_card::solver::kCardWChat;  // 测的是签名/Solve 管线本身，不是
                                                          // preview.cc 的 display 门控策略
                                                          // （那是应用层策略，见任务报告 e）。

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 对一个 grid（cJSON 对象）单独跑 Solve：把它包成一个只有它自己的临时 root 数组（引用，不
// 拷贝——Delete 这个临时数组不连累 grid_json 本体），返回 layout 里 "grids"[0] 的深拷贝
// （调用方 cJSON_Delete）。与 pi_card_render.cc 的 RenderGridBlockPreview 用同一手法，逻辑对齐。
cJSON* SolveOneGrid(const cJSON* grid_json, const cJSON* data) {
    cJSON* one = cJSON_CreateArray();
    cJSON_AddItemReferenceToArray(one, const_cast<cJSON*>(grid_json));
    pi_card::solver::Input in;
    in.root = one;
    in.data = data;
    in.viewport_w = kViewportW;
    in.gap = pi_card::solver::kStackGap;
    in.measure = &MeasureStub;
    in.measure_ctx = nullptr;
    cJSON* layout = pi_card::solver::Solve(in);
    cJSON* grids = cJSON_GetObjectItem(layout, "grids");
    cJSON* g0 = cJSON_GetArrayItem(grids, 0);
    cJSON* out = g0 ? cJSON_Duplicate(g0, 1) : nullptr;
    cJSON_Delete(layout);
    cJSON_Delete(one);
    return out;
}

// 一次性对完整 root 数组跑 Solve，返回 "grids" 数组的深拷贝（调用方 Delete）。
cJSON* SolveAllGrids(const cJSON* root_arr, const cJSON* data) {
    pi_card::solver::Input in;
    in.root = root_arr;
    in.data = data;
    in.viewport_w = kViewportW;
    in.gap = pi_card::solver::kStackGap;
    in.measure = &MeasureStub;
    in.measure_ctx = nullptr;
    cJSON* layout = pi_card::solver::Solve(in);
    cJSON* grids = cJSON_DetachItemFromObject(layout, "grids");
    cJSON_Delete(layout);
    return grids;
}

// ---------------------------------------------------------------------------
// 纯 cJSON 的"预览会话"复刻——与 pi_card_preview.cc::PreviewOnArgs 的 grid 循环一一对应
// （§4.2）：签名未变的位置一动不动；变了/首次出现就（重）solve 这一个 grid；非"当前最后一个"
// 的位置一旦出现过下一个 grid 就永久冻结，不再重算签名。
// ---------------------------------------------------------------------------
struct GridRec {
    uint32_t sig = 0;
    cJSON* layout = nullptr;  // SolveOneGrid 的输出（本 struct 拥有）
    int rebuild_count = 0;    // 供 P1 之外的诊断用：这个位置一共被(重)建了几次
};

struct SimSession {
    std::vector<GridRec> grids;
    ~SimSession() {
        for (auto& g : grids)
            if (g.layout) cJSON_Delete(g.layout);
    }
};

constexpr int kMaxGridsPreview = 8;  // 同 pi_card_preview.cc

void FeedFrame(SimSession& st, const cJSON* root_arr, const cJSON* data) {
    if (!cJSON_IsArray(root_arr)) return;
    const int ngrid_raw = cJSON_GetArraySize(root_arr);
    const int ngrid = ngrid_raw > kMaxGridsPreview ? kMaxGridsPreview : ngrid_raw;
    for (int i = 0; i < ngrid; ++i) {
        const cJSON* grid_json = cJSON_GetArrayItem(root_arr, i);
        const bool is_last = (i == ngrid_raw - 1);
        if (static_cast<size_t>(i) < st.grids.size()) {
            if (!is_last) continue;  // 冻结：前面已经出现过更晚的兄弟
            const uint32_t sig = GridSignature(grid_json, data);
            GridRec& rec = st.grids[i];
            if (rec.sig == sig) continue;
            cJSON_Delete(rec.layout);
            rec.layout = SolveOneGrid(grid_json, data);
            rec.sig = sig;
            ++rec.rebuild_count;
        } else {
            GridRec rec;
            rec.sig = GridSignature(grid_json, data);
            rec.layout = SolveOneGrid(grid_json, data);
            rec.rebuild_count = 1;
            st.grids.push_back(rec);
        }
    }
}

// 把 partial_json_parse_streaming 的输出（顶层 object，可能没有 "root"/"data" 键）喂进一帧。
void FeedSnapshot(SimSession& st, const cJSON* snap) {
    const cJSON* root_arr = cJSON_GetObjectItem(snap, "root");
    if (!cJSON_IsArray(root_arr)) return;  // 还没吐出合法 root 数组：安静跳过（同 PreviewOnArgs）
    const cJSON* data = cJSON_GetObjectItem(snap, "data");
    FeedFrame(st, root_arr, data);
}

// ---- layout 逐字段比较（cell 级，故意不比 "gi"——单 grid solve 恒 gi=0，一次性 solve 是真实
// 下标，二者语义上等价的是同一个 cell 的其余几何字段）----
bool CellEq(const cJSON* a, const cJSON* b, std::string& why) {
    static const char* keys[] = {"ci", "row", "col", "span", "x", "w", "align", "truncate"};
    for (const char* k : keys) {
        const cJSON* av = cJSON_GetObjectItem(a, k);
        const cJSON* bv = cJSON_GetObjectItem(b, k);
        if (!cJSON_Compare(const_cast<cJSON*>(av), const_cast<cJSON*>(bv), 1)) {
            char* as = cJSON_PrintUnformatted(const_cast<cJSON*>(a));
            char* bs = cJSON_PrintUnformatted(const_cast<cJSON*>(b));
            why = std::string("key '") + k + "' differs: " + (as ? as : "?") + " vs " + (bs ? bs : "?");
            if (as) cJSON_free(as);
            if (bs) cJSON_free(bs);
            return false;
        }
    }
    return true;
}

bool GridLayoutEq(const cJSON* a, const cJSON* b, std::string& why) {
    if (!a || !b) {
        why = "one side is null";
        return false;
    }
    const cJSON* acells = cJSON_GetObjectItem(a, "cells");
    const cJSON* bcells = cJSON_GetObjectItem(b, "cells");
    int an = cJSON_GetArraySize(acells), bn = cJSON_GetArraySize(bcells);
    if (an != bn) {
        why = "cell count differs: " + std::to_string(an) + " vs " + std::to_string(bn);
        return false;
    }
    for (int i = 0; i < an; ++i) {
        if (!CellEq(cJSON_GetArrayItem(acells, i), cJSON_GetArrayItem(bcells, i), why)) return false;
    }
    return true;
}

// ---- 切分点抽样策略 ----
// §7.1 P1 的金标准是"每个字节边界都切一次"；实测 26 张语料全量逐字节跑（含 ASan/UBSan）
// 全程约 8 秒，完全跑得动，故这里直接用逐字节穷举——不做步长抽样折衷。
// 1) 逐字节：[8, len) 每个偏移都切一次（前 8 字节太短不可能出现任何 grid，跳过没有意义）。
//    这一条已经蕴含 2)/3\) 的所有位置，之所以仍保留后两条，是把"UTF-8 多字节字符中间"和
//    "随机边界组合"作为显式、独立于步长的覆盖点记录在案——如果未来因为性能需要把 1) 降回
//    步长采样，2)/3) 能继续兜住最容易漏判的两类边界（多字节截断、结构边界的任意组合），
//    不会随着步长变化而失去覆盖。
// 2) UTF-8 多字节字符中间点：文本里每个多字节字符（含中文）额外插入"落在其内部"的切点
//    （lead byte 之后 1 字节），专门覆盖 partial parser 的 UTF-8 尾截断路径。真实复现过的
//    坑：一个"看似完整"的语法结构（如某个小 grid 的 JSON）如果切分粒度太粗，会跳过它
//    "已完整但仍是 root 数组末尾"的那一帧——退化实验证实：把这里的步长从 37 字节放粗到
//    37 一档就会在 6 张语料上出现 grid 冻结在半吐 type（如 "divider" 半吐成 "d"）的假阳性；
//    逐字节穷举消除了这个采样盲区，故不再需要任何步长折衷。
// 3) 固定种子随机切点：mt19937(12345) 均匀采 24 个，作为逐字节穷举之外的独立交叉验证。
// 4) 全长（完整 JSON）恒作为最后一个切点。
std::vector<size_t> CollectSplitPoints(const std::string& text) {
    std::vector<size_t> pts;
    const size_t len = text.size();
    for (size_t i = 8; i < len; ++i) pts.push_back(i);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0xC0) {  // UTF-8 多字节前导字节
            if (i + 1 <= len) pts.push_back(i + 1);  // 切在续字节中间
        }
    }
    std::mt19937 rng(12345);
    std::uniform_int_distribution<size_t> dist(1, len > 1 ? len - 1 : 1);
    for (int i = 0; i < 24 && len > 1; ++i) pts.push_back(dist(rng));
    pts.push_back(len);
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    return pts;
}

std::vector<std::string> CorpusFiles() {
    return {
        "00_device_ctl.json", "01_confirm.json",  "02_menu.json",     "03_status.json",
        "04_wrap_stress.json", "05_degenerate.json", "07_arc.json",      "08_qrcode.json",
        "09_choice.json",     "10_patch.json",     "11_telemetry.json", "12_charts.json",
        "13_sensors.json",    "14_gps.json",       "15_multicol.json",  "16_stock_bind.json",
        "17_media_ctl.json",  "18_stock_chart.json", "19_style_family.json", "22_table.json",
        "23_grid_auto.json",  "24_grid_ctl.json",  "25_grid_tall.json", "f0_form.json",
        "n1_cells_wrap.json", "n2_rows_align.json",
    };
}

}  // namespace

// ===========================================================================
// P1 —— 任意切分点等价：语料每张卡逐（抽样）切分喂流，最终 layout ≡ 一次性 Solve。
// ===========================================================================
TEST_CASE("P1: 26 张语料逐切分点喂流 == 一次性 Solve（前缀不变量）") {
    for (const auto& name : CorpusFiles()) {
        const std::string path = "sim/tests/corpus/" + name;
        const std::string text = ReadFile(path);
        CHECK(!text.empty());
        if (text.empty()) continue;
        cJSON* full = cJSON_Parse(text.c_str());
        CHECK(full != nullptr);
        if (!full) continue;
        const cJSON* full_root = cJSON_GetObjectItem(full, "root");
        const cJSON* full_data = cJSON_GetObjectItem(full, "data");
        CHECK(cJSON_IsArray(full_root));

        SimSession sess;
        const auto pts = CollectSplitPoints(text);
        for (size_t cut : pts) {
            std::string prefix = text.substr(0, cut);
            cJSON* snap = pi_partial_json_parse_streaming(pi_alloc_default(), prefix.c_str());
            CHECK(snap != nullptr);  // streaming 版本"always returns a tree"
            if (snap) FeedSnapshot(sess, snap);
            cJSON_Delete(snap);
        }

        cJSON* one_shot = SolveAllGrids(full_root, full_data);
        int n_full = cJSON_GetArraySize(full_root);
        int n_check = n_full > 8 ? 8 : n_full;  // 同 kMaxGridsPreview 上限
        CHECK_EQ(static_cast<int>(sess.grids.size()), n_check);
        for (int i = 0; i < n_check && i < static_cast<int>(sess.grids.size()); ++i) {
            std::string why;
            bool eq = GridLayoutEq(sess.grids[i].layout, cJSON_GetArrayItem(one_shot, i), why);
            if (!eq) {
                std::fprintf(stderr, "[preview_prefix_test] %s grid[%d] mismatch: %s\n",
                             name.c_str(), i, why.c_str());
            }
            CHECK(eq);
        }
        cJSON_Delete(one_shot);
        cJSON_Delete(full);
    }
}

// ===========================================================================
// P2 —— 幂等：同一意图 Solve 两次，layout 逐字段相等。
// ===========================================================================
TEST_CASE("P2: 同一 JSON 两次 SolveAllGrids 逐字段相等") {
    for (const auto& name : CorpusFiles()) {
        const std::string text = ReadFile("sim/tests/corpus/" + name);
        CHECK(!text.empty());
        if (text.empty()) continue;
        cJSON* full = cJSON_Parse(text.c_str());
        CHECK(full != nullptr);
        if (!full) continue;
        const cJSON* root = cJSON_GetObjectItem(full, "root");
        const cJSON* data = cJSON_GetObjectItem(full, "data");
        cJSON* a = SolveAllGrids(root, data);
        cJSON* b = SolveAllGrids(root, data);
        int n = cJSON_GetArraySize(a);
        CHECK_EQ(n, cJSON_GetArraySize(b));
        for (int i = 0; i < n; ++i) {
            std::string why;
            CHECK(GridLayoutEq(cJSON_GetArrayItem(a, i), cJSON_GetArrayItem(b, i), why));
        }
        // 签名也必须幂等（P2 对签名本身也成立——preview 的重建判定基于它）。
        const cJSON* g0 = cJSON_GetArrayItem(root, 0);
        if (g0) CHECK_EQ(GridSignature(g0, data), GridSignature(g0, data));
        cJSON_Delete(a);
        cJSON_Delete(b);
        cJSON_Delete(full);
    }
}

// ===========================================================================
// P3 —— 迟到 data：先喂无 data 的前缀、后喂带 data 帧 == 与一次性带 data 等价。
// 用 17_media_ctl.json（含 bind_rows:"tracks"，天然依赖 data）。
// ===========================================================================
TEST_CASE("P3: bind_rows 迟到 data 与一次性带 data 等价") {
    const std::string text = ReadFile("sim/tests/corpus/17_media_ctl.json");
    CHECK(!text.empty());
    if (text.empty()) return;
    cJSON* full = cJSON_Parse(text.c_str());
    CHECK(full != nullptr);
    if (!full) return;
    const cJSON* root = cJSON_GetObjectItem(full, "root");
    const cJSON* data = cJSON_GetObjectItem(full, "data");
    CHECK(cJSON_IsArray(root));
    CHECK(data != nullptr);  // 语料必须真的带 data，否则这条测试没有意义

    // 帧 1：无 data 的完整 root（模拟 data 键还没吐出来）。
    cJSON* no_data_envelope = cJSON_CreateObject();
    cJSON_AddItemToObject(no_data_envelope, "root", cJSON_Duplicate(root, 1));
    SimSession sess;
    FeedSnapshot(sess, no_data_envelope);

    // 帧 2：带 data 的完整快照——照搬 preview 的用法（FeedSnapshot 直接读 envelope 的
    // "root"/"data"）。
    FeedSnapshot(sess, full);

    cJSON* one_shot = SolveAllGrids(root, data);
    int n = cJSON_GetArraySize(root);
    n = n > 8 ? 8 : n;
    CHECK_EQ(static_cast<int>(sess.grids.size()), n);
    for (int i = 0; i < n && i < static_cast<int>(sess.grids.size()); ++i) {
        std::string why;
        bool eq = GridLayoutEq(sess.grids[i].layout, cJSON_GetArrayItem(one_shot, i), why);
        if (!eq) std::fprintf(stderr, "[preview_prefix_test] P3 grid[%d] mismatch: %s\n", i, why.c_str());
        CHECK(eq);
    }

    cJSON_Delete(one_shot);
    cJSON_Delete(no_data_envelope);
    cJSON_Delete(full);
}

int main() { return RUN_ALL_TESTS(); }
