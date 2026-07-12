#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Inline markdown scanner: one pass, no nesting. Precedence: `code` >
// [text](url) > **bold** > ~~strike~~ > *italic*; an unterminated marker
// renders as literal text. `_underscore_` emphasis is deliberately not
// recognized (snake_case identifiers would misfire). No LVGL dependency.

namespace lvmd {

enum SpanFlags : uint8_t {
    kSpanBold = 1,
    kSpanCode = 2,
    kSpanLink = 4,  // text kept, URL dropped
    kSpanItalic = 8,
    kSpanStrike = 16,
};

struct Span {
    std::string text;
    uint8_t flags = 0;
};

// Scans a single line (input must not contain '\n').
std::vector<Span> ParseInline(std::string_view line);

}  // namespace lvmd
