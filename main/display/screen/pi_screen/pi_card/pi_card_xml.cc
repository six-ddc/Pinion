#include "pi_card_xml.h"

// ---------------------------------------------------------------------------
// 两段式实现（都是纯函数，双端编译）：
//   1) Parse：宽容 SAX 扫一遍字节流 → 轻量元素树（tag 小写化、实体解码、未闭合自动闭合、
//      尾部残缺 token 丢弃、错配闭标签按 HTML 传统弹栈恢复、void 元素免闭合）。
//   2) Compile：元素树 → cJSON 信封（word-for-word 对应 docs/CARD_XML.md §2 的映射表 +
//      §2.5 HTML 子集容错）。一切降级写 note，绝不整卡失败。
// 词表封闭、树深恒 2（card→块→叶，table 多一层 tr/td），不支持命名空间/DTD/CDATA。
// ---------------------------------------------------------------------------

#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>

namespace pi_card {

namespace {

// ---------------------------------------------------------------------------
// 元素树
// ---------------------------------------------------------------------------

struct Elem;

struct Node {  // 混合内容：二选一（text 非空 or el 非空）
    std::string text;
    std::unique_ptr<Elem> el;
};

struct Elem {
    std::string tag;  // 已小写
    std::vector<std::pair<std::string, std::string>> attrs;  // name 小写、value 已实体解码
    std::vector<Node> kids;

    const std::string* Attr(const char* name) const {
        for (const auto& a : attrs)
            if (a.first == name) return &a.second;
        return nullptr;
    }
};

// 解析过程的降级记录（Compile 阶段汇总成 note）
struct ParseDiag {
    bool truncated = false;    // 尾部残缺 token（半个开标签/引号没收尾）被丢弃
    int auto_closed = 0;       // EOF 时自动闭合的未闭合元素数
};

bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsNameChar(char c) {
    return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '.';
}
bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c += 32;
    return out;
}

// 标准实体 + 数字实体解码；裸 & 当字面量（LLM 高频遗漏点，§2.6）。
std::string DecodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        size_t semi = s.find(';', i + 1);
        bool decoded = false;
        if (semi != std::string_view::npos && semi - i <= 10) {
            std::string_view ent = s.substr(i + 1, semi - i - 1);
            if (ent == "amp") { out += '&'; decoded = true; }
            else if (ent == "lt") { out += '<'; decoded = true; }
            else if (ent == "gt") { out += '>'; decoded = true; }
            else if (ent == "quot") { out += '"'; decoded = true; }
            else if (ent == "apos") { out += '\''; decoded = true; }
            else if (!ent.empty() && ent[0] == '#') {
                long cp = -1;
                char* endp = nullptr;
                std::string num(ent.substr(1));
                if (!num.empty() && (num[0] == 'x' || num[0] == 'X'))
                    cp = std::strtol(num.c_str() + 1, &endp, 16);
                else
                    cp = std::strtol(num.c_str(), &endp, 10);
                if (endp && *endp == '\0' && cp > 0 && cp <= 0x10FFFF) {
                    // 码点 → UTF-8
                    unsigned u = static_cast<unsigned>(cp);
                    if (u < 0x80) out += static_cast<char>(u);
                    else if (u < 0x800) {
                        out += static_cast<char>(0xC0 | (u >> 6));
                        out += static_cast<char>(0x80 | (u & 0x3F));
                    } else if (u < 0x10000) {
                        out += static_cast<char>(0xE0 | (u >> 12));
                        out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (u & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (u >> 18));
                        out += static_cast<char>(0x80 | ((u >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (u & 0x3F));
                    }
                    decoded = true;
                }
            }
        }
        if (decoded) {
            i = semi + 1;
        } else {
            out += '&';  // 裸 & 字面量
            i++;
        }
    }
    return out;
}

// 无内容叶子/HTML void：开标签即闭合，不吃后续兄弟。qrcode/label/button/choice/td 等
// 合法带内容的不在列。
bool IsVoidTag(const std::string& tag) {
    static const std::set<std::string> kVoid = {"br",    "hr",  "img",   "input", "divider",
                                               "icon",  "slider", "arc", "switch", "bar",
                                               "chart", "stock_chart"};
    return kVoid.count(tag) != 0;
}

constexpr int kMaxDepth = 16;  // 结构上只需 4 层（card→table→tr→td），防恶意/病态输入

// 宽容 SAX：xml[0..len) → 顶层节点列表（伪 document root 的 kids）。永不失败。
std::vector<Node> ParseXml(const char* xml, size_t len, ParseDiag& diag) {
    std::vector<Node> top;
    std::vector<Elem*> stack;  // 当前开着的元素链（不含伪 root）

    auto kids_of_top = [&]() -> std::vector<Node>& {
        return stack.empty() ? top : stack.back()->kids;
    };
    auto append_text = [&](std::string_view raw) {
        if (raw.empty()) return;
        Node n;
        n.text = DecodeEntities(raw);
        kids_of_top().push_back(std::move(n));
    };

    size_t i = 0;
    size_t text_start = 0;
    while (i < len) {
        if (xml[i] != '<') {
            i++;
            continue;
        }
        if (i + 1 >= len) {
            // 尾部孤立 '<'（流式切分高频落点）：它是残缺 token 的开头，丢弃；之前的文本照收
            append_text(std::string_view(xml + text_start, i - text_start));
            diag.truncated = true;
            return top;
        }
        // '<' 后不是标签形态（如 "a < b"）：当字面量文本继续走
        char c1 = xml[i + 1];
        if (!IsNameStart(c1) && c1 != '/' && c1 != '!' && c1 != '?') {
            i++;
            continue;
        }
        append_text(std::string_view(xml + text_start, i - text_start));

        if (c1 == '!' || c1 == '?') {
            // 注释 / DOCTYPE / PI：跳过。注释要认 "-->"，其余认 '>'。
            if (i + 3 < len && xml[i + 2] == '-' && xml[i + 3] == '-') {
                size_t p = i + 4;
                while (p + 2 < len && !(xml[p] == '-' && xml[p + 1] == '-' && xml[p + 2] == '>')) p++;
                if (p + 2 >= len) { diag.truncated = true; return top; }
                i = p + 3;
            } else {
                size_t gt = i;
                while (gt < len && xml[gt] != '>') gt++;
                if (gt >= len) { diag.truncated = true; return top; }
                i = gt + 1;
            }
            text_start = i;
            continue;
        }

        if (c1 == '/') {
            // 闭标签：</name>
            size_t p = i + 2;
            size_t name_start = p;
            while (p < len && IsNameChar(xml[p])) p++;
            std::string name = Lower(std::string_view(xml + name_start, p - name_start));
            while (p < len && xml[p] != '>') p++;
            if (p >= len) { diag.truncated = true; return top; }
            i = p + 1;
            text_start = i;
            // HTML 传统恢复：栈里找同名，弹到它为止（中间的自动闭合）；找不到 → 忽略孤儿闭标签
            for (int d = static_cast<int>(stack.size()) - 1; d >= 0; d--) {
                if (stack[d]->tag == name) {
                    diag.auto_closed += static_cast<int>(stack.size()) - 1 - d;
                    stack.resize(static_cast<size_t>(d));
                    break;
                }
            }
            continue;
        }

        // 开标签：<name attr=... > 或 <name ... />
        size_t p = i + 1;
        size_t name_start = p;
        while (p < len && IsNameChar(xml[p])) p++;
        auto el = std::make_unique<Elem>();
        el->tag = Lower(std::string_view(xml + name_start, p - name_start));
        bool self_close = false;
        bool complete = false;
        while (p < len) {
            while (p < len && IsWs(xml[p])) p++;
            if (p >= len) break;
            if (xml[p] == '>') { complete = true; p++; break; }
            if (xml[p] == '/') {
                if (p + 1 < len && xml[p + 1] == '>') { self_close = complete = true; p += 2; break; }
                p++;  // 孤立 '/'：跳过
                continue;
            }
            if (!IsNameStart(xml[p])) { p++; continue; }  // 属性位置的杂质字符：跳过
            size_t an = p;
            while (p < len && IsNameChar(xml[p])) p++;
            std::string aname = Lower(std::string_view(xml + an, p - an));
            std::string aval;
            while (p < len && IsWs(xml[p])) p++;
            if (p < len && xml[p] == '=') {
                p++;
                while (p < len && IsWs(xml[p])) p++;
                if (p < len && (xml[p] == '"' || xml[p] == '\'')) {
                    char q = xml[p++];
                    size_t vs = p;
                    while (p < len && xml[p] != q) p++;
                    if (p >= len) { complete = false; p = len; break; }  // 引号没收尾：整元素丢弃
                    aval = DecodeEntities(std::string_view(xml + vs, p - vs));
                    p++;
                } else {  // 无引号值：到空白或 '>' 或 '/'
                    size_t vs = p;
                    while (p < len && !IsWs(xml[p]) && xml[p] != '>' && xml[p] != '/') p++;
                    aval = DecodeEntities(std::string_view(xml + vs, p - vs));
                }
            }
            el->attrs.emplace_back(std::move(aname), std::move(aval));
        }
        if (!complete) {
            // EOF 落在开标签中间：整个残缺片段丢弃（流式预览的每一帧都可能落在这），
            // 已收的前序内容照常返回。
            diag.truncated = true;
            return top;
        }
        i = p;
        text_start = i;
        bool container = !self_close && !IsVoidTag(el->tag);
        Elem* raw = el.get();
        Node n;
        n.el = std::move(el);
        kids_of_top().push_back(std::move(n));
        if (container && stack.size() < kMaxDepth) stack.push_back(raw);
    }
    if (text_start < len) append_text(std::string_view(xml + text_start, len - text_start));
    diag.auto_closed += static_cast<int>(stack.size());  // EOF：未闭合的全部自动闭合（§2.6）
    return top;
}

// ---------------------------------------------------------------------------
// Compile：元素树 → cJSON 信封
// ---------------------------------------------------------------------------

struct CompileCtx {
    std::vector<std::string> notes;
    std::set<std::string> stripped_attrs;  // 聚合成一条 note，防 style/class 刷屏
    std::set<std::string> unknown_tags;
};

bool ParseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    out = std::strtod(s.c_str(), &endp);
    if (endp == s.c_str()) return false;
    while (*endp == ' ' || *endp == '\t') endp++;
    return *endp == '\0';
}

bool ParseBool(const std::string& v) {
    // 布尔属性：空值（<label mono>）/ "1" / "true" 为真；"0"/"false" 为假；其余按真。
    return !(v == "0" || v == "false");
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsWs(s[b])) b++;
    while (e > b && IsWs(s[e - 1])) e--;
    return s.substr(b, e - b);
}

// 元素的直属+后代文本拍平成一段（b/i/em 等行内杂标签剥掉、br → 换行），供 text 内容提取。
void FlattenText(const Elem& el, std::string& out) {
    for (const auto& k : el.kids) {
        if (k.el) {
            if (k.el->tag == "br") out += '\n';
            else FlattenText(*k.el, out);
        } else {
            out += k.text;
        }
    }
}

std::string ElemText(const Elem& el) {
    std::string t;
    FlattenText(el, t);
    return Trim(t);
}

// ---- 动作微语法（§2.4）：逗号分步、每步 动词[:载荷]、载荷可用引号包逗号 ----

std::vector<std::string> SplitActionSteps(const std::string& s) {
    std::vector<std::string> steps;
    std::string cur;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            cur += c;
            quote = c;
        } else if (c == ',') {
            steps.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    steps.push_back(cur);
    return steps;
}

std::string Unquote(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

// "tap"/"change"/"release" 属性值 → action 数组；全部步都不合法 → nullptr（调用方不挂事件）。
cJSON* CompileActions(const std::string& val, const char* attr, CompileCtx& ctx) {
    cJSON* arr = cJSON_CreateArray();
    for (const std::string& raw : SplitActionSteps(val)) {
        std::string step = Trim(raw);
        if (step.empty()) continue;
        size_t colon = step.find(':');
        std::string verb = Trim(step.substr(0, colon));
        std::string payload = colon == std::string::npos ? "" : Unquote(Trim(step.substr(colon + 1)));
        cJSON* act = nullptr;
        if (verb == "close") {
            act = cJSON_CreateObject();
            cJSON_AddStringToObject(act, "do", "close");
        } else if (verb == "set") {
            size_t eq = payload.find('=');
            if (eq == std::string::npos) {
                ctx.notes.push_back(std::string("dropped ") + attr + " step 'set:" + payload +
                                    "' — set needs path=value");
            } else {
                std::string path = Trim(payload.substr(0, eq));
                std::string v = Unquote(Trim(payload.substr(eq + 1)));
                act = cJSON_CreateObject();
                cJSON_AddStringToObject(act, "do", "set");
                cJSON_AddStringToObject(act, "path", path.c_str());
                double num;
                if (ParseNumber(v, num)) cJSON_AddNumberToObject(act, "value", num);
                else cJSON_AddStringToObject(act, "value", v.c_str());  // "{i}" 行模板占位等
            }
        } else if (verb == "report") {
            act = cJSON_CreateObject();
            cJSON_AddStringToObject(act, "do", "report");
            if (!payload.empty()) cJSON_AddStringToObject(act, "text", payload.c_str());
        } else if (verb == "toggle" || verb == "show" || verb == "hide") {
            if (payload.empty()) {
                ctx.notes.push_back(std::string("dropped ") + attr + " step '" + verb +
                                    "' — needs a target id");
            } else {
                act = cJSON_CreateObject();
                cJSON_AddStringToObject(act, "do", verb.c_str());
                cJSON_AddStringToObject(act, "target", payload.c_str());
            }
        } else if (verb == "invoke") {
            if (payload.empty()) {
                ctx.notes.push_back(std::string("dropped ") + attr + " step 'invoke' — needs a cmd");
            } else {
                act = cJSON_CreateObject();
                cJSON_AddStringToObject(act, "do", "invoke");
                cJSON_AddStringToObject(act, "cmd", payload.c_str());
            }
        } else if (verb == "patch") {
            ctx.notes.push_back(std::string("dropped ") + attr +
                                " step 'patch' — not in xml v1, mutate via ui_update instead");
        } else {
            ctx.notes.push_back(std::string("dropped unknown ") + attr + " step '" + verb + "'");
        }
        if (act) cJSON_AddItemToArray(arr, act);
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return nullptr;
    }
    return arr;
}

// ---- 叶子编译 ----

// HTML 别名 → (真叶子 tag, 预置 role)。返回空串表示不是叶子别名。
std::string LeafAlias(const std::string& tag, const char** preset_role) {
    *preset_role = nullptr;
    if (tag == "h1") { *preset_role = "title"; return "label"; }
    if (tag == "h2") { *preset_role = "section"; return "label"; }
    if (tag == "h3") { *preset_role = "heading"; return "label"; }
    if (tag == "p" || tag == "span") return "label";
    if (tag == "hr" || tag == "br") return "divider";  // br 单独出现在块级：当分隔线容错
    return "";
}

bool IsLeafTag(const std::string& tag) {
    static const std::set<std::string> kLeaves = {"label",   "button", "slider", "arc",
                                                  "switch",  "bar",    "icon",   "divider",
                                                  "qrcode",  "choice", "chart",  "stock_chart"};
    return kLeaves.count(tag) != 0;
}

// 单个属性套到叶子对象上（词表内→映射，词表外→聚合剥除 note）。leaf_type 决定同名属性的
// 分派（chart 的 bind→bind_history；icon 的 name→icon）。已存在的键不覆盖（td 属性并入时
// 叶子自身优先，§2.2）。
void ApplyLeafAttr(cJSON* leaf, const std::string& type, const std::string& name,
                   const std::string& val, CompileCtx& ctx) {
    auto add_str = [&](const char* key) {
        if (!cJSON_GetObjectItem(leaf, key)) cJSON_AddStringToObject(leaf, key, val.c_str());
    };
    auto add_num = [&](const char* key) {
        double num;
        if (ParseNumber(val, num)) {
            if (!cJSON_GetObjectItem(leaf, key)) cJSON_AddNumberToObject(leaf, key, num);
        } else {
            ctx.notes.push_back("dropped non-numeric " + name + "='" + val + "' on <" + type + ">");
        }
    };
    auto add_bool = [&](const char* key) {
        if (!cJSON_GetObjectItem(leaf, key)) cJSON_AddBoolToObject(leaf, key, ParseBool(val));
    };

    if (name == "id" || name == "tone" || name == "side" || name == "text" || name == "role" ||
        name == "fmt" || name == "variant" || name == "bind_data" || name == "symbol" ||
        name == "mode") {
        add_str(name.c_str());
    } else if (name == "bind") {
        add_str(type == "chart" ? "bind_history" : "bind");  // 对模型统一心智：绑路径一律叫 bind
    } else if (name == "bind_history") {
        add_str("bind_history");
    } else if (name == "icon") {
        add_str("icon");
    } else if (name == "name") {
        if (type == "icon") add_str("icon");        // <icon name="wifi"/>
        else if (type == "stock_chart") add_str("name");
        else ctx.stripped_attrs.insert(name);
    } else if (name == "min" || name == "max" || name == "value" || name == "points") {
        add_num(name.c_str());
    } else if (name == "mono" || name == "hidden" || name == "checked") {
        add_bool(name.c_str());
    } else if (name == "options") {
        // "15分|30分|60分" 管道分隔；没有 '|' 时容错逗号分隔
        if (!cJSON_GetObjectItem(leaf, "options")) {
            char sep = val.find('|') != std::string::npos ? '|' : ',';
            cJSON* arr = cJSON_CreateArray();
            std::string cur;
            for (char c : val) {
                if (c == sep) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(Trim(cur).c_str()));
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            cJSON_AddItemToArray(arr, cJSON_CreateString(Trim(cur).c_str()));
            cJSON_AddItemToObject(leaf, "options", arr);
        }
    } else if (name == "tap" || name == "change" || name == "release") {
        const char* key = name == "tap" ? "on_click" : (name == "change" ? "on_change" : "on_release");
        if (!cJSON_GetObjectItem(leaf, key)) {
            if (cJSON* acts = CompileActions(val, name.c_str(), ctx))
                cJSON_AddItemToObject(leaf, key, acts);
        }
    } else if (name == "type") {
        // <input type=…> 已在调用方消费；其它叶子上的 type 属性剥除
        ctx.stripped_attrs.insert(name);
    } else {
        ctx.stripped_attrs.insert(name);  // style/class/onclick/href/src/… 一律剥除
    }
}

// 叶子元素（含 HTML 别名/input）→ 叶子 cJSON；不是叶子返回 nullptr。
cJSON* CompileLeaf(const Elem& el, CompileCtx& ctx) {
    const char* preset_role = nullptr;
    std::string type = el.tag;
    if (!IsLeafTag(type)) {
        std::string alias = LeafAlias(type, &preset_role);
        if (!alias.empty()) {
            type = alias;
        } else if (type == "input") {
            const std::string* t = el.Attr("type");
            if (t && *t == "range") type = "slider";
            else if (t && *t == "checkbox") type = "switch";
            else {
                ctx.notes.push_back("skipped <input type='" + (t ? *t : std::string("?")) +
                                    "'> — only range/checkbox map to controls");
                return nullptr;
            }
        } else {
            return nullptr;
        }
    }
    cJSON* leaf = cJSON_CreateObject();
    cJSON_AddStringToObject(leaf, "type", type.c_str());
    for (const auto& a : el.attrs) ApplyLeafAttr(leaf, type, a.first, a.second, ctx);
    if (preset_role && !cJSON_GetObjectItem(leaf, "role"))
        cJSON_AddStringToObject(leaf, "role", preset_role);
    // 元素文本内容 = text 属性（属性优先，§2.3）；qrcode 的内容也进 text（二维码载荷）
    if ((type == "label" || type == "button" || type == "qrcode") &&
        !cJSON_GetObjectItem(leaf, "text")) {
        std::string t = ElemText(el);
        if (!t.empty()) cJSON_AddStringToObject(leaf, "text", t.c_str());
    }
    return leaf;
}

// ---- 块编译 ----

bool IsGridTag(const std::string& tag) { return tag == "grid" || tag == "div" || tag == "section"; }
bool IsTableTag(const std::string& tag) { return tag == "table" || tag == "ul" || tag == "ol"; }

// cols="项,值:num" → [{"title":"项"},{"title":"值","num":true}]
cJSON* CompileColsAttr(const std::string& val) {
    cJSON* cols = cJSON_CreateArray();
    std::string cur;
    auto flush = [&]() {
        std::string seg = Trim(cur);
        cur.clear();
        cJSON* col = cJSON_CreateObject();
        bool num = false;
        if (seg.size() >= 4 && seg.compare(seg.size() - 4, 4, ":num") == 0) {
            num = true;
            seg = Trim(seg.substr(0, seg.size() - 4));
        }
        if (!seg.empty()) cJSON_AddStringToObject(col, "title", seg.c_str());
        if (num) cJSON_AddBoolToObject(col, "num", true);
        cJSON_AddItemToArray(cols, col);
    };
    for (char c : val) {
        if (c == ',') flush();
        else cur += c;
    }
    flush();
    return cols;
}

// 块级公共属性（fill/id/hidden，grid/table/list 三形态同权——"让自然写法合法"：模型折叠
// 一个分组/表格是高频直觉，id/hidden 下沉到块级后 toggle:块id 直接合法）。返回 true 表示
// 该属性已消费。
bool ApplyBlockAttr(cJSON* grid, const std::string& name, const std::string& val) {
    if (name == "fill" || name == "id") {
        if (!cJSON_GetObjectItem(grid, name.c_str()))
            cJSON_AddStringToObject(grid, name.c_str(), val.c_str());
        return true;
    }
    if (name == "hidden") {
        if (!cJSON_GetObjectItem(grid, "hidden"))
            cJSON_AddBoolToObject(grid, "hidden", ParseBool(val));
        return true;
    }
    return false;
}

// <td>/<th>/<li> → 恰一个叶子：优先取首个叶子元素子节点（td 属性并入、叶子自身优先），
// 否则拍平文本 → label。多叶子取第一个 + note（决策 D4：v1 恰一个）。
cJSON* CompileCell(const Elem& cell, CompileCtx& ctx) {
    cJSON* leaf = nullptr;
    int leaf_count = 0;
    for (const auto& k : cell.kids) {
        if (!k.el) continue;
        cJSON* l = CompileLeaf(*k.el, ctx);
        if (!l) continue;
        leaf_count++;
        if (!leaf) leaf = l;
        else cJSON_Delete(l);
    }
    if (leaf_count > 1)
        ctx.notes.push_back("a <" + cell.tag + "> holds one leaf — extra " +
                            std::to_string(leaf_count - 1) + " dropped");
    if (!leaf) {
        leaf = cJSON_CreateObject();
        cJSON_AddStringToObject(leaf, "type", "label");
        std::string t = ElemText(cell);
        if (!t.empty()) cJSON_AddStringToObject(leaf, "text", t.c_str());
    }
    // td 自身属性并入叶子（叶子已有的键优先）
    const cJSON* ty = cJSON_GetObjectItem(leaf, "type");
    std::string type = cJSON_IsString(ty) ? ty->valuestring : "label";
    for (const auto& a : cell.attrs) ApplyLeafAttr(leaf, type, a.first, a.second, ctx);
    return leaf;
}

// <table>/<ul>/<ol> → rows 形态 grid。空表返回 nullptr（调用方丢弃 + note）。
cJSON* CompileTable(const Elem& el, CompileCtx& ctx) {
    cJSON* grid = cJSON_CreateObject();
    cJSON* cols = nullptr;
    if (const std::string* c = el.Attr("cols")) cols = CompileColsAttr(*c);
    for (const auto& a : el.attrs) {
        if (a.first == "cols") continue;
        if (!ApplyBlockAttr(grid, a.first, a.second)) ctx.stripped_attrs.insert(a.first);
    }

    cJSON* rows = cJSON_CreateArray();
    std::vector<const Elem*> stray_cells;  // 直接躺在 table 下的 td（漏了 tr）：并成一行
    bool first_tr = true;
    for (const auto& k : el.kids) {
        if (!k.el) continue;  // 表格层级的裸文本：忽略（多为排版空白）
        const Elem& kid = *k.el;
        if (kid.tag == "tr") {
            cJSON* row = cJSON_CreateArray();
            bool all_th = true;
            int ncell = 0;
            for (const auto& c : kid.kids) {
                if (!c.el) continue;
                if (c.el->tag == "td" || c.el->tag == "th") {
                    if (c.el->tag != "th") all_th = false;
                    cJSON_AddItemToArray(row, CompileCell(*c.el, ctx));
                    ncell++;
                } else if (cJSON* l = CompileLeaf(*c.el, ctx)) {
                    all_th = false;
                    cJSON_AddItemToArray(row, l);  // tr 里直接躺叶子：当一格
                    ncell++;
                }
            }
            if (ncell == 0) {
                cJSON_Delete(row);
                continue;
            }
            // HTML 先验容错：首行全 <th> 且没写 cols 属性 → 当表头（title 取 th 文本）
            if (first_tr && all_th && !cols) {
                cols = cJSON_CreateArray();
                cJSON* c = nullptr;
                cJSON_ArrayForEach(c, row) {
                    cJSON* col = cJSON_CreateObject();
                    const cJSON* t = cJSON_GetObjectItem(c, "text");
                    if (cJSON_IsString(t)) cJSON_AddStringToObject(col, "title", t->valuestring);
                    cJSON_AddItemToArray(cols, col);
                }
                cJSON_Delete(row);
                ctx.notes.push_back("first <th> row became cols header");
            } else {
                cJSON_AddItemToArray(rows, row);
            }
            first_tr = false;
        } else if (kid.tag == "li") {
            cJSON* row = cJSON_CreateArray();  // ul/ol：每 li 一行单列（§2.5）
            cJSON_AddItemToArray(row, CompileCell(kid, ctx));
            cJSON_AddItemToArray(rows, row);
        } else if (kid.tag == "td" || kid.tag == "th") {
            stray_cells.push_back(&kid);
        } else if (cJSON* l = CompileLeaf(kid, ctx)) {
            cJSON* row = cJSON_CreateArray();  // table 下直接躺叶子：单格行（竖排列表语义）
            cJSON_AddItemToArray(row, l);
            cJSON_AddItemToArray(rows, row);
        } else {
            ctx.unknown_tags.insert(kid.tag);
        }
    }
    if (!stray_cells.empty()) {
        cJSON* row = cJSON_CreateArray();
        for (const Elem* c : stray_cells) cJSON_AddItemToArray(row, CompileCell(*c, ctx));
        cJSON_AddItemToArray(rows, row);
        ctx.notes.push_back("<td> outside <tr> merged into one row");
    }
    if (cJSON_GetArraySize(rows) == 0) {
        cJSON_Delete(rows);
        if (cols) cJSON_Delete(cols);
        cJSON_Delete(grid);
        return nullptr;
    }
    if (cols) cJSON_AddItemToObject(grid, "cols", cols);
    cJSON_AddItemToObject(grid, "rows", rows);
    return grid;
}

// <list bind="key" max= empty=> 模板叶子 </list> → bind_rows 形态。没 bind → 退化 cells。
cJSON* CompileList(const Elem& el, CompileCtx& ctx, std::vector<cJSON*>& promoted);

// <grid>（含 div/section 别名）→ cells 形态。嵌套容器拍平一层进当前 cells（§2.1 剥壳）；
// 嵌套 table/list 无法拍平 → 提升成兄弟块（promoted，插在本 grid 之后）。空 grid → nullptr。
cJSON* CompileGrid(const Elem& el, CompileCtx& ctx, std::vector<cJSON*>& promoted) {
    cJSON* grid = cJSON_CreateObject();
    for (const auto& a : el.attrs) {
        if (ApplyBlockAttr(grid, a.first, a.second)) continue;
        if (a.first == "tap" || a.first == "change" || a.first == "release") {
            ctx.notes.push_back("grid-level '" + a.first + "' dropped — put events on a leaf");
        } else {
            ctx.stripped_attrs.insert(a.first);
        }
    }
    cJSON* cells = cJSON_CreateArray();
    // 保序 DFS（索引栈）：叶子直收，嵌套容器拍平（子内容原位提升），裸文本 → label。
    struct Frame {
        const std::vector<Node>* kids;
        size_t idx = 0;
    };
    std::vector<Frame> frames{{&el.kids, 0}};
    bool flattened = false;
    while (!frames.empty()) {
        Frame& f = frames.back();
        if (f.idx >= f.kids->size()) {
            frames.pop_back();
            continue;
        }
        const Node& n = (*f.kids)[f.idx++];
        if (!n.el) {
            std::string t = Trim(n.text);
            if (!t.empty()) {
                cJSON* l = cJSON_CreateObject();
                cJSON_AddStringToObject(l, "type", "label");
                cJSON_AddStringToObject(l, "text", t.c_str());
                cJSON_AddItemToArray(cells, l);
            }
            continue;
        }
        const Elem& kid = *n.el;
        if (cJSON* leaf = CompileLeaf(kid, ctx)) {
            cJSON_AddItemToArray(cells, leaf);
        } else if (IsGridTag(kid.tag) || kid.tag == "card") {
            flattened = true;  // grid 里套容器：拍平（子内容提升进当前 cells）
            frames.push_back({&kid.kids, 0});
        } else if (IsTableTag(kid.tag)) {
            if (cJSON* g = CompileTable(kid, ctx)) {
                // 外层 grid 的 hidden 传播给被提升的块（视觉上同组同隐）；id 不复制（唯一性）
                // ——想整组折叠，直接给 <table> 挂自己的 id/hidden。
                if (el.Attr("hidden") && !cJSON_GetObjectItem(g, "hidden"))
                    cJSON_AddBoolToObject(g, "hidden", ParseBool(*el.Attr("hidden")));
                promoted.push_back(g);
            }
            ctx.notes.push_back("a <table> nested in <grid> was promoted to its own block — give "
                                "the <table> its own id/hidden to fold it");
        } else if (kid.tag == "list") {
            if (cJSON* g = CompileList(kid, ctx, promoted)) {
                if (el.Attr("hidden") && !cJSON_GetObjectItem(g, "hidden"))
                    cJSON_AddBoolToObject(g, "hidden", ParseBool(*el.Attr("hidden")));
                promoted.push_back(g);
            }
            ctx.notes.push_back("a <list> nested in <grid> was promoted to its own block — give "
                                "the <list> its own id/hidden to fold it");
        } else if (kid.tag == "tr" || kid.tag == "td" || kid.tag == "th" || kid.tag == "li") {
            frames.push_back({&kid.kids, 0});  // 迷路的表格件：剥壳取其内容
        } else if (!kid.kids.empty()) {
            ctx.unknown_tags.insert(kid.tag);
            frames.push_back({&kid.kids, 0});  // 未知标签：剥壳保留子树（§2.5）
        } else {
            ctx.unknown_tags.insert(kid.tag);  // 未知空标签：整个跳过
        }
    }
    if (flattened) ctx.notes.push_back("nested containers inside <grid> flattened — depth is card>grid>leaf");
    if (cJSON_GetArraySize(cells) == 0) {
        cJSON_Delete(cells);
        // 空壳但有提升块：壳上的 id/fill 转移给第一个提升块——<grid id="x" hidden> 只包一个
        // <table> 是模型高频写法（真机 toggle:m2 实录），语义 ≡ <table id="x" hidden>；壳丢
        // id 会让 toggle 目标落空连环拒（hidden 已在提升时传播）。
        if (!promoted.empty()) {
            cJSON* first = promoted.front();
            for (const char* key : {"id", "fill"}) {
                const cJSON* v = cJSON_GetObjectItem(grid, key);
                if (cJSON_IsString(v) && !cJSON_GetObjectItem(first, key))
                    cJSON_AddStringToObject(first, key, v->valuestring);
            }
        }
        cJSON_Delete(grid);
        return nullptr;
    }
    cJSON_AddItemToObject(grid, "cells", cells);
    return grid;
}

cJSON* CompileList(const Elem& el, CompileCtx& ctx, std::vector<cJSON*>& promoted) {
    const std::string* bind = el.Attr("bind");
    if (!bind || bind->empty()) {
        ctx.notes.push_back("<list> without bind=\"dataKey\" degraded to a plain grid");
        return CompileGrid(el, ctx, promoted);
    }
    cJSON* grid = cJSON_CreateObject();
    cJSON* item = cJSON_CreateArray();
    for (const auto& k : el.kids) {
        if (!k.el) continue;
        if (cJSON* leaf = CompileLeaf(*k.el, ctx)) cJSON_AddItemToArray(item, leaf);
        else ctx.unknown_tags.insert(k.el->tag);
    }
    if (cJSON_GetArraySize(item) == 0) {
        // 模板空：给个 {item.title} 兜底比拒卡友好？——不发明内容，丢块 + note。
        cJSON_Delete(item);
        cJSON_Delete(grid);
        ctx.notes.push_back("<list> with no template leaves dropped");
        return nullptr;
    }
    cJSON_AddItemToObject(grid, "item", item);
    cJSON_AddStringToObject(grid, "bind_rows", bind->c_str());
    for (const auto& a : el.attrs) {
        if (a.first == "bind") continue;
        if (a.first == "max") {
            double n;
            if (ParseNumber(a.second, n)) cJSON_AddNumberToObject(grid, "max", n);
        } else if (a.first == "empty") {
            cJSON_AddStringToObject(grid, "empty", a.second.c_str());
        } else if (!ApplyBlockAttr(grid, a.first, a.second)) {
            ctx.stripped_attrs.insert(a.first);
        }
    }
    return grid;
}

// <data> → 卡级 data 对象。标量 <key>值</key>；记录行 <key attr=… /> 重复出现即数组。
void CompileData(const Elem& el, cJSON* data, CompileCtx& ctx) {
    for (const auto& k : el.kids) {
        if (!k.el) continue;
        const Elem& kid = *k.el;
        const std::string& key = kid.tag;
        if (!kid.attrs.empty()) {
            // 记录：属性 → 字段（数值样式转 number）；同名累积成数组
            cJSON* rec = cJSON_CreateObject();
            for (const auto& a : kid.attrs) {
                double num;
                if (ParseNumber(a.second, num)) cJSON_AddNumberToObject(rec, a.first.c_str(), num);
                else cJSON_AddStringToObject(rec, a.first.c_str(), a.second.c_str());
            }
            cJSON* arr = cJSON_GetObjectItem(data, key.c_str());
            if (!cJSON_IsArray(arr)) {
                cJSON_DeleteItemFromObject(data, key.c_str());
                arr = cJSON_CreateArray();
                cJSON_AddItemToObject(data, key.c_str(), arr);
            }
            cJSON_AddItemToArray(arr, rec);
        } else {
            std::string t = ElemText(kid);
            double num;
            cJSON* val = ParseNumber(t, num) ? cJSON_CreateNumber(num) : cJSON_CreateString(t.c_str());
            cJSON* prev = cJSON_GetObjectItem(data, key.c_str());
            if (!prev) {
                cJSON_AddItemToObject(data, key.c_str(), val);
            } else if (cJSON_IsArray(prev)) {
                cJSON_AddItemToArray(prev, val);
            } else {  // 第二次出现同名标量：升级成数组
                cJSON* arr = cJSON_CreateArray();
                cJSON_AddItemToArray(arr, cJSON_Duplicate(prev, 1));
                cJSON_AddItemToArray(arr, val);
                cJSON_ReplaceItemInObject(data, key.c_str(), arr);
            }
        }
    }
    (void)ctx;
}

// ttl："30s" / "500ms" / 裸数字（毫秒）→ ms。解析不动 → -1。
int ParseTtl(const std::string& raw) {
    std::string s = Trim(raw);
    double mult = 1.0;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "ms") == 0) {
        s = s.substr(0, s.size() - 2);
    } else if (s.size() > 1 && (s.back() == 's' || s.back() == 'S')) {
        mult = 1000.0;
        s = s.substr(0, s.size() - 1);
    }
    double n;
    if (!ParseNumber(s, n) || n < 0) return -1;
    return static_cast<int>(n * mult);
}

}  // namespace

// ---------------------------------------------------------------------------

bool XmlCompile(const char* xml, size_t len, cJSON** out_args, std::vector<std::string>* notes,
                std::string& err) {
    *out_args = nullptr;
    if (!xml) len = 0;
    if (len > 128 * 1024) {
        err = "xml too large (>128KB) — cards are small, emit a compact <card>";
        return false;
    }

    ParseDiag diag;
    std::vector<Node> top = ParseXml(xml, len, diag);

    // 整段输入连一个元素都没有：这是唯一的失败面（err 给可修复引导，宽进严出的兜底线）
    bool any_elem = false;
    for (const auto& n : top)
        if (n.el) { any_elem = true; break; }
    if (!any_elem) {
        err = "no XML elements found — emit <card>…</card> containing <grid>/<table>/<list> "
              "blocks of leaves (see the ui_render description)";
        return false;
    }

    CompileCtx ctx;
    cJSON* env = cJSON_CreateObject();
    cJSON* root = cJSON_CreateArray();
    cJSON* data = nullptr;

    // 定位 <card>：首个 card 元素的 kids 为卡内容；没有 card 包裹 → 顶层节点整体当卡内容。
    const Elem* card = nullptr;
    int extra_cards = 0;
    for (const auto& n : top) {
        if (n.el && n.el->tag == "card") {
            if (!card) card = n.el.get();
            else extra_cards++;
        }
    }
    if (extra_cards > 0)
        ctx.notes.push_back("only the first <card> renders — emit exactly one <card> per ui_render");

    // card 属性 → 信封键
    if (card) {
        for (const auto& a : card->attrs) {
            if (a.first == "display") {
                if (a.second == "chat" || a.second == "overlay" || a.second == "standby") {
                    cJSON_AddStringToObject(env, "display", a.second.c_str());
                } else {
                    ctx.notes.push_back("unknown display='" + a.second + "' ignored (chat|overlay|standby)");
                }
            } else if (a.first == "ttl") {
                int ms = ParseTtl(a.second);
                if (ms >= 0) cJSON_AddNumberToObject(env, "ttl_ms", ms);
                else ctx.notes.push_back("bad ttl='" + a.second + "' ignored (ms number or '30s')");
            } else if (a.first == "id" || a.first == "card") {
                cJSON_AddStringToObject(env, "card", a.second.c_str());
            } else {
                ctx.stripped_attrs.insert(a.first);
            }
        }
    } else {
        ctx.notes.push_back("missing <card> wrapper — treated the input as card content");
    }

    // 卡内容遍历：块直译；散叶子聚成 cells grid；<data> 汇入信封。
    std::vector<cJSON*> pending;             // 连续散叶子（含块级 divider——leaf divider 在
                                             // cells 里本就 SPAN_ALL 独占行，语义等价 §2.2）
    std::vector<cJSON*> blocks;              // 编译完成的 grid 块序列
    auto flush_pending = [&]() {
        if (pending.empty()) return;
        cJSON* g = cJSON_CreateObject();
        cJSON* cells = cJSON_CreateArray();
        for (cJSON* l : pending) cJSON_AddItemToArray(cells, l);
        pending.clear();
        cJSON_AddItemToObject(g, "cells", cells);
        blocks.push_back(g);
    };

    // 未定位到 card 时把顶层当内容；定位到 card 时只走 card->kids（伪 root 的其余兄弟忽略）
    struct Frame {
        const std::vector<Node>* kids;
        size_t idx = 0;
    };
    std::vector<Frame> frames{{card ? &card->kids : &top, 0}};
    while (!frames.empty()) {
        Frame& f = frames.back();
        if (f.idx >= f.kids->size()) {
            frames.pop_back();
            continue;
        }
        const Node& n = (*f.kids)[f.idx++];
        if (!n.el) {
            std::string t = Trim(n.text);
            if (!t.empty()) {  // 卡级裸文本 → label 散叶（HTML body 先验）
                cJSON* l = cJSON_CreateObject();
                cJSON_AddStringToObject(l, "type", "label");
                cJSON_AddStringToObject(l, "text", t.c_str());
                pending.push_back(l);
            }
            continue;
        }
        const Elem& kid = *n.el;
        std::vector<cJSON*> promoted;
        if (kid.tag == "data") {
            if (!data) data = cJSON_CreateObject();
            CompileData(kid, data, ctx);
        } else if (IsGridTag(kid.tag)) {
            flush_pending();
            cJSON* g = CompileGrid(kid, ctx, promoted);
            if (g) blocks.push_back(g);
            else if (promoted.empty()) ctx.notes.push_back("empty <" + kid.tag + "> dropped");
            for (cJSON* p : promoted) blocks.push_back(p);
        } else if (IsTableTag(kid.tag)) {
            flush_pending();
            if (cJSON* g = CompileTable(kid, ctx)) blocks.push_back(g);
            else ctx.notes.push_back("empty <" + kid.tag + "> dropped");
        } else if (kid.tag == "list") {
            flush_pending();
            if (cJSON* g = CompileList(kid, ctx, promoted)) blocks.push_back(g);
            for (cJSON* p : promoted) blocks.push_back(p);
        } else if (cJSON* leaf = CompileLeaf(kid, ctx)) {
            pending.push_back(leaf);
        } else if (kid.tag == "card" || kid.tag == "html" || kid.tag == "body") {
            frames.push_back({&kid.kids, 0});  // 嵌套 card / html 包裹：剥壳
            if (kid.tag != "card") ctx.unknown_tags.insert(kid.tag);
        } else if (kid.tag == "tr" || kid.tag == "td" || kid.tag == "th" || kid.tag == "li") {
            frames.push_back({&kid.kids, 0});  // 迷路的表格件：剥壳
        } else if (!kid.kids.empty()) {
            ctx.unknown_tags.insert(kid.tag);
            frames.push_back({&kid.kids, 0});  // 未知带子树：剥壳保留（§2.5）
        } else {
            ctx.unknown_tags.insert(kid.tag);  // 未知空元素：跳过
        }
    }
    flush_pending();

    for (cJSON* b : blocks) cJSON_AddItemToArray(root, b);
    cJSON_AddItemToObject(env, "root", root);
    if (data) cJSON_AddItemToObject(env, "data", data);

    // 聚合 note（unknown attrs/tags 各一条，防刷屏）；截断/自动闭合提示放最后。
    if (!ctx.stripped_attrs.empty()) {
        std::string s = "stripped unknown attrs:";
        for (const auto& a : ctx.stripped_attrs) s += " " + a;
        ctx.notes.push_back(s);
    }
    if (!ctx.unknown_tags.empty()) {
        std::string s = "unknown tags unwrapped/skipped:";
        for (const auto& t : ctx.unknown_tags) s += " <" + t + ">";
        ctx.notes.push_back(s);
    }
    if (diag.truncated)
        ctx.notes.push_back("input ended mid-tag — the incomplete fragment was dropped");
    else if (diag.auto_closed > 0)
        ctx.notes.push_back(std::to_string(diag.auto_closed) + " unclosed tag(s) auto-closed");

    if (notes)
        for (auto& s : ctx.notes) notes->push_back(std::move(s));
    *out_args = env;
    return true;
}

}  // namespace pi_card
