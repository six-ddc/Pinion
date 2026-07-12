// Host unit tests for lv_markdown's LVGL-free core (md_parser + md_inline).
// Build via the sim project: cmake --build sim/build --target md_test && ./sim/build/md_test

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lv_markdown/md_inline.h"
#include "lv_markdown/md_parser.h"

using lvmd::Block;
using lvmd::BlockType;
using lvmd::ParseInline;
using lvmd::Span;
using lvmd::StreamParser;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

std::vector<Block> ParseAll(const std::string& src, size_t step) {
    StreamParser p;
    std::vector<Block> out;
    for (size_t i = 0; i < src.size(); i += step) {
        p.Feed(std::string_view(src).substr(i, step));
        for (Block& b : p.TakeClosed()) out.push_back(std::move(b));
    }
    for (Block& b : p.Finish()) out.push_back(std::move(b));
    return out;
}

bool SameBlocks(const std::vector<Block>& a, const std::vector<Block>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].type != b[i].type || a[i].level != b[i].level || a[i].text != b[i].text ||
            a[i].info != b[i].info) {
            return false;
        }
    }
    return true;
}

void TestBlocks() {
    const std::string src =
        "# Title\n"
        "para line1\n"
        "para line2\n"
        "\n"
        "## Sub **bold**\n"
        "- item one\n"
        "* item two\n"
        "12. numbered\n"
        "> quote a\n"
        "> quote b\n"
        "---\n"
        "```c\n"
        "#include <stdio.h>\n"
        "\n"
        "int main() {}\n"
        "```\n"
        "tail para";
    auto blocks = ParseAll(src, src.size());
    CHECK(blocks.size() == 10);
    CHECK(blocks[0].type == BlockType::kHeading && blocks[0].level == 1 && blocks[0].text == "Title");
    CHECK(blocks[1].type == BlockType::kParagraph && blocks[1].text == "para line1\npara line2");
    CHECK(blocks[2].type == BlockType::kHeading && blocks[2].level == 2 && blocks[2].text == "Sub **bold**");
    CHECK(blocks[3].type == BlockType::kBullet && blocks[3].text == "item one");
    CHECK(blocks[4].type == BlockType::kBullet && blocks[4].text == "item two");
    CHECK(blocks[5].type == BlockType::kOrdered && blocks[5].level == 12 && blocks[5].text == "numbered");
    CHECK(blocks[6].type == BlockType::kQuote && blocks[6].text == "quote a\nquote b");
    CHECK(blocks[7].type == BlockType::kRule);
    CHECK(blocks[8].type == BlockType::kFence && blocks[8].info == "c");
    CHECK(blocks[8].text == "#include <stdio.h>\n\nint main() {}");
    CHECK(blocks[9].type == BlockType::kParagraph && blocks[9].text == "tail para");  // via Finish()
    auto blocks2 = ParseAll(src, 7);
    CHECK(SameBlocks(blocks, blocks2));
}

void TestFuzzSplit() {
    const std::string src =
        "#### four hashes stay literal\n"
        "#nospace stays paragraph\n"
        "\n"
        "### H3 中文标题\n"
        "  - indented item 中文\n"
        "1. one\n"
        "```\n"
        "code 中文 ` ** [x](y)\n"
        "```\n"
        "> q\n"
        "not a quote continuation\n"
        "**unclosed bold\n"
        "---\n"
        "end";
    auto ref = ParseAll(src, src.size());
    for (size_t step = 1; step <= 5; step++) {
        auto got = ParseAll(src, step);
        CHECK(SameBlocks(ref, got));
    }
    CHECK(ref.size() == 10);
    CHECK(ref[0].type == BlockType::kHeading && ref[0].level == 4);  // H4 supported now
    CHECK(ref[1].type == BlockType::kParagraph && ref[1].text == "#nospace stays paragraph");
    CHECK(ref[2].type == BlockType::kHeading && ref[2].level == 3);
    CHECK(ref[3].type == BlockType::kBullet && ref[3].text == "indented item 中文");
    CHECK(ref[4].type == BlockType::kOrdered && ref[4].level == 1);
    CHECK(ref[5].type == BlockType::kFence && ref[5].text == "code 中文 ` ** [x](y)");
    CHECK(ref[6].type == BlockType::kQuote && ref[6].text == "q");
    // the non-'>' line closed the quote and became its own paragraph merged
    // with the unclosed-bold line (no blank line between them)
    CHECK(ref[7].type == BlockType::kParagraph &&
          ref[7].text == "not a quote continuation\n**unclosed bold");
    CHECK(ref[8].type == BlockType::kRule);
    CHECK(ref[9].type == BlockType::kParagraph && ref[9].text == "end");
}

void TestUnclosedFence() {
    StreamParser p;
    p.Feed("```py\nx = 1\n");
    CHECK(p.TakeClosed().empty());
    const Block* open = p.Open();
    CHECK(open != nullptr && open->type == BlockType::kFence && open->info == "py");
    CHECK(open->text == "x = 1");
    auto rest = p.Finish();
    CHECK(rest.size() == 1 && rest[0].type == BlockType::kFence && rest[0].text == "x = 1");
}

void TestTentativeOpen() {
    StreamParser p;
    p.Feed("# He");  // half a heading, no newline yet
    const Block* open = p.Open();
    CHECK(open != nullptr && open->type == BlockType::kHeading && open->level == 1 && open->text == "He");
    uint32_t rev = p.OpenRevision();
    p.Feed("llo");
    open = p.Open();
    CHECK(open != nullptr && open->text == "Hello");
    CHECK(p.OpenRevision() != rev);
    p.Feed("\n");
    auto closed = p.TakeClosed();
    CHECK(closed.size() == 1 && closed[0].type == BlockType::kHeading && closed[0].text == "Hello");
    CHECK(p.Open() == nullptr);

    StreamParser q;
    q.Feed("para\nmore");  // open paragraph + partial continuation
    const Block* o2 = q.Open();
    CHECK(o2 != nullptr && o2->type == BlockType::kParagraph && o2->text == "para\nmore");
}

void TestExtendedSyntax() {
    const std::string src =
        "#### 四级\n"
        "###### 六级\n"
        "####### 七个井号是段落\n"
        "- [ ] 待办一\n"
        "- [x] 已完成\n"
        "- [x]nospace 是普通列表项\n"
        "| 名称 | 值 |\n"
        "|------|----|\n"
        "| a | **1** |\n"
        "| b | 2 |\n"
        "not a table row\n"
        "| 假表头 |\n"
        "没有分隔行\n";
    auto ref = ParseAll(src, src.size());
    for (size_t step = 1; step <= 3; step++) CHECK(SameBlocks(ref, ParseAll(src, step)));
    CHECK(ref.size() == 10);
    CHECK(ref[0].type == BlockType::kHeading && ref[0].level == 4);
    CHECK(ref[1].type == BlockType::kHeading && ref[1].level == 6);
    CHECK(ref[2].type == BlockType::kParagraph && ref[2].text == "####### 七个井号是段落");
    CHECK(ref[3].type == BlockType::kTask && ref[3].level == 0 && ref[3].text == "待办一");
    CHECK(ref[4].type == BlockType::kTask && ref[4].level == 1 && ref[4].text == "已完成");
    CHECK(ref[5].type == BlockType::kBullet && ref[5].text == "[x]nospace 是普通列表项");
    CHECK(ref[6].type == BlockType::kTableRow && ref[6].info == std::string("名称") + lvmd::kCellSep + "值");
    CHECK(ref[6].text == std::string("a") + lvmd::kCellSep + "**1**");
    CHECK(ref[7].type == BlockType::kTableRow && ref[7].text == std::string("b") + lvmd::kCellSep + "2");
    CHECK(ref[8].type == BlockType::kParagraph && ref[8].text == "not a table row");
    // header candidate without a separator line reverts to a paragraph
    CHECK(ref[9].type == BlockType::kParagraph && ref[9].text == "| 假表头 |\n没有分隔行");
}

void TestInline() {
    auto spans = ParseInline("a **b** `c` [d](http://e) f");
    CHECK(spans.size() == 7);
    CHECK(spans[0].flags == 0 && spans[0].text == "a ");
    CHECK(spans[1].flags == lvmd::kSpanBold && spans[1].text == "b");
    CHECK(spans[2].flags == 0 && spans[2].text == " ");
    CHECK(spans[3].flags == lvmd::kSpanCode && spans[3].text == "c");
    CHECK(spans[5].flags == lvmd::kSpanLink && spans[5].text == "d");
    CHECK(spans[6].flags == 0 && spans[6].text == " f");

    // unclosed markers stay literal (and merge into a single plain span)
    auto lit = ParseInline("**open `tick [brk](x");
    CHECK(lit.size() == 1);
    std::string joined;
    for (auto& s : lit) joined += s.text;
    CHECK(joined == "**open `tick [brk](x");

    // backtick protects inner markers and '#'
    auto code = ParseInline("see `#include <a.h>` and `**not bold**`");
    CHECK(code.size() == 4);
    CHECK(code[1].flags == lvmd::kSpanCode && code[1].text == "#include <a.h>");
    CHECK(code[3].flags == lvmd::kSpanCode && code[3].text == "**not bold**");

    // bold containing '#'
    auto bold = ParseInline("**C#** rocks");
    CHECK(bold.size() == 2 && bold[0].flags == lvmd::kSpanBold && bold[0].text == "C#");

    // empty bold "****" stays literal
    auto eb = ParseInline("****");
    std::string ebj;
    for (auto& s : eb) ebj += s.text;
    CHECK(ebj == "****");

    // single-* italic with flanking rules; ~~strike~~
    auto it = ParseInline("a *em* b");
    CHECK(it.size() == 3 && it[1].flags == lvmd::kSpanItalic && it[1].text == "em");
    auto sp = ParseInline("a * b * c");  // space-flanked stars stay literal
    std::string spj;
    for (auto& s : sp) spj += s.text;
    CHECK(sp.size() == 1 && spj == "a * b * c");
    auto st = ParseInline("~~gone~~ kept");
    CHECK(st.size() == 2 && st[0].flags == lvmd::kSpanStrike && st[0].text == "gone");
    auto mix = ParseInline("**b** *i* ~~s~~");
    CHECK(mix.size() == 5 && mix[0].flags == lvmd::kSpanBold && mix[2].flags == lvmd::kSpanItalic &&
          mix[4].flags == lvmd::kSpanStrike);

    // backslash escapes and image alt text
    auto esc = ParseInline("\\*not italic\\* and \\`raw\\`");
    CHECK(esc.size() == 1 && esc[0].text == "*not italic* and `raw`");
    auto img = ParseInline("see ![示意图](http://x/y.png) here");
    CHECK(img.size() == 3 && img[1].flags == lvmd::kSpanLink && img[1].text == "示意图");
}

}  // namespace

int main() {
    TestBlocks();
    TestFuzzSplit();
    TestUnclosedFence();
    TestTentativeOpen();
    TestExtendedSyntax();
    TestInline();
    if (g_failures == 0) {
        std::printf("md_test: all passed\n");
        return 0;
    }
    std::printf("md_test: %d failure(s)\n", g_failures);
    return 1;
}
