// pi_card_solver.cc —— CARD V2 纯函数布局求解器（docs/CARD_V2.md §2）。
//
// 全整数运算 + 注入 measure 回调，零 LVGL / 零 ESP 依赖。逐条实现契约表(§2.2)、cells 折行(§2.3)、
// rows 分宽(§2.4)、内建装饰(§2.5)、fmt 代表串。输出 layout cJSON（§2.6）。
#include "pi_card_solver.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pi_card {
namespace solver {
namespace {

// ---- 契约建模 ----
enum class Occ { kInline, kFill, kSpanAll, kSquare };
enum class Align { kStart, kCenter, kEnd };

// 文本排布三态（渲染器交叉点消费，见头注 §2.6 wrap 字段）：
//   kWrap     —— 独占整行的正文，允许多行折行撑高（渲染器不钳单行高，LONG_WRAP）。
//   kEllipsis —— 与其它 cell/行共享一行空间的文本（cells 里挤宽度 / rows 表格文本列），
//                单行 + 省略号，渲染器钳单行高（否则 LV_SIZE_CONTENT 下 DOT/CLIP 形同虚设）。
//   kNowrap   —— 数值/非文本控件，单行不截断，且轨道宽已由 solver 保证 ≥ 真实测量宽。
// 同一个正文 label 到底是 WRAP 还是 ELLIPSIS 取决于折行结果（独占一行 or 跟别的 cell 挤同一
// 行），只有 SolveCells 折完行才知道，故在“行内落位”那一步（而非契约求解阶段）判定并回填，
// 不是叶子的静态属性。
enum class WrapMode { kWrap, kEllipsis, kNowrap };

const char* WrapModeStr(WrapMode w) {
    switch (w) {
        case WrapMode::kWrap:
            return "wrap";
        case WrapMode::kEllipsis:
            return "ellipsis";
        case WrapMode::kNowrap:
            return "nowrap";
    }
    return "nowrap";
}

const char* AlignStr(Align a) {
    switch (a) {
        case Align::kStart:
            return "start";
        case Align::kCenter:
            return "center";
        case Align::kEnd:
            return "end";
    }
    return "start";
}

struct Contract {
    std::string type;
    int min_w = 0;
    int pref_w = 0;
    Occ occ = Occ::kInline;
    Align align = Align::kStart;
    bool stretch = false;     // 参与剩余空间分配（FILL / 正文 label）
    bool str_bind = false;    // 文本契约的 label 且带 bind/bind_data（活值宽度不可预测，
                              // 供 SolveRows 否决 cols.num 强制的数值列几何）
    bool growable = false;    // 纯控件均分成员（button/slider/bar/arc/choice）
    bool truncate_ok = false; // 文本列超宽截断
    bool is_num = false;      // 数值 label（右对齐 mono 不截断）
    bool is_arc = false;
    bool side_end = false;    // side:"end"
    int h = 0;                // 高度提示（advisory）
    int natural_w = 0;        // 文本 label 的真实测量宽（clamp≤400 之前），截断判定用它而不是
                              // 被 clamp 过的 pref_w（F4：clamp 只是"契约里报出去的宽度"上限，
                              // 不代表文本真实有多宽——轨道宽落在 (400, natural_w) 区间时，用
                              // clamp 后的 pref_w 跟轨道宽比较会误判"没超宽不用截断"）。非文本
                              // label 类型不设（保持默认 0，不参与比较）。
    const cJSON* node = nullptr;
    int ci = -1;              // 原 JSON 叶子下标（合成 cell = -1）
};

// ---- cJSON 小工具 ----
const char* GetStr(const cJSON* o, const char* k) {
    const cJSON* it = cJSON_GetObjectItem(o, k);
    return (it && cJSON_IsString(it)) ? it->valuestring : nullptr;
}
bool GetBool(const cJSON* o, const char* k) {
    const cJSON* it = cJSON_GetObjectItem(o, k);
    return cJSON_IsTrue(it);
}
bool Has(const cJSON* o, const char* k) { return cJSON_GetObjectItem(o, k) != nullptr; }

int Measure(const Input& in, const char* s, int role, bool mono) {
    if (!s || !in.measure) return 0;
    return in.measure(s, role, mono, in.measure_ctx);
}

// role 串 → int code（§1.4）。
int RoleCode(const char* role) {
    if (!role) return kRoleNone;
    if (!std::strcmp(role, "eyebrow")) return kRoleEyebrow;
    if (!std::strcmp(role, "kicker")) return kRoleKicker;
    if (!std::strcmp(role, "section")) return kRoleSection;
    if (!std::strcmp(role, "title")) return kRoleTitle;
    if (!std::strcmp(role, "heading")) return kRoleHeading;
    if (!std::strcmp(role, "label")) return kRoleLabel;
    if (!std::strcmp(role, "value")) return kRoleValue;
    if (!std::strcmp(role, "caption")) return kRoleCaption;
    return kRoleNone;
}

// 标题类文本 role（在 cells 里独占整行 SPAN_ALL；见决策 B 及 S7 消解，逐条说明见返回）。
bool IsBlockRole(int rc) {
    return rc == kRoleEyebrow || rc == kRoleKicker || rc == kRoleSection || rc == kRoleCaption ||
           rc == kRoleTitle || rc == kRoleHeading;
}

// fmt 是否数值型（非 %s）。
bool FmtNumeric(const char* fmt) {
    if (!fmt) return true;  // 有 bind 无 fmt 视作数值
    return std::strstr(fmt, "%s") == nullptr;
}

// fmt 是否含数值转换符（%d/%u/%i/%f/%e/%g/%x/%X，排除 %s/%c/%%）——F3 修复：role:"value" 不再
// 单独授予数值特权，这是判定依据之一。
bool FmtHasNumericConv(const char* fmt) {
    if (!fmt) return false;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            ++p;
            continue;
        }
        ++p;
        if (*p == '%') {  // 字面 '%'，不是转换
            ++p;
            continue;
        }
        while (*p && !std::strchr("diuxXfeEgGscb", *p)) ++p;
        if (!*p) break;
        if (std::strchr("diuxXfeEgG", *p)) return true;
        ++p;  // %s/%c：跳过，继续找下一个 %
    }
    return false;
}

// 静态 text 是否"看起来像数值样式"（F3 修复的第三条依据）：去首尾空白，剥掉常见单位后缀
// （连续的字母 和/或 '%'，以及它们之间的空格），剩下的核心必须是非空且只含数字/'.'/'-'/'+'。
// 例："42%"→核心"42"；"88 dBm"→核心"88"；"Pinion Pro"→剥到"Pinion"（不含数字）→ false。
bool TextLooksNumeric(const char* text) {
    if (!text || !*text) return false;
    std::string s(text);
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    size_t b = s.find_last_not_of(" \t");
    s = s.substr(a, b - a + 1);
    size_t end = s.size();
    while (end > 0 && (std::isalpha(static_cast<unsigned char>(s[end - 1])) || s[end - 1] == '%')) --end;
    while (end > 0 && s[end - 1] == ' ') --end;
    if (end == 0) return false;
    std::string core = s.substr(0, end);
    bool has_digit = false;
    for (char ch : core) {
        if (ch >= '0' && ch <= '9') {
            has_digit = true;
            continue;
        }
        if (ch == '.' || ch == '-' || ch == '+') continue;
        return false;
    }
    return has_digit;
}

// fmt 代表串（§2.2）：整型转换 → 独占整个 fmt 时 5 个 8，否则 3 个 8；%.Nf → "88" + '.' + N 个 8；
// %% → '%'；其余字面照抄。空 fmt → "88888"。
// 说明：文档示例本身对整型位数不自洽（"%d"→88888 但 "%d%%"→888%），此规则是复现全部四个示例的最小解。
std::string FmtRep(const char* fmt) {
    if (!fmt || !*fmt) return "88888";
    // 判定：整个 fmt 是否恰为单个整型转换。
    bool solo_int = (!std::strcmp(fmt, "%d") || !std::strcmp(fmt, "%u") || !std::strcmp(fmt, "%i"));
    std::string out;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out.push_back(*p++);
            continue;
        }
        ++p;  // 跳过 '%'
        if (*p == '%') {
            out.push_back('%');
            ++p;
            continue;
        }
        // 跳过宽度/精度前缀，找到转换字符。
        std::string prec;  // 精度数字（用于浮点）
        bool had_dot = false;
        while (*p && !std::strchr("diuxXfeggsc", *p)) {
            if (*p == '.') had_dot = true;
            if (had_dot && *p >= '0' && *p <= '9') prec.push_back(*p);
            ++p;
        }
        char conv = *p ? *p : 'd';
        if (*p) ++p;
        if (conv == 'f' || conv == 'e' || conv == 'g') {
            int n = prec.empty() ? 1 : std::atoi(prec.c_str());
            if (n < 0) n = 0;
            out += "88";
            if (n > 0) {
                out.push_back('.');
                for (int i = 0; i < n; ++i) out.push_back('8');
            }
        } else if (conv == 's' || conv == 'c') {
            out += "8888";  // 文本占位（string bind 另走文本列，此处仅兜底）
        } else {
            out += solo_int ? "88888" : "888";
        }
    }
    return out;
}

// ---- 叶子契约求解（§2.2）----
Contract ContractFor(const Input& in, const cJSON* cell, int viewport_w) {
    Contract c;
    c.node = cell;
    const char* type = GetStr(cell, "type");
    c.type = type ? type : "";
    c.side_end = false;
    const char* side = GetStr(cell, "side");
    if (side && !std::strcmp(side, "end")) c.side_end = true;

    const std::string& t = c.type;

    if (t == "label") {
        int rc = RoleCode(GetStr(cell, "role"));
        bool mono = GetBool(cell, "mono");
        const char* fmt = GetStr(cell, "fmt");
        bool has_bind = Has(cell, "bind");
        bool has_bind_data = Has(cell, "bind_data");
        const char* text = GetStr(cell, "text");
        // 字符串证据（一票否决数值列判定，§2.2"string bind 不算数值列"）：fmt 带 %s，或
        // 注入的类型提示说 bind 是字符串路径。真机实录：net.ssid/net.operator 这类字符串
        // bind 被 mono/cols.num 摁进数值列后，64px 兜底轨道装不下活值 → NOWRAP 硬裁。
        bool str_evidence = (fmt && std::strstr(fmt, "%s") != nullptr);
        if (!str_evidence && has_bind && in.bind_kind) {
            const char* bp = GetStr(cell, "bind");
            if (bp && in.bind_kind(bp, in.bind_kind_ctx) == 2) str_evidence = true;
        }

        // 标题类 role → 整行 SPAN_ALL 文本（WRAP）。
        if (rc == kRoleTitle || rc == kRoleHeading) {
            c.occ = Occ::kSpanAll;
            c.align = Align::kStart;
            c.min_w = viewport_w;
            c.pref_w = viewport_w;
            c.h = RoleCode("") == rc ? 0 : 0;
            c.h = (rc == kRoleTitle) ? 40 : 34;
            return c;
        }
        if (IsBlockRole(rc)) {  // eyebrow/kicker/section/caption：小 mono 眉标，整行独占。
            c.occ = Occ::kSpanAll;
            c.align = Align::kStart;
            int w = text ? Measure(in, text, rc, true) : 0;
            c.min_w = viewport_w;
            c.pref_w = viewport_w;
            (void)w;
            c.h = 22;
            return c;
        }

        // 数值 label：mono 显式声明 / 数值 bind（非 %s）/ fmt 含数值转换符 / 静态 text 本身
        // 看起来像数值样式。F3 修复：role=="value" 不再单独授予数值特权——纯文本（如
        // "Pinion" 这种品牌名）标了 role:"value" 但不满足以上任何一条时，按普通文本列
        // 处理（可截断、单行），不强制"不截断+右对齐+mono"逼出折行。
        // has_bind_data（卡级 data 模型绑定）无 fmt 时默认按字符串对待（既有口径——见
        // S:%s/bind_data 字符串 label 走文本列 64px 兜底测试），故此处只沿用 has_bind（DataHub
        // 路径绑定）参与"无 fmt 视为数值"判定，不把 has_bind_data 一并折进来。
        bool numeric = !str_evidence && (mono || (has_bind && FmtNumeric(fmt)) ||
                                         FmtHasNumericConv(fmt) || TextLooksNumeric(text));
        if (numeric) {
            std::string rep = FmtRep(fmt);
            int w = Measure(in, rep.c_str(), rc == kRoleNone ? kRoleValue : rc, true);
            // 数值 cell 是 NOWRAP 不截断，轨道装不下=读数错误，宽度必须宁大勿小（§2.2）：
            //  - fmt 缺席的数值 bind：活值形如 "3832.26"（7 字符）而缺省代表串 "88888" 只有
            //    5 字符 → 左裁掉头一位（真机行情卡实录）。用 "88888.88" 保守代表串兜底。
            //  - 静态数值 text（含 mono 静态串）：内容已知，直接量它取 max——代表串只是预测，
            //    真身在手不该输给预测。
            if (has_bind && (!fmt || !*fmt)) {
                int bw = Measure(in, "88888.88", rc == kRoleNone ? kRoleValue : rc, true);
                if (bw > w) w = bw;
            }
            if (text && *text) {
                int tw = Measure(in, text, rc == kRoleNone ? kRoleValue : rc, true);
                if (tw > w) w = tw;
            }
            c.occ = Occ::kInline;
            c.align = Align::kEnd;
            c.is_num = true;
            c.min_w = w;
            c.pref_w = w;
            c.truncate_ok = false;
            c.h = 28;
            return c;
        }

        // 文本 label（正文 / bind_data 字符串 / %s / 字符串 bind）。
        c.occ = Occ::kInline;
        c.align = Align::kStart;
        c.truncate_ok = true;
        c.stretch = true;  // 可拉（吸收剩余）
        c.str_bind = has_bind || has_bind_data;  // 供 SolveRows 否决 cols.num 强制声明
        int w = 0;
        if (text)
            w = Measure(in, text, rc, mono);
        else if (has_bind_data || has_bind)
            w = 64;  // 字符串 bind 兜底
        c.natural_w = w;  // F4：截断判定要用 clamp 之前的真实测量宽，见 SolveRows TEXT 分支
        if (w > 400) w = 400;  // clamp≤400（这个 clamp 只影响契约报出去的 pref_w/列基准宽）
        c.min_w = 0;
        c.pref_w = w;
        c.h = 28;
        return c;
    }

    if (t == "button") {
        const char* text = GetStr(cell, "text");
        int tw = text ? Measure(in, text, kRoleNone, false) : 0;
        int pref = tw + 32;
        c.min_w = pref > 72 ? pref : 72;
        c.pref_w = pref;
        c.occ = Occ::kInline;
        c.align = Align::kCenter;
        c.growable = true;
        c.h = kTouchMinH;
        return c;
    }
    if (t == "slider") {
        c.min_w = 160;
        c.pref_w = 160;
        c.occ = Occ::kFill;
        c.align = Align::kStart;
        c.stretch = true;
        c.growable = true;
        c.h = kTouchMinH;
        return c;
    }
    if (t == "bar") {
        c.min_w = 120;
        c.pref_w = 120;
        c.occ = Occ::kFill;
        c.align = Align::kStart;
        c.stretch = true;
        c.growable = true;
        c.h = 24;
        return c;
    }
    if (t == "arc") {
        c.min_w = 120;
        c.pref_w = 132;
        c.occ = Occ::kInline;  // 方形但随行内布局（与 value 同行，见 §5.2 ⑦），非硬换行。
        c.align = Align::kCenter;
        c.growable = true;
        c.is_arc = true;
        c.h = 132;
        return c;
    }
    if (t == "switch") {
        c.min_w = 52;
        c.pref_w = 52;
        c.occ = Occ::kInline;
        c.align = Align::kEnd;
        c.h = kTouchMinH;
        return c;
    }
    if (t == "icon") {
        c.min_w = 22;
        c.pref_w = 22;
        c.occ = Occ::kInline;
        c.align = Align::kStart;
        c.h = 28;
        return c;
    }
    if (t == "divider") {
        c.min_w = viewport_w;
        c.pref_w = viewport_w;
        c.occ = Occ::kSpanAll;
        c.h = 1;
        return c;
    }
    if (t == "qrcode") {
        int sz = 160;
        c.min_w = sz;
        c.pref_w = sz;
        c.occ = Occ::kSquare;
        c.align = Align::kCenter;
        c.h = sz;
        return c;
    }
    if (t == "choice") {
        const cJSON* opts = cJSON_GetObjectItem(cell, "options");
        int nseg = cJSON_IsArray(opts) ? cJSON_GetArraySize(opts) : 2;
        if (nseg < 2) nseg = 2;
        c.min_w = nseg * kTouchMinH;
        c.pref_w = viewport_w;
        c.occ = Occ::kSpanAll;  // 默认 cells 里满宽；rows 多列里降级为 FILL（在 rows 求解处理）。
        c.align = Align::kStart;
        c.stretch = true;
        c.growable = true;
        c.h = kTouchMinH;
        return c;
    }
    if (t == "chart") {
        c.min_w = viewport_w;
        c.pref_w = viewport_w;
        c.occ = Occ::kSpanAll;
        c.h = 120;
        return c;
    }
    if (t == "stock_chart") {
        c.min_w = viewport_w;
        c.pref_w = viewport_w;
        c.occ = Occ::kSpanAll;
        c.h = 260;
        return c;
    }

    // 未知类型：按最小 INLINE 兜底。
    c.min_w = 0;
    c.pref_w = 0;
    c.occ = Occ::kInline;
    c.h = 24;
    return c;
}

// ---- 输出辅助 ----
// truncate 字段为兼容保留（单测/旧读者仍按它判断"是否真的超宽会截断"）；wrap 是渲染器实际
// 消费的三态字段，二者独立：truncate 只影响 DOT 是否会真的显示省略号，wrap 决定渲染器要不要
// 钳单行高（ELLIPSIS 恒钳，即使这个具体 cell 内容当下没超宽也钳——同列其它行可能超宽，钳高
// 是这一整列的共同约束，不是逐 cell 现算）。
void EmitCell(cJSON* cells_out, int gi, int ci, int row, int col, int span, int x, int w, Align a,
              bool truncate, WrapMode wrap) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "gi", gi);
    cJSON_AddNumberToObject(o, "ci", ci);
    cJSON_AddNumberToObject(o, "row", row);
    cJSON_AddNumberToObject(o, "col", col);
    cJSON_AddNumberToObject(o, "span", span);
    cJSON_AddNumberToObject(o, "x", x);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddStringToObject(o, "align", AlignStr(a));
    cJSON_AddBoolToObject(o, "truncate", truncate);
    cJSON_AddStringToObject(o, "wrap", WrapModeStr(wrap));
    cJSON_AddItemToArray(cells_out, o);
}

// 计算某组 arc 的统一直径（S5）：取各自可用宽下限的最小值，clamp 到 [120,132]。
int ArcDiameter(const std::vector<Contract>& row) {
    int d = 132;
    for (const auto& c : row)
        if (c.is_arc && c.pref_w < d) d = c.pref_w;
    if (d < 120) d = 120;
    return d;
}

// ===========================================================================
// cells 折行（形态 A，§2.3）
// ===========================================================================
void SolveCells(const Input& in, const cJSON* cells, int gi, int vw, int gap, cJSON* cells_out,
                int& h_hint) {
    std::vector<Contract> cs;
    int idx = 0;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, cells) {
        Contract c = ContractFor(in, it, vw);
        c.ci = idx++;
        cs.push_back(c);
    }

    // 折行：顺序扫描，SPAN_ALL/SQUARE 硬换行独占。
    std::vector<std::vector<int>> rows;  // 每行 cs 下标
    std::vector<int> cur;
    int used = 0;  // 当前行 Σmin + Σgap
    auto flush = [&]() {
        if (!cur.empty()) rows.push_back(cur);
        cur.clear();
        used = 0;
    };
    for (int i = 0; i < (int)cs.size(); ++i) {
        Occ o = cs[i].occ;
        if (o == Occ::kSpanAll || o == Occ::kSquare) {
            flush();
            rows.push_back({i});
            continue;
        }
        int add = (cur.empty() ? 0 : gap) + cs[i].min_w;
        if (!cur.empty() && used + add > vw) flush();
        used += (cur.empty() ? 0 : gap) + cs[i].min_w;
        cur.push_back(i);
    }
    flush();

    int total_h = 0;
    int arc_d = ArcDiameter(cs);  // 全 grid 统一
    for (int r = 0; r < (int)rows.size(); ++r) {
        auto& line = rows[r];
        int row_h = 0;
        // 独占行（SPAN_ALL / SQUARE）。
        if (line.size() == 1 && (cs[line[0]].occ == Occ::kSpanAll || cs[line[0]].occ == Occ::kSquare)) {
            Contract& c = cs[line[0]];
            if (c.occ == Occ::kSpanAll) {
                // F2：SPAN_ALL 叶子带 side:"end" 时内容靠右（叶子本身已占满整行宽度，side:end
                // 影响的是内容在这个满宽容器里的对齐方式）；否则维持契约给的默认 align。
                Align a = c.side_end ? Align::kEnd : c.align;
                // SPAN_ALL 独占整行：正是 WRAP 语义的定义场景（title/heading/eyebrow/kicker/
                // section/caption/choice/divider/chart 等都在这条分支，允许自然长高）。
                EmitCell(cells_out, gi, c.ci, r, 0, 1, 0, vw, a, false, WrapMode::kWrap);
            } else {  // SQUARE：居中
                int w = c.pref_w;
                int x = (vw - w) / 2;
                if (x < 0) x = 0;
                EmitCell(cells_out, gi, c.ci, r, 0, 1, x, w, Align::kCenter, false, WrapMode::kNowrap);
            }
            row_h = c.h;
            total_h += row_h + (r ? gap : 0);
            continue;
        }

        // 普通 INLINE/FILL 行布局。
        int n = (int)line.size();
        // 单个 cell 独占行且不可拉 → 居中（S7）；但显式 side:"end" 优先于默认居中——
        // 标题/眉标是 SPAN_ALL 恒独占行，模型想放"标题行右上角"的角标按钮（真机行情卡的
        // 刷新钮实录）只能落到下一行，此时 side:end 是它仅存的"靠右"意图表达，吞掉它就
        // 变成孤零零居中一颗钮。
        if (n == 1 && !cs[line[0]].stretch) {
            Contract& c = cs[line[0]];
            int w = c.is_arc ? arc_d : (c.pref_w > c.min_w ? c.pref_w : c.min_w);
            if (w > vw) w = vw;
            int x = c.side_end ? (vw - w) : (vw - w) / 2;
            if (x < 0) x = 0;
            // 独占行但非 stretch：一定是非文本控件（文本 label 恒 stretch=true，不会走这支），
            // truncate_ok 恒为 false，NOWRAP 语义。
            EmitCell(cells_out, gi, c.ci, r, 0, 1, x, w, c.side_end ? Align::kEnd : Align::kCenter,
                     c.truncate_ok, WrapMode::kNowrap);
            row_h = c.h;
            total_h += row_h + (r ? gap : 0);
            continue;
        }

        // 是否全 growable（均分）。
        bool all_grow = n > 1;
        for (int i : line)
            if (!cs[i].growable) all_grow = false;

        std::vector<int> w(n, 0);
        int total_gap = gap * (n - 1);
        if (all_grow) {
            // 均分但尊重 min_w，且总宽恰好填满整行：各 cell 起于 min_w，剩余空间平摊。
            // F2 修复：side:"end" 的 growable cell 是显式的"退到自己内容宽、靠边"声明，优先级
            // 高于默认均分——它退回自己的内容宽（不参与均分膨胀），下方"end 分组落位"会把它
            // 摆到行尾；其余非 side_end 的 growable cell 在刨掉 end 组占用之后的剩余空间里均分。
            int avail = vw - total_gap;
            if (avail < 0) avail = 0;
            std::vector<int> end_idx, normal_idx;
            for (int i = 0; i < n; ++i) {
                if (cs[line[i]].side_end)
                    end_idx.push_back(i);
                else
                    normal_idx.push_back(i);
            }
            int end_sum = 0;
            for (int i : end_idx) {
                Contract& c = cs[line[i]];
                w[i] = c.pref_w > c.min_w ? c.pref_w : c.min_w;
                end_sum += w[i];
            }
            int rem_avail = avail - end_sum;
            if (rem_avail < 0) rem_avail = 0;
            int sum_min = 0;
            for (int i : normal_idx) sum_min += cs[line[i]].min_w;
            int extra = rem_avail - sum_min;
            if (extra < 0) extra = 0;  // 折行已保证 Σmin ≤ avail，此处防御
            int cnt = (int)normal_idx.size();
            int base = cnt > 0 ? extra / cnt : 0;
            int rem2 = cnt > 0 ? extra - base * cnt : 0;
            for (int k = 0; k < cnt; ++k) {
                int i = normal_idx[k];
                w[i] = cs[line[i]].min_w + base + (k < rem2 ? 1 : 0);
            }
        } else {
            // 非 growable：非 stretch 用 pref（arc 用统一直径），stretch 用 min + 剩余分配。
            int fixed_sum = 0;
            std::vector<int> stretch_idx;
            for (int i = 0; i < n; ++i) {
                Contract& c = cs[line[i]];
                if (c.stretch) {
                    w[i] = c.min_w;
                    stretch_idx.push_back(i);
                } else {
                    w[i] = c.is_arc ? arc_d : (c.pref_w > c.min_w ? c.pref_w : c.min_w);
                }
                fixed_sum += w[i];
            }
            int rem = vw - fixed_sum - total_gap;
            if (rem > 0 && !stretch_idx.empty()) {
                int base = rem / (int)stretch_idx.size();
                int extra = rem - base * (int)stretch_idx.size();
                for (int k = 0; k < (int)stretch_idx.size(); ++k)
                    w[stretch_idx[k]] += base + (k < extra ? 1 : 0);
            } else if (rem < 0) {
                // 收缩可截断/可拉 cell（不压固定/数值）。
                for (int i = 0; i < n && rem < 0; ++i) {
                    Contract& c = cs[line[i]];
                    if (c.truncate_ok || c.stretch) {
                        int cut = -rem;
                        if (cut > w[i]) cut = w[i];
                        w[i] -= cut;
                        rem += cut;
                    }
                }
            }
        }

        // end 分组落位：非 end 靠左顺排，end 顺序靠右。
        int left_x = 0;
        // 先算 end 组总宽。
        int end_total = 0, end_cnt = 0;
        for (int i = 0; i < n; ++i)
            if (cs[line[i]].side_end || cs[line[i]].align == Align::kEnd) {
                end_total += w[i];
                ++end_cnt;
            }
        int end_gap = end_cnt > 0 ? gap * end_cnt : 0;  // 组间 + 组内 gap 近似
        int right_x = vw - end_total - (end_cnt > 0 ? gap * (end_cnt) : 0);
        // 更精确：end 组从右缘往左排。
        // 先扫左组顺排，再扫 end 组从右往左（按原序保证 x 递增）。
        std::vector<int> x(n, 0);
        {
            // 左组
            bool first_left = true;
            for (int i = 0; i < n; ++i) {
                Contract& c = cs[line[i]];
                bool is_end = c.side_end || c.align == Align::kEnd;
                if (is_end) continue;
                if (!first_left) left_x += gap;
                x[i] = left_x;
                left_x += w[i];
                first_left = false;
            }
            // end 组：靠右，保持原序 → 计算总宽（含组内 gap），从右缘铺开。
            int esum = 0, ecnt = 0;
            for (int i = 0; i < n; ++i) {
                Contract& c = cs[line[i]];
                if (c.side_end || c.align == Align::kEnd) {
                    esum += w[i];
                    ++ecnt;
                }
            }
            int estart = vw - esum - (ecnt > 1 ? gap * (ecnt - 1) : 0);
            int cursor = estart;
            for (int i = 0; i < n; ++i) {
                Contract& c = cs[line[i]];
                if (!(c.side_end || c.align == Align::kEnd)) continue;
                x[i] = cursor;
                cursor += w[i] + gap;
            }
        }
        (void)right_x;
        (void)end_gap;

        for (int i = 0; i < n; ++i) {
            Contract& c = cs[line[i]];
            int ww = w[i];
            if (c.is_arc) ww = arc_d;
            // 三态回填（交叉点）：c.truncate_ok 只标记"这是正文类文本"，真正落地成 WRAP 还是
            // ELLIPSIS 要看折完行之后它是否独占这一行（n==1）——n>1 意味着它在跟别的 cell
            // （典型是按钮）挤同一行的有限空间，超了必须截断不能自己长高把整行撑爆。
            WrapMode wrap = WrapMode::kNowrap;
            bool trunc = false;
            if (c.truncate_ok) {
                wrap = (n == 1) ? WrapMode::kWrap : WrapMode::kEllipsis;
                trunc = (wrap == WrapMode::kEllipsis);
            }
            EmitCell(cells_out, gi, c.ci, r, i, 1, x[i], ww, c.align, trunc, wrap);
            if (c.h > row_h) row_h = c.h;
        }
        total_h += row_h + (r ? gap : 0);
    }
    h_hint = total_h;
}

// ===========================================================================
// rows 轨道分宽（形态 B/C，§2.4 + §2.5）
// ===========================================================================
enum class ColType { kText, kNum, kFixed, kFill };

// 把 bind_rows 展开成 rows（字符串占位符替换）。返回构造好的行数组（新分配 cJSON 数组，caller 释放）。
std::string SubstStr(const char* tmpl, const cJSON* item, int i) {
    std::string out;
    for (const char* p = tmpl; *p;) {
        if (*p == '{') {
            const char* e = std::strchr(p, '}');
            if (e) {
                std::string key(p + 1, e - p - 1);
                if (key == "i") {
                    out += std::to_string(i);
                } else if (key == "n") {
                    out += std::to_string(i + 1);
                } else if (key == "item") {
                    if (item && cJSON_IsString(item))
                        out += item->valuestring;
                    else if (item && cJSON_IsNumber(item))
                        out += std::to_string(item->valueint);
                } else if (key.rfind("item.", 0) == 0 && item && cJSON_IsObject(item)) {
                    const cJSON* f = cJSON_GetObjectItem(item, key.c_str() + 5);
                    if (f && cJSON_IsString(f))
                        out += f->valuestring;
                    else if (f && cJSON_IsNumber(f))
                        out += std::to_string(f->valueint);
                }
                p = e + 1;
                continue;
            }
        }
        out.push_back(*p++);
    }
    return out;
}

cJSON* CloneWithSubst(const cJSON* node, const cJSON* item, int i) {
    cJSON* dup = cJSON_Duplicate(node, 1);
    // 递归替换所有字符串值里的占位符。
    // 简化：遍历对象/数组树。
    std::vector<cJSON*> stack{dup};
    while (!stack.empty()) {
        cJSON* cur = stack.back();
        stack.pop_back();
        for (cJSON* ch = cur->child; ch; ch = ch->next) {
            if (cJSON_IsString(ch) && ch->valuestring && std::strchr(ch->valuestring, '{')) {
                std::string s = SubstStr(ch->valuestring, item, i);
                cJSON_SetValuestring(ch, s.c_str());
            } else if (cJSON_IsObject(ch) || cJSON_IsArray(ch)) {
                stack.push_back(ch);
            }
        }
    }
    return dup;
}

// 构造 rows 二维（每元素是「行」= 叶子数组）。bind_rows 时展开 data。
// 返回持有型 cJSON 数组（caller cJSON_Delete）。
cJSON* BuildRows(const Input& in, const cJSON* grid) {
    const cJSON* rows = cJSON_GetObjectItem(grid, "rows");
    if (cJSON_IsArray(rows)) return cJSON_Duplicate(rows, 1);

    // bind_rows 形态。
    cJSON* out = cJSON_CreateArray();
    const char* key = GetStr(grid, "bind_rows");
    const cJSON* item = cJSON_GetObjectItem(grid, "item");
    const cJSON* arr = (in.data && key) ? cJSON_GetObjectItem(in.data, key) : nullptr;
    int max = Has(grid, "max") ? cJSON_GetObjectItem(grid, "max")->valueint : 20;
    if (max < 1) max = 1;
    if (max > 20) max = 20;
    int count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (count > max) count = max;

    if (count == 0) {
        const char* empty = GetStr(grid, "empty");
        cJSON* row = cJSON_CreateArray();
        cJSON* lbl = cJSON_CreateObject();
        cJSON_AddStringToObject(lbl, "type", "label");
        cJSON_AddStringToObject(lbl, "text", empty ? empty : "");
        cJSON_AddItemToArray(row, lbl);
        cJSON_AddItemToArray(out, row);
        return out;
    }

    for (int i = 0; i < count; ++i) {
        const cJSON* el = cJSON_GetArrayItem(arr, i);
        cJSON* row = cJSON_CreateArray();
        if (cJSON_IsArray(item)) {
            const cJSON* leaf = nullptr;
            cJSON_ArrayForEach(leaf, item) cJSON_AddItemToArray(row, CloneWithSubst(leaf, el, i));
        } else if (item) {
            cJSON_AddItemToArray(row, CloneWithSubst(item, el, i));
        }
        cJSON_AddItemToArray(out, row);
    }
    return out;
}

void SolveRows(const Input& in, const cJSON* grid, int gi, int vw, int gap, cJSON* cells_out,
               int& ncol_out, cJSON* track_out, int& h_hint) {
    cJSON* rows = BuildRows(in, grid);

    // 列数。
    const cJSON* cols = cJSON_GetObjectItem(grid, "cols");
    int ncol = 0;
    if (cJSON_IsArray(cols)) ncol = cJSON_GetArraySize(cols);
    const cJSON* row = nullptr;
    cJSON_ArrayForEach(row, rows) {
        int n = cJSON_IsArray(row) ? cJSON_GetArraySize(row) : 0;
        if (n > ncol) ncol = n;
    }
    if (ncol < 1) ncol = 1;

    // 收集每个数据行的契约（保留原行下标，供 ci 回指）。
    struct RCell {
        Contract c;
        int row;  // 原始数据行下标
        int col;
    };
    std::vector<std::vector<Contract>> grid_cs;  // [row][col]
    int rcount = cJSON_GetArraySize(rows);
    for (int r = 0; r < rcount; ++r) {
        const cJSON* rr = cJSON_GetArrayItem(rows, r);
        std::vector<Contract> line;
        int cc = 0;
        const cJSON* leaf = nullptr;
        cJSON_ArrayForEach(leaf, rr) {
            Contract c = ContractFor(in, leaf, vw);
            c.ci = r * 100 + cc;  // 行主序编号（合成表头用负数）
            line.push_back(c);
            ++cc;
        }
        grid_cs.push_back(std::move(line));
    }

    // 列类型 & 基准轨道宽。
    std::vector<ColType> ctype(ncol, ColType::kText);
    std::vector<int> track(ncol, 0);
    for (int c = 0; c < ncol; ++c) {
        bool col_num = false;
        if (cJSON_IsArray(cols) && c < cJSON_GetArraySize(cols)) {
            const cJSON* cd = cJSON_GetArrayItem(cols, c);
            col_num = GetBool(cd, "num");
        }
        bool any = false, all_num = true, all_fixed = true, has_fill = false, has_str = false;
        int base = 0;
        for (auto& line : grid_cs) {
            if (c >= (int)line.size()) continue;
            Contract& cell = line[c];
            if (cell.occ == Occ::kSpanAll) continue;  // 跨行 cell 不参与列宽
            any = true;
            if (cell.occ == Occ::kFill) has_fill = true;
            if (cell.type == "choice") has_fill = true;  // rows 多列里 choice → FILL
            if (!cell.is_num) all_num = false;
            if (cell.str_bind) has_str = true;
            bool fixed = (cell.type == "icon" || cell.type == "switch" || cell.is_num);
            if (!fixed) all_fixed = false;
        }
        if (!any) {
            ctype[c] = ColType::kText;
            track[c] = 0;
            continue;
        }
        if (has_fill) {
            ctype[c] = ColType::kFill;
            // 基准 = 该列 FILL/控件的 min_w 最大值。
            for (auto& line : grid_cs) {
                if (c >= (int)line.size()) continue;
                Contract& cell = line[c];
                if (cell.occ == Occ::kSpanAll) continue;
                int mn = (cell.type == "choice") ? cell.min_w : cell.min_w;
                if (mn > base) base = mn;
            }
        } else if ((col_num || all_num) && !has_str) {
            // has_str 一票否决 cols.num：字符串 bind 的活值宽度不可预测，摁进"固定轨+NOWRAP"
            // 的数值列几何=64px 兜底轨装长串必硬裁（真机「连接详情」实录：num:true 列全是
            // fmt %s 的 net.ssid/operator）。降级为 kText（stretch 列，吸剩余、可截断）。
            ctype[c] = ColType::kNum;
            // 数值列轨道宽下限 = 该列所有行 cell 的真实测量宽最大值。用 natural_w（未被
            // clamp≤400 污染的原始测量宽，同 F4）兜底 pref_w——cols.num:true 强制声明的列里，
            // 契约没判定出"数值"的 cell（如无 mono/无数值 fmt 但被 cols.num 摁成数值列的文本，
            // 见 25_grid_tall.json 的 v01.."v22"）natural_w 才有意义；真正数值契约 cell
            // （is_num=true）不设 natural_w（恒 0），退回 pref_w（已是 FmtRep 代表串按正确字体
            // 测出的宽，不受 400 clamp 影响，本来就准）。
            for (auto& line : grid_cs) {
                if (c >= (int)line.size()) continue;
                Contract& cell = line[c];
                if (cell.occ == Occ::kSpanAll) continue;
                int w = cell.natural_w > 0 ? cell.natural_w : cell.pref_w;
                if (w > base) base = w;
            }
        } else if (all_fixed) {
            ctype[c] = ColType::kFixed;
            for (auto& line : grid_cs) {
                if (c >= (int)line.size()) continue;
                Contract& cell = line[c];
                if (cell.occ == Occ::kSpanAll) continue;
                if (cell.min_w > base) base = cell.min_w;
            }
        } else {
            ctype[c] = ColType::kText;
            for (auto& line : grid_cs) {
                if (c >= (int)line.size()) continue;
                Contract& cell = line[c];
                if (cell.occ == Occ::kSpanAll) continue;
                int w = cell.pref_w > 400 ? 400 : cell.pref_w;
                if (w > base) base = w;
            }
        }
        // 表头标题参与定轨：title 比列内容宽时按标题定——否则「涨跌幅」这类三字表头落进
        // 窄数值轨会折行/截断（表头行与数据行共享同一套轨道，宽度契约必须同时罩住两者）。
        if (cJSON_IsArray(cols) && c < cJSON_GetArraySize(cols)) {
            const char* title = GetStr(cJSON_GetArrayItem(cols, c), "title");
            if (title && *title) {
                int tw = Measure(in, title, kRoleSection, true);
                if (tw > base) base = tw;
            }
        }
        track[c] = base;
    }

    // 剩余分配 / 压缩。
    int sum = 0;
    for (int t : track) sum += t;
    int rem = vw - sum - gap * (ncol - 1);
    // stretch 列：FILL 优先，否则 TEXT。
    std::vector<int> stretch_cols;
    for (int c = 0; c < ncol; ++c)
        if (ctype[c] == ColType::kFill) stretch_cols.push_back(c);
    if (stretch_cols.empty())
        for (int c = 0; c < ncol; ++c)
            if (ctype[c] == ColType::kText) stretch_cols.push_back(c);
    if (rem > 0 && !stretch_cols.empty()) {
        int base = rem / (int)stretch_cols.size();
        int extra = rem - base * (int)stretch_cols.size();
        for (int k = 0; k < (int)stretch_cols.size(); ++k)
            track[stretch_cols[k]] += base + (k < extra ? 1 : 0);
    } else if (rem > 0 && ncol > 0) {
        // 全 NUM/FIXED 列（一个 stretch 列都没有，如整表全是数值 bind 的行情卡）：剩余宽度
        // 按轨道宽比例摊给所有列铺满整幅——否则表格停在自然宽、右半悬空（真机实录）。
        // 数值列加宽是安全的：NOWRAP 只要求轨道 ≥natural，cell 在更宽的轨道里仍按列对齐
        // （num 恒右对齐）落位，观感恰是行情表的「列拉开」样式。
        int given = 0;
        for (int c = 0; c < ncol; ++c) {
            int add = (c == ncol - 1)
                          ? rem - given
                          : (sum > 0 ? (int)((long long)rem * track[c] / sum) : rem / ncol);
            track[c] += add;
            given += add;
        }
    } else if (rem < 0) {
        // 压缩 TEXT/FILL 列（不压 NUM/FIXED）。
        for (int c = 0; c < ncol && rem < 0; ++c) {
            if (ctype[c] == ColType::kText || ctype[c] == ColType::kFill) {
                int cut = -rem;
                if (cut > track[c]) cut = track[c];
                track[c] -= cut;
                rem += cut;
            }
        }
    }

    // 列 x 偏移。
    std::vector<int> colx(ncol, 0);
    int acc = 0;
    for (int c = 0; c < ncol; ++c) {
        colx[c] = acc;
        acc += track[c] + gap;
    }

    int out_row = 0;
    int total_h = 0;
    int arc_d = 132;  // rows 内 arc 统一（简化，一般不出现）

    // 自动表头（§2.5）：cols 有 title → section 行 + divider 行。
    bool has_title = false;
    if (cJSON_IsArray(cols)) {
        const cJSON* cd = nullptr;
        cJSON_ArrayForEach(cd, cols)
            if (GetStr(cd, "title")) has_title = true;
    }
    if (has_title) {
        int rh = 0;
        for (int c = 0; c < ncol; ++c) {
            const cJSON* cd = (c < cJSON_GetArraySize(cols)) ? cJSON_GetArrayItem(cols, c) : nullptr;
            const char* title = cd ? GetStr(cd, "title") : nullptr;
            bool col_num = cd ? GetBool(cd, "num") : false;
            Align a = col_num ? Align::kEnd : Align::kStart;
            // 合成表头标题：短串、独占自己的表头格（不跟别的 cell 挤），语义等价 ELLIPSIS
            // （单行+可截断），跟真实 rows 文本列的表头一致处理。
            EmitCell(cells_out, gi, -1, out_row, c, 1, colx[c], track[c], a, false, WrapMode::kEllipsis);
            if (title) {
                int th = 22;
                if (th > rh) rh = th;
            }
        }
        total_h += rh;
        ++out_row;
        // divider 行。
        EmitCell(cells_out, gi, -1, out_row, 0, ncol, 0, vw, Align::kStart, false, WrapMode::kNowrap);
        total_h += 1 + gap;
        ++out_row;
    }

    // 数据行。
    for (int r = 0; r < (int)grid_cs.size(); ++r) {
        auto& line = grid_cs[r];
        int rh = 0;
        // 整行 SPAN（单 cell 且 SPAN_ALL，如 [divider]/[chart]）。
        if (line.size() == 1 && line[0].occ == Occ::kSpanAll) {
            // 独占整行（divider/chart，或极少见的 rows 里单独一个标题类 label）：WRAP 语义。
            EmitCell(cells_out, gi, line[0].ci, out_row, 0, ncol, 0, vw, line[0].align, false,
                     WrapMode::kWrap);
            rh = line[0].h;
            total_h += rh + (out_row ? gap : 0);
            ++out_row;
            continue;
        }
        for (int c = 0; c < (int)line.size() && c < ncol; ++c) {
            Contract& cell = line[c];
            int x = colx[c];
            int w = track[c];
            Align a;
            bool trunc = false;
            WrapMode wrap = WrapMode::kNowrap;
            if (ctype[c] == ColType::kNum) {
                a = Align::kEnd;
                trunc = false;
                wrap = WrapMode::kNowrap;  // 数值列：单行不截断，轨道宽已保证≥真实测量宽。
                // 数值 cell 内容宽 ≤ 轨道，右对齐；宽度取轨道（右对齐在轨道内）。
            } else if (ctype[c] == ColType::kFill) {
                if (cell.stretch) {
                    // 真正可拉伸的控件（slider/bar/rows 多列里的 choice）吃满整个列轨道。
                    a = Align::kStart;
                } else {
                    // F1 修复：列类型是 FILL（因为该列在别的行里有 slider/bar/choice），但
                    // *这个* cell 本身契约 stretch==false（switch/icon/数值 label）——逐 cell
                    // 判断，不因所在列是 FILL 列就被拉伸。保持自己的契约宽，落在自己轨道内，
                    // 按契约默认对齐（switch→end 贴右 / icon→start 贴左 / 数值 label→end）。
                    w = cell.is_arc ? arc_d : (cell.pref_w > cell.min_w ? cell.pref_w : cell.min_w);
                    if (w > track[c]) w = track[c];
                    a = cell.align;
                }
                // FILL 列绝大多数是非文本控件（slider/bar/choice/switch/icon），NOWRAP；边界
                // 情况：同列其它行放了 slider 导致这列判成 FILL，但*这个* cell 恰好是文本 label
                // ——仍按"跟别的行共享列轨道"的 ELLIPSIS 语义处理，不因列类型是 FILL 就当非文本。
                wrap = cell.truncate_ok ? WrapMode::kEllipsis : WrapMode::kNowrap;
            } else if (ctype[c] == ColType::kFixed) {
                a = cell.align;  // icon start / switch end
                wrap = WrapMode::kNowrap;
            } else {  // TEXT
                a = Align::kStart;
                // F4：用未被 clamp≤400 污染的真实测量宽 natural_w 跟轨道宽比较，而不是 pref_w
                // ——轨道宽落在 (400, natural_w) 区间时 pref_w(≤400) 会小于 w，误判"不用截断"，
                // 结果 LVGL 收到 truncate=false 对超宽文本走 WRAP，断词多行。truncate 字段保留
                // 这个"是否真超宽"的精确判定（兼容旧读者/单测）；但 wrap 恒 ELLIPSIS——rows 文本
                // 列的本质是多行共享同一轨道，即使这一个 cell 眼下没超宽也要单行+钳高，不然同列
                // 其它行的超宽 cell 触发 DOT 时，这一个却按 WRAP 自然长高，行高参差不齐。
                trunc = cell.truncate_ok && (cell.natural_w > w);
                wrap = WrapMode::kEllipsis;
            }
            if (cell.side_end) a = Align::kEnd;
            if (cell.is_arc) w = arc_d;
            // 当 cell 未吃满轨道时（F1 分支），按最终对齐把 x 落在轨道内，而不是永远贴轨道左缘。
            if (w < track[c]) {
                if (a == Align::kEnd) x = colx[c] + track[c] - w;
                else if (a == Align::kCenter) x = colx[c] + (track[c] - w) / 2;
            }
            EmitCell(cells_out, gi, cell.ci, out_row, c, 1, x, w, a, trunc, wrap);
            if (cell.h > rh) rh = cell.h;
        }
        total_h += rh + (out_row ? gap : 0);
        ++out_row;
    }

    // 输出 track。
    for (int c = 0; c < ncol; ++c) cJSON_AddItemToArray(track_out, cJSON_CreateNumber(track[c]));
    ncol_out = ncol;
    h_hint = total_h;

    cJSON_Delete(rows);
}

}  // namespace

cJSON* Solve(const Input& in) {
    cJSON* out = cJSON_CreateObject();
    cJSON* grids = cJSON_AddArrayToObject(out, "grids");
    if (!in.root || !cJSON_IsArray(in.root)) return out;

    int vw = in.viewport_w;
    int gap = in.gap > 0 ? in.gap : kStackGap;

    int gi = 0;
    const cJSON* grid = nullptr;
    cJSON_ArrayForEach(grid, in.root) {
        cJSON* gout = cJSON_CreateObject();
        cJSON* cells_out = cJSON_CreateArray();
        cJSON* track_out = cJSON_CreateArray();
        int ncol = 1;
        int h_hint = 0;

        // 带 fill/bg 底色的 grid：内容四周留 kFillInset 内边距——按收窄后的视口求解，渲染器
        // 给容器补同宽 pad（否则文字贴着底色边缘画，见 fill 内边距修复）。
        int inset = (Has(grid, "fill") || Has(grid, "bg")) ? kFillInset : 0;
        int gvw = vw - 2 * inset;
        if (gvw < 1) {  // 防御：视口小到塞不下内边距时放弃 inset
            inset = 0;
            gvw = vw;
        }

        const cJSON* cells = cJSON_GetObjectItem(grid, "cells");
        if (cJSON_IsArray(cells)) {
            // cells 形态：ncol = 各行最大 cell 数，track_w 留空（渲染器按 x/w 落位）。
            SolveCells(in, cells, gi, gvw, gap, cells_out, h_hint);
            // 计算 ncol（各 row 最大 col+1）。
            int maxc = 1;
            const cJSON* c = nullptr;
            cJSON_ArrayForEach(c, cells_out) {
                int col = cJSON_GetObjectItem(c, "col")->valueint + 1;
                if (col > maxc) maxc = col;
            }
            ncol = maxc;
        } else {
            SolveRows(in, grid, gi, gvw, gap, cells_out, ncol, track_out, h_hint);
        }

        if (inset > 0) h_hint += 2 * inset;  // 上下内边距计入高度预估
        cJSON_AddNumberToObject(gout, "ncol", ncol);
        cJSON_AddItemToObject(gout, "track_w", track_out);
        cJSON_AddNumberToObject(gout, "h_hint", h_hint);
        cJSON_AddNumberToObject(gout, "inset", inset);
        cJSON_AddItemToObject(gout, "cells", cells_out);
        cJSON_AddItemToArray(grids, gout);
        ++gi;
    }
    return out;
}

}  // namespace solver
}  // namespace pi_card
