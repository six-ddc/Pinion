// card_xml_test —— pi_card XML 线格式编译器（docs/CARD_XML.md）宿主单测。
//
// 覆盖：§2.2/2.3 块与叶子全映射、§2.4 动作微语法（含引号包逗号）、§2.5 HTML 容错逐条、
// §2.6 转义/裸 &/未闭合/截断恢复、<td> 双形态、<data> 线格式、错误降级 note 断言、
// 前缀切分不变量（任意字节切分点喂前缀不崩、全量前缀 ≡ 一次性编译）。
//
//   cmake --build sim/build --target card_xml_test && ./sim/build/card_xml_test

#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "minitest.h"
#include "pi_card_xml.h"

using pi_card::XmlCompile;

namespace {

// 便捷封装：编译并返回信封（调用方 cJSON_Delete）；期望成功。
cJSON* Compile(const std::string& xml, std::vector<std::string>* notes = nullptr) {
    cJSON* env = nullptr;
    std::string err;
    bool ok = XmlCompile(xml.c_str(), xml.size(), &env, notes, err);
    CHECK(ok);
    CHECK(env != nullptr);
    return env;
}

// 信封里第 i 个 grid 块
cJSON* Grid(cJSON* env, int i) {
    cJSON* root = cJSON_GetObjectItem(env, "root");
    CHECK(cJSON_IsArray(root));
    return cJSON_GetArrayItem(root, i);
}

// grid.cells[i]
cJSON* Cell(cJSON* env, int gi, int ci) {
    cJSON* cells = cJSON_GetObjectItem(Grid(env, gi), "cells");
    CHECK(cJSON_IsArray(cells));
    return cJSON_GetArrayItem(cells, ci);
}

const char* Str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "(missing)";
}

double Num(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -999999;
}

bool HasNote(const std::vector<std::string>& notes, const char* substr) {
    for (const auto& n : notes)
        if (n.find(substr) != std::string::npos) return true;
    return false;
}

std::string CompactJson(const cJSON* j) {
    char* s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "";
    free(s);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 顶层信封（§2.1）
// ---------------------------------------------------------------------------

TEST_CASE("最小卡：card+grid+label 文本内容") {
    cJSON* env = Compile("<card><grid><label>你好</label></grid></card>");
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 1);
    cJSON* leaf = Cell(env, 0, 0);
    CHECK_EQ(std::string(Str(leaf, "type")), "label");
    CHECK_EQ(std::string(Str(leaf, "text")), "你好");
    CHECK(cJSON_GetObjectItem(env, "display") == nullptr);  // 未写不落键
    cJSON_Delete(env);
}

TEST_CASE("card 属性：display/ttl 秒缩写/id→card") {
    cJSON* env = Compile("<card display=\"overlay\" ttl=\"30s\" id=\"c1\"><grid><label>x</label></grid></card>");
    CHECK_EQ(std::string(Str(env, "display")), "overlay");
    CHECK_EQ(Num(env, "ttl_ms"), 30000);
    CHECK_EQ(std::string(Str(env, "card")), "c1");
    cJSON_Delete(env);
}

TEST_CASE("ttl 裸数字=毫秒 / ms 后缀 / 坏值降级 note") {
    cJSON* env = Compile("<card ttl=\"1500\"><grid><label>x</label></grid></card>");
    CHECK_EQ(Num(env, "ttl_ms"), 1500);
    cJSON_Delete(env);
    env = Compile("<card ttl=\"800ms\"><grid><label>x</label></grid></card>");
    CHECK_EQ(Num(env, "ttl_ms"), 800);
    cJSON_Delete(env);
    std::vector<std::string> notes;
    env = Compile("<card ttl=\"forever\" display=\"modal\"><grid><label>x</label></grid></card>", &notes);
    CHECK(cJSON_GetObjectItem(env, "ttl_ms") == nullptr);
    CHECK(cJSON_GetObjectItem(env, "display") == nullptr);
    CHECK(HasNote(notes, "bad ttl"));
    CHECK(HasNote(notes, "unknown display"));
    cJSON_Delete(env);
}

TEST_CASE("缺 card 包裹：容错 + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<grid><label>裸的</label></grid>", &notes);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 1);
    CHECK(HasNote(notes, "missing <card>"));
    cJSON_Delete(env);
}

TEST_CASE("多个 card：只取第一个 + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><label>A</label></grid></card><card><grid><label>B</label></grid></card>", &notes);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 1);
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "A");
    CHECK(HasNote(notes, "first <card>"));
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// 叶子映射（§2.3）
// ---------------------------------------------------------------------------

TEST_CASE("label 全属性：role/bind/fmt/mono 布尔裸属性") {
    cJSON* env = Compile("<card><grid><label role=\"value\" bind=\"battery.level\" fmt=\"%d%%\" mono/></grid></card>");
    cJSON* l = Cell(env, 0, 0);
    CHECK_EQ(std::string(Str(l, "role")), "value");
    CHECK_EQ(std::string(Str(l, "bind")), "battery.level");
    CHECK_EQ(std::string(Str(l, "fmt")), "%d%%");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(l, "mono")));
    cJSON_Delete(env);
}

TEST_CASE("布尔属性取值表：1/true/空=真，0/false=假") {
    cJSON* env = Compile(
        "<card><grid>"
        "<label mono=\"1\">a</label><label mono=\"true\">b</label>"
        "<label mono=\"0\">c</label><label mono=\"false\">d</label>"
        "<switch checked/>"
        "</grid></card>");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(Cell(env, 0, 0), "mono")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(Cell(env, 0, 1), "mono")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(Cell(env, 0, 2), "mono")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(Cell(env, 0, 3), "mono")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(Cell(env, 0, 4), "checked")));
    cJSON_Delete(env);
}

TEST_CASE("text 属性优先于元素内容") {
    cJSON* env = Compile("<card><grid><button text=\"属性版\">内容版</button></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "属性版");
    cJSON_Delete(env);
}

TEST_CASE("数值属性：slider min/max/value；非数值 value 丢弃+note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><slider min=\"0\" max=\"100\" value=\"60\"/>"
                         "<bar value=\"abc\"/></grid></card>", &notes);
    cJSON* s = Cell(env, 0, 0);
    CHECK_EQ(Num(s, "min"), 0);
    CHECK_EQ(Num(s, "max"), 100);
    CHECK_EQ(Num(s, "value"), 60);
    CHECK(cJSON_GetObjectItem(Cell(env, 0, 1), "value") == nullptr);
    CHECK(HasNote(notes, "non-numeric"));
    cJSON_Delete(env);
}

TEST_CASE("choice：options 管道分隔 + 逗号容错") {
    cJSON* env = Compile("<card><grid><choice id=\"dur\" options=\"15分|30分|60分\" value=\"1\"/>"
                         "<choice options=\"a, b, c\"/></grid></card>");
    cJSON* opts = cJSON_GetObjectItem(Cell(env, 0, 0), "options");
    CHECK_EQ(cJSON_GetArraySize(opts), 3);
    CHECK_EQ(std::string(cJSON_GetArrayItem(opts, 1)->valuestring), "30分");
    CHECK_EQ(Num(Cell(env, 0, 0), "value"), 1);
    cJSON* opts2 = cJSON_GetObjectItem(Cell(env, 0, 1), "options");
    CHECK_EQ(cJSON_GetArraySize(opts2), 3);
    CHECK_EQ(std::string(cJSON_GetArrayItem(opts2, 2)->valuestring), "c");
    cJSON_Delete(env);
}

TEST_CASE("icon name→icon；qrcode 内容→text；chart bind→bind_history") {
    cJSON* env = Compile("<card><grid><icon name=\"wifi\"/><qrcode>https://a.b/c?d=1</qrcode>"
                         "<chart bind=\"battery.level\" points=\"60\"/></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "icon")), "wifi");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "text")), "https://a.b/c?d=1");
    cJSON* ch = Cell(env, 0, 2);
    CHECK_EQ(std::string(Str(ch, "bind_history")), "battery.level");
    CHECK(cJSON_GetObjectItem(ch, "bind") == nullptr);
    CHECK_EQ(Num(ch, "points"), 60);
    cJSON_Delete(env);
}

TEST_CASE("stock_chart：symbol/name/mode 直传") {
    cJSON* env = Compile("<card><grid><stock_chart symbol=\"sh600519\" name=\"茅台\" mode=\"day\"/></grid></card>");
    cJSON* l = Cell(env, 0, 0);
    CHECK_EQ(std::string(Str(l, "symbol")), "sh600519");
    CHECK_EQ(std::string(Str(l, "name")), "茅台");
    CHECK_EQ(std::string(Str(l, "mode")), "day");
    cJSON_Delete(env);
}

TEST_CASE("side/tone/id/hidden 通用属性") {
    cJSON* env = Compile("<card><grid><switch side=\"end\" id=\"sw\" hidden/><label tone=\"ok\">在线</label></grid></card>");
    cJSON* sw = Cell(env, 0, 0);
    CHECK_EQ(std::string(Str(sw, "side")), "end");
    CHECK_EQ(std::string(Str(sw, "id")), "sw");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(sw, "hidden")));
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "tone")), "ok");
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// 动作微语法（§2.4）
// ---------------------------------------------------------------------------

TEST_CASE("tap 多步：report+close；变体/图标") {
    cJSON* env = Compile("<card><grid><button variant=\"primary\" icon=\"play\" side=\"end\" "
                         "tap=\"report:开始,close\">开始</button></grid></card>");
    cJSON* b = Cell(env, 0, 0);
    CHECK_EQ(std::string(Str(b, "variant")), "primary");
    CHECK_EQ(std::string(Str(b, "icon")), "play");
    cJSON* acts = cJSON_GetObjectItem(b, "on_click");
    CHECK_EQ(cJSON_GetArraySize(acts), 2);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(acts, 0), "do")), "report");
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(acts, 0), "text")), "开始");
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(acts, 1), "do")), "close");
    cJSON_Delete(env);
}

TEST_CASE("set：数值载荷转 number、{i} 保持字符串") {
    cJSON* env = Compile("<card><grid><slider change=\"set:audio.volume=50\"/>"
                         "<button tap=\"set:media.play_index={i}\">播</button></grid></card>");
    cJSON* a0 = cJSON_GetArrayItem(cJSON_GetObjectItem(Cell(env, 0, 0), "on_change"), 0);
    CHECK_EQ(std::string(Str(a0, "path")), "audio.volume");
    CHECK(cJSON_IsNumber(cJSON_GetObjectItem(a0, "value")));
    CHECK_EQ(Num(a0, "value"), 50);
    cJSON* a1 = cJSON_GetArrayItem(cJSON_GetObjectItem(Cell(env, 0, 1), "on_click"), 0);
    CHECK_EQ(std::string(Str(a1, "value")), "{i}");
    cJSON_Delete(env);
}

TEST_CASE("report 含逗号：引号包住不拆步") {
    cJSON* env = Compile("<card><grid><button tap=\"report:'选了A,不选B',close\">选</button></grid></card>");
    cJSON* acts = cJSON_GetObjectItem(Cell(env, 0, 0), "on_click");
    CHECK_EQ(cJSON_GetArraySize(acts), 2);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(acts, 0), "text")), "选了A,不选B");
    cJSON_Delete(env);
}

TEST_CASE("toggle/show/hide/invoke 与 release 事件") {
    cJSON* env = Compile("<card><grid>"
                         "<button tap=\"toggle:hist\">展开</button>"
                         "<button tap=\"show:a,hide:b\">切</button>"
                         "<button tap=\"invoke:net.reconnect\">重连</button>"
                         "<slider release=\"set:display.brightness=80\"/>"
                         "</grid></card>");
    cJSON* t = cJSON_GetArrayItem(cJSON_GetObjectItem(Cell(env, 0, 0), "on_click"), 0);
    CHECK_EQ(std::string(Str(t, "do")), "toggle");
    CHECK_EQ(std::string(Str(t, "target")), "hist");
    cJSON* sh = cJSON_GetObjectItem(Cell(env, 0, 1), "on_click");
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(sh, 0), "do")), "show");
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(sh, 1), "do")), "hide");
    cJSON* inv = cJSON_GetArrayItem(cJSON_GetObjectItem(Cell(env, 0, 2), "on_click"), 0);
    CHECK_EQ(std::string(Str(inv, "cmd")), "net.reconnect");
    cJSON* rel = cJSON_GetArrayItem(cJSON_GetObjectItem(Cell(env, 0, 3), "on_release"), 0);
    CHECK_EQ(std::string(Str(rel, "do")), "set");
    cJSON_Delete(env);
}

TEST_CASE("未知动词/patch/残缺 set 丢步 + note；全废→不挂事件") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid>"
                         "<button tap=\"explode:now,close\">a</button>"
                         "<button tap=\"patch:x.text=1\">b</button>"
                         "<button tap=\"set:no_equals\">c</button>"
                         "</grid></card>", &notes);
    cJSON* a = cJSON_GetObjectItem(Cell(env, 0, 0), "on_click");
    CHECK_EQ(cJSON_GetArraySize(a), 1);  // explode 丢了，close 保留
    CHECK(cJSON_GetObjectItem(Cell(env, 0, 1), "on_click") == nullptr);  // patch 全废
    CHECK(cJSON_GetObjectItem(Cell(env, 0, 2), "on_click") == nullptr);
    CHECK(HasNote(notes, "unknown"));
    CHECK(HasNote(notes, "patch"));
    CHECK(HasNote(notes, "path=value"));
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// table / list / divider 块（§2.2）
// ---------------------------------------------------------------------------

TEST_CASE("table：cols 属性 :num 后缀 + tr/td 二维") {
    cJSON* env = Compile("<card><table cols=\"项,今日:num,昨日:num\">"
                         "<tr><td>温度</td><td>24</td><td>22</td></tr>"
                         "<tr><td>湿度</td><td>60</td><td>55</td></tr>"
                         "</table></card>");
    cJSON* g = Grid(env, 0);
    cJSON* cols = cJSON_GetObjectItem(g, "cols");
    CHECK_EQ(cJSON_GetArraySize(cols), 3);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(cols, 0), "title")), "项");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(cols, 1), "num")));
    cJSON* rows = cJSON_GetObjectItem(g, "rows");
    CHECK_EQ(cJSON_GetArraySize(rows), 2);
    cJSON* r0 = cJSON_GetArrayItem(rows, 0);
    CHECK_EQ(cJSON_GetArraySize(r0), 3);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(r0, 0), "type")), "label");
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(r0, 1), "text")), "24");
    cJSON_Delete(env);
}

TEST_CASE("table 无 cols：不落 cols 键（solver 自动推断）") {
    cJSON* env = Compile("<card><table><tr><td>亮度</td><td><slider bind=\"display.brightness\"/></td></tr></table></card>");
    cJSON* g = Grid(env, 0);
    CHECK(cJSON_GetObjectItem(g, "cols") == nullptr);
    cJSON* cell1 = cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(g, "rows"), 0), 1);
    CHECK_EQ(std::string(Str(cell1, "type")), "slider");  // td 内恰一个叶子
    cJSON_Delete(env);
}

TEST_CASE("td 属性并入叶子、叶子自身优先；多叶子取首个+note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><table><tr>"
                         "<td tone=\"dim\" mono><label tone=\"ok\">x</label></td>"
                         "<td><label>a</label><label>b</label></td>"
                         "</tr></table></card>", &notes);
    cJSON* rows = cJSON_GetObjectItem(Grid(env, 0), "rows");
    cJSON* c0 = cJSON_GetArrayItem(cJSON_GetArrayItem(rows, 0), 0);
    CHECK_EQ(std::string(Str(c0, "tone")), "ok");  // 叶子自身优先
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(c0, "mono")));  // td 补进
    CHECK(HasNote(notes, "one leaf"));
    cJSON_Delete(env);
}

TEST_CASE("首行全 th → cols 表头 + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><table><tr><th>城市</th><th>温度</th></tr>"
                         "<tr><td>北京</td><td>24</td></tr></table></card>", &notes);
    cJSON* g = Grid(env, 0);
    cJSON* cols = cJSON_GetObjectItem(g, "cols");
    CHECK_EQ(cJSON_GetArraySize(cols), 2);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(cols, 0), "title")), "城市");
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(g, "rows")), 1);
    CHECK(HasNote(notes, "cols header"));
    cJSON_Delete(env);
}

TEST_CASE("list：bind/max/empty + 模板叶子 + {item.FIELD}") {
    cJSON* env = Compile("<card><list bind=\"tracks\" max=\"8\" empty=\"暂无\">"
                         "<button variant=\"ghost\" tap=\"report:'选了{item.title}'\">{item.title}</button>"
                         "</list></card>");
    cJSON* g = Grid(env, 0);
    CHECK_EQ(std::string(Str(g, "bind_rows")), "tracks");
    CHECK_EQ(Num(g, "max"), 8);
    CHECK_EQ(std::string(Str(g, "empty")), "暂无");
    cJSON* item = cJSON_GetObjectItem(g, "item");
    CHECK_EQ(cJSON_GetArraySize(item), 1);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(item, 0), "text")), "{item.title}");
    cJSON_Delete(env);
}

TEST_CASE("list 缺 bind：退化 cells grid + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><list><label>a</label></list></card>", &notes);
    CHECK(cJSON_GetObjectItem(Grid(env, 0), "cells") != nullptr);
    CHECK(HasNote(notes, "without bind"));
    cJSON_Delete(env);
}

TEST_CASE("块级 divider 与 grid 内 divider") {
    cJSON* env = Compile("<card><grid><label>a</label></grid><divider/><grid><label>b</label></grid></card>");
    // 块级 divider 单独成 cells 块（夹在两个 grid 之间）
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 3);
    CHECK_EQ(std::string(Str(Cell(env, 1, 0), "type")), "divider");
    cJSON_Delete(env);
    env = Compile("<card><grid><label>a</label><divider/><label>b</label></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "type")), "divider");
    cJSON_Delete(env);
}

TEST_CASE("grid fill 属性；grid 级事件剥除+note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid fill=\"card2\" tap=\"close\"><label>x</label></grid></card>", &notes);
    CHECK_EQ(std::string(Str(Grid(env, 0), "fill")), "card2");
    CHECK(cJSON_GetObjectItem(Grid(env, 0), "on_click") == nullptr);
    CHECK(HasNote(notes, "grid-level"));
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// <data> 线格式
// ---------------------------------------------------------------------------

TEST_CASE("data：记录行重复成数组、标量、数值转型") {
    cJSON* env = Compile("<card><list bind=\"tracks\"><button>{item.title}</button></list>"
                         "<data><tracks title=\"七里香\" idx=\"0\"/><tracks title=\"花海\" idx=\"1\"/>"
                         "<temp>24</temp><city>北京</city></data></card>");
    cJSON* data = cJSON_GetObjectItem(env, "data");
    cJSON* tracks = cJSON_GetObjectItem(data, "tracks");
    CHECK_EQ(cJSON_GetArraySize(tracks), 2);
    cJSON* t0 = cJSON_GetArrayItem(tracks, 0);
    CHECK_EQ(std::string(Str(t0, "title")), "七里香");
    CHECK(cJSON_IsNumber(cJSON_GetObjectItem(t0, "idx")));
    CHECK_EQ(Num(data, "temp"), 24);
    CHECK_EQ(std::string(Str(data, "city")), "北京");
    cJSON_Delete(env);
}

TEST_CASE("data：同名标量二次出现升级成数组") {
    cJSON* env = Compile("<card><grid><label>x</label></grid>"
                         "<data><tag>红</tag><tag>蓝</tag></data></card>");
    cJSON* arr = cJSON_GetObjectItem(cJSON_GetObjectItem(env, "data"), "tag");
    CHECK(cJSON_IsArray(arr));
    CHECK_EQ(cJSON_GetArraySize(arr), 2);
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// HTML 子集容错（§2.5）
// ---------------------------------------------------------------------------

TEST_CASE("div/section→grid；h1/h2/h3/p/span→label 角色阶梯") {
    cJSON* env = Compile("<card><div><h1>标题</h1><h2>小节</h2><h3>子头</h3><p>正文</p><span>行内</span></div></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "role")), "title");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "role")), "section");
    CHECK_EQ(std::string(Str(Cell(env, 0, 2), "role")), "heading");
    CHECK(cJSON_GetObjectItem(Cell(env, 0, 3), "role") == nullptr);
    CHECK_EQ(std::string(Str(Cell(env, 0, 4), "text")), "行内");
    cJSON_Delete(env);
}

TEST_CASE("h1 显式 role 属性优先于别名预置") {
    cJSON* env = Compile("<card><grid><h1 role=\"heading\">x</h1></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "role")), "heading");
    cJSON_Delete(env);
}

TEST_CASE("hr→divider；ul/li→单列 table") {
    cJSON* env = Compile("<card><grid><label>a</label><hr></grid><ul><li>第一项</li><li>第二项</li></ul></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "type")), "divider");
    cJSON* rows = cJSON_GetObjectItem(Grid(env, 1), "rows");
    CHECK_EQ(cJSON_GetArraySize(rows), 2);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetArrayItem(rows, 0)), 1);
    CHECK_EQ(std::string(Str(cJSON_GetArrayItem(cJSON_GetArrayItem(rows, 1), 0), "text")), "第二项");
    cJSON_Delete(env);
}

TEST_CASE("input range→slider / checkbox→switch / 其它跳过+note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid>"
                         "<input type=\"range\" bind=\"audio.volume\" min=\"0\" max=\"100\"/>"
                         "<input type=\"checkbox\" checked bind=\"ui.theme\"/>"
                         "<input type=\"text\"/>"
                         "</grid></card>", &notes);
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "type")), "slider");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "bind")), "audio.volume");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "type")), "switch");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(Cell(env, 0, 1), "checked")));
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(Grid(env, 0), "cells")), 2);
    CHECK(HasNote(notes, "only range/checkbox"));
    cJSON_Delete(env);
}

TEST_CASE("style/class/onclick 未知属性剥除，聚合一条 note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid style=\"color:red\"><label class=\"big\" onclick=\"js()\">x</label></grid></card>", &notes);
    cJSON* l = Cell(env, 0, 0);
    CHECK(cJSON_GetObjectItem(l, "class") == nullptr);
    CHECK(cJSON_GetObjectItem(l, "onclick") == nullptr);
    int strip_notes = 0;
    for (const auto& n : notes)
        if (n.find("stripped unknown attrs") != std::string::npos) strip_notes++;
    CHECK_EQ(strip_notes, 1);
    CHECK(HasNote(notes, "style"));
    CHECK(HasNote(notes, "class"));
    cJSON_Delete(env);
}

TEST_CASE("未知标签剥壳保留子树 / 空未知跳过，各留 note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><widget><label>藏在里面</label></widget><ghost/></grid></card>", &notes);
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "藏在里面");
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(Grid(env, 0), "cells")), 1);
    CHECK(HasNote(notes, "<widget>"));
    CHECK(HasNote(notes, "<ghost>"));
    cJSON_Delete(env);
}

TEST_CASE("grid 套 grid：拍平 + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><label>a</label><div><label>b</label></div></grid></card>", &notes);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 1);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(Grid(env, 0), "cells")), 2);
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "text")), "b");
    CHECK(HasNote(notes, "flattened"));
    cJSON_Delete(env);
}

TEST_CASE("grid 里嵌 table：提升成兄弟块 + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><label>头</label>"
                         "<table><tr><td>k</td><td>v</td></tr></table></grid></card>", &notes);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 2);
    CHECK(cJSON_GetObjectItem(Grid(env, 1), "rows") != nullptr);
    CHECK(HasNote(notes, "promoted"));
    cJSON_Delete(env);
}

TEST_CASE("散叶子聚成 cells grid；label 里 b/em 拍平、br 换行") {
    cJSON* env = Compile("<card><h1>标题</h1><p>第一行<br/>第<b>二</b>行</p><grid><label>块</label></grid></card>");
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 2);
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "role")), "title");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "text")), "第一行\n第二行");
    cJSON_Delete(env);
}

TEST_CASE("卡级/grid 级裸文本 → label") {
    cJSON* env = Compile("<card>直接说话<grid>格里说话<label>正经叶子</label></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "直接说话");
    CHECK_EQ(std::string(Str(Cell(env, 1, 0), "text")), "格里说话");
    CHECK_EQ(std::string(Str(Cell(env, 1, 1), "text")), "正经叶子");
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// 转义与宽容解析（§2.6）
// ---------------------------------------------------------------------------

TEST_CASE("标准实体 + 数字实体 + 裸 & 字面量") {
    cJSON* env = Compile("<card><grid><label>A&amp;B &lt;tag&gt; &#20320;&#x597d; M&M</label>"
                         "<qrcode text=\"https://x.y/?a=1&amp;b=2&c=3\"/></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "A&B <tag> 你好 M&M");
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "text")), "https://x.y/?a=1&b=2&c=3");
    cJSON_Delete(env);
}

TEST_CASE("未闭合标签全部自动闭合 + note") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><label>没关的", &notes);
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "没关的");
    CHECK(HasNote(notes, "auto-closed"));
    cJSON_Delete(env);
}

TEST_CASE("截断在开标签中间：残缺片段丢弃、前序照编译") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid><label>完整的</label><slider bi", &notes);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(Grid(env, 0), "cells")), 1);
    CHECK(HasNote(notes, "mid-tag"));
    cJSON_Delete(env);
}

TEST_CASE("截断在属性引号里：整元素丢弃") {
    cJSON* env = Compile("<card><grid><label>好的</label><button tap=\"report:半截");
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(Grid(env, 0), "cells")), 1);
    cJSON_Delete(env);
}

TEST_CASE("错配闭标签：HTML 弹栈恢复 + 孤儿闭标签忽略") {
    cJSON* env = Compile("<card><grid><label>a</grid></label><grid><label>b</label></grid></card>");
    // </grid> 把没关的 label 一起弹掉；后面的 </label> 是孤儿被忽略
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 2);
    CHECK_EQ(std::string(Str(Cell(env, 1, 0), "text")), "b");
    cJSON_Delete(env);
}

TEST_CASE("void 元素不带斜杠不吃兄弟") {
    cJSON* env = Compile("<card><grid><icon name=\"wifi\"><label>兄弟</label></grid></card>");
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(Grid(env, 0), "cells")), 2);
    CHECK_EQ(std::string(Str(Cell(env, 0, 1), "text")), "兄弟");
    cJSON_Delete(env);
}

TEST_CASE("注释/DOCTYPE/PI 跳过；'a < b' 字面小于号存活") {
    cJSON* env = Compile("<?xml version=\"1.0\"?><!DOCTYPE card><card><!-- 备注 --><grid><label>a < b</label></grid></card>");
    CHECK_EQ(std::string(Str(Cell(env, 0, 0), "text")), "a < b");
    cJSON_Delete(env);
}

TEST_CASE("空输入/无元素输入 → 唯一失败面，err 带引导") {
    cJSON* env = nullptr;
    std::string err;
    CHECK(!XmlCompile("", 0, &env, nullptr, err));
    CHECK(env == nullptr);
    CHECK(err.find("card") != std::string::npos);
    CHECK(!XmlCompile("就是一句话没有标签", strlen("就是一句话没有标签"), &env, nullptr, err));
    cJSON_Delete(env);
}

TEST_CASE("空 grid/空 table 丢块 + note；空卡编译成功交给 Validate") {
    std::vector<std::string> notes;
    cJSON* env = Compile("<card><grid></grid><table></table></card>", &notes);
    CHECK_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")), 0);
    CHECK(HasNote(notes, "empty <grid>"));
    CHECK(HasNote(notes, "empty <table>"));
    cJSON_Delete(env);
}

TEST_CASE("病态深嵌套不崩（深度钳制）") {
    std::string deep = "<card>";
    for (int i = 0; i < 64; i++) deep += "<div>";
    deep += "<label>底</label>";
    cJSON* env = Compile(deep);  // 不闭合，靠自动闭合；只要不崩且能出叶子就行
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(env, "root")) >= 1);
    cJSON_Delete(env);
}

// ---------------------------------------------------------------------------
// 前缀不变量（流式预览复用同一函数，§2.6 / CARD_V2 §7.1 P1 同款口径）
// ---------------------------------------------------------------------------

TEST_CASE("前缀切分：任意字节切分点不崩、全量前缀 ≡ 一次性编译") {
    const char* samples[] = {
        "<card display=\"overlay\" ttl=\"30s\"><grid><icon name=\"volume\"/>"
        "<slider bind=\"audio.volume\"/><label role=\"value\" bind=\"audio.volume\" fmt=\"%d%%\"/></grid>"
        "<table cols=\"项,值:num\"><tr><td>电量</td><td>88</td></tr></table>"
        "<list bind=\"tracks\" max=\"8\"><button tap=\"report:'选{item.title}'\">{item.title}</button></list>"
        "<data><tracks title=\"七里香\"/><tracks title=\"花海\"/></data></card>",
        "<card><h1>标题 &amp; 附注</h1><p>正文</p><divider/><grid fill=\"card2\">"
        "<chart bind=\"battery.level\" points=\"60\"/></grid></card>",
    };
    for (const char* xml : samples) {
        size_t n = strlen(xml);
        cJSON* full = nullptr;
        std::string err;
        CHECK(XmlCompile(xml, n, &full, nullptr, err));
        std::string full_s = CompactJson(full);
        // 任意切分点：要么 ok 要么 err（不崩）；ok 时信封至少有 root 数组
        for (size_t cut = 0; cut <= n; cut++) {
            cJSON* part = nullptr;
            std::string perr;
            if (XmlCompile(xml, cut, &part, nullptr, perr)) {
                CHECK(cJSON_IsArray(cJSON_GetObjectItem(part, "root")));
                cJSON_Delete(part);
            }
        }
        // 全量前缀 ≡ 一次性
        cJSON* again = nullptr;
        CHECK(XmlCompile(xml, n, &again, nullptr, err));
        CHECK_EQ(full_s, CompactJson(again));
        cJSON_Delete(again);
        cJSON_Delete(full);
    }
}

TEST_CASE("前缀单调性：已冻结的前序 grid 的编译产物逐帧字节稳定") {
    // 模拟流式：每个前缀切分点编译一次，对「非最后一个」grid 的紧凑 JSON 做一致性检查
    // ——这是 preview_sig 整块签名不抖动（前序块冻结，§4.1）的编译器侧前提。
    const char* xml =
        "<card><grid><label role=\"eyebrow\">状态</label></grid>"
        "<table cols=\",:num\"><tr><td>电量</td><td>88</td></tr><tr><td>信号</td><td>-52</td></tr></table>"
        "<grid><button variant=\"primary\" tap=\"close\">好</button></grid></card>";
    size_t n = strlen(xml);
    std::vector<std::string> frozen;  // frozen[i] = grid i 首次冻结时的 JSON
    for (size_t cut = 1; cut <= n; cut++) {
        cJSON* env = nullptr;
        std::string err;
        if (!XmlCompile(xml, cut, &env, nullptr, err)) continue;
        cJSON* root = cJSON_GetObjectItem(env, "root");
        int ngrid = cJSON_GetArraySize(root);
        for (int i = 0; i + 1 < ngrid; i++) {  // 最后一个是生长边，不检查
            std::string s = CompactJson(cJSON_GetArrayItem(root, i));
            if (static_cast<int>(frozen.size()) <= i) frozen.push_back(s);
            else CHECK_EQ(frozen[static_cast<size_t>(i)], s);
        }
        cJSON_Delete(env);
    }
    CHECK_EQ(static_cast<int>(frozen.size()), 2);
}

int main() { return RUN_ALL_TESTS(); }
