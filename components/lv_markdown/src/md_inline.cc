#include "lv_markdown/md_inline.h"

#include <cstring>

namespace lvmd {

std::vector<Span> ParseInline(std::string_view line) {
    std::vector<Span> out;
    std::string plain;
    auto flush_plain = [&] {
        if (!plain.empty()) {
            out.push_back(Span{std::move(plain), 0});
            plain.clear();
        }
    };

    auto match_link = [&line](size_t lb, size_t* end, std::string* text) {
        // lb points at '['; returns the bracket text of [text](url)
        size_t rb = line.find(']', lb + 1);
        if (rb == std::string_view::npos || rb + 1 >= line.size() || line[rb + 1] != '(') return false;
        size_t rp = line.find(')', rb + 2);
        if (rp == std::string_view::npos) return false;
        *text = std::string(line.substr(lb + 1, rb - lb - 1));
        *end = rp + 1;  // URL dropped
        return true;
    };

    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        char c = line[i];
        if (c == '\\' && i + 1 < n && std::strchr("\\`*_~[]()#!|>-+.", line[i + 1]) != nullptr) {
            plain += line[i + 1];  // backslash escape: emit the punctuation literally
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t close = line.find('`', i + 1);
            if (close != std::string_view::npos) {
                flush_plain();
                out.push_back(Span{std::string(line.substr(i + 1, close - i - 1)), kSpanCode});
                i = close + 1;
                continue;
            }
        } else if (c == '[') {
            size_t end = 0;
            std::string text;
            if (match_link(i, &end, &text)) {
                flush_plain();
                out.push_back(Span{std::move(text), kSpanLink});
                i = end;
                continue;
            }
        } else if (c == '!' && i + 1 < n && line[i + 1] == '[') {
            size_t end = 0;
            std::string text;
            if (match_link(i + 1, &end, &text)) {  // image: alt text only, no leading '!'
                flush_plain();
                out.push_back(Span{std::move(text), kSpanLink});
                i = end;
                continue;
            }
        } else if (c == '*' && i + 1 < n && line[i + 1] == '*') {
            size_t close = line.find("**", i + 2);
            if (close != std::string_view::npos && close > i + 2) {
                flush_plain();
                out.push_back(Span{std::string(line.substr(i + 2, close - i - 2)), kSpanBold});
                i = close + 2;
                continue;
            }
        } else if (c == '~' && i + 1 < n && line[i + 1] == '~') {
            size_t close = line.find("~~", i + 2);
            if (close != std::string_view::npos && close > i + 2) {
                flush_plain();
                out.push_back(Span{std::string(line.substr(i + 2, close - i - 2)), kSpanStrike});
                i = close + 2;
                continue;
            }
        } else if (c == '*' && i + 1 < n && line[i + 1] != ' ' && line[i + 1] != '\t') {
            // single-* emphasis, flanking-lite: opener not followed by space,
            // closer not preceded by space (so "a * b * c" stays literal)
            size_t j = i + 2;
            while (j < n && !(line[j] == '*' && line[j - 1] != ' ' && line[j - 1] != '\t')) j++;
            if (j < n) {
                flush_plain();
                out.push_back(Span{std::string(line.substr(i + 1, j - i - 1)), kSpanItalic});
                i = j + 1;
                continue;
            }
        }
        plain += c;
        i++;
    }
    flush_plain();
    return out;
}

}  // namespace lvmd
