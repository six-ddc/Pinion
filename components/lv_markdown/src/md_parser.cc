#include "lv_markdown/md_parser.h"

namespace lvmd {
namespace {

bool IsBlank(std::string_view s) {
    for (char c : s) {
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

std::string_view TrimRight(std::string_view s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    return s.substr(0, end);
}

std::string_view TrimLeft(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t')) begin++;
    return s.substr(begin);
}

// ```info -- up to 3 leading spaces, >=3 backticks, rest is the info string.
bool MatchFenceOpen(std::string_view line, std::string* info) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') i++;
    size_t ticks = 0;
    while (i + ticks < line.size() && line[i + ticks] == '`') ticks++;
    if (ticks < 3) return false;
    std::string_view rest = TrimRight(TrimLeft(line.substr(i + ticks)));
    if (rest.find('`') != std::string_view::npos) return false;  // ``x`` inline, not a fence
    *info = std::string(rest);
    return true;
}

// Closing fence: nothing but >=3 backticks (and surrounding whitespace).
bool IsFenceClose(std::string_view line) {
    std::string_view t = TrimRight(TrimLeft(line));
    if (t.size() < 3) return false;
    for (char c : t) {
        if (c != '`') return false;
    }
    return true;
}

// #{1,6} + space. 7+ hashes or a missing space fall through to paragraph.
bool MatchHeading(std::string_view line, int* level, std::string_view* text) {
    std::string_view t = line;
    size_t sp = 0;
    while (sp < t.size() && sp < 3 && t[sp] == ' ') sp++;
    t = t.substr(sp);
    size_t hashes = 0;
    while (hashes < t.size() && t[hashes] == '#') hashes++;
    if (hashes < 1 || hashes > 6) return false;
    if (hashes >= t.size() || (t[hashes] != ' ' && t[hashes] != '\t')) return false;
    *level = static_cast<int>(hashes);
    *text = TrimRight(TrimLeft(t.substr(hashes)));
    return true;
}

// A line of nothing but >=3 of the same rule marker (--- / *** / ___).
bool IsRule(std::string_view line) {
    std::string_view t = TrimRight(TrimLeft(line));
    if (t.size() < 3) return false;
    char m = t[0];
    if (m != '-' && m != '*' && m != '_') return false;
    for (char c : t) {
        if (c != m) return false;
    }
    return true;
}

// [-*+] + space, any indent (nested lists deliberately flatten to one level).
bool MatchBullet(std::string_view line, std::string_view* text) {
    std::string_view t = TrimLeft(line);
    if (t.size() < 2) return false;
    if (t[0] != '-' && t[0] != '*' && t[0] != '+') return false;
    if (t[1] != ' ' && t[1] != '\t') return false;
    *text = TrimRight(TrimLeft(t.substr(2)));
    return !text->empty();
}

// 1-3 digits + '.' + space, any indent.
bool MatchOrdered(std::string_view line, int* number, std::string_view* text) {
    std::string_view t = TrimLeft(line);
    size_t digits = 0;
    while (digits < t.size() && digits < 3 && t[digits] >= '0' && t[digits] <= '9') digits++;
    if (digits == 0 || digits + 1 >= t.size()) return false;
    if (t[digits] != '.') return false;
    if (t[digits + 1] != ' ' && t[digits + 1] != '\t') return false;
    int n = 0;
    for (size_t i = 0; i < digits; i++) n = n * 10 + (t[i] - '0');
    *number = n;
    *text = TrimRight(TrimLeft(t.substr(digits + 2)));
    return !text->empty();
}

// "[ ] " / "[x] " task prefix on an already-stripped bullet body.
bool MatchTask(std::string_view text, bool* checked, std::string_view* rest) {
    if (text.size() < 3 || text[0] != '[' || text[2] != ']') return false;
    char mark = text[1];
    if (mark != ' ' && mark != 'x' && mark != 'X') return false;
    std::string_view r = text.substr(3);
    if (!r.empty() && r[0] != ' ' && r[0] != '\t') return false;  // "[x]foo" stays a bullet
    *checked = mark != ' ';
    *rest = TrimRight(TrimLeft(r));
    return true;
}

// '>' with up to 3 leading spaces; strips "> " / ">".
bool MatchQuote(std::string_view line, std::string_view* text) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') i++;
    if (i >= line.size() || line[i] != '>') return false;
    std::string_view rest = line.substr(i + 1);
    if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
    *text = TrimRight(rest);
    return true;
}

void AppendLine(std::string* dst, std::string_view line) {
    if (!dst->empty()) *dst += '\n';
    dst->append(line);
}

// A table line: '|' after up to 3 leading spaces.
bool IsTableLine(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') i++;
    return i < line.size() && line[i] == '|';
}

// GFM header/body separator: only '|', '-', ':' and whitespace, >=1 dash.
bool IsTableSeparator(std::string_view line) {
    bool dash = false;
    for (char c : line) {
        if (c == '-') {
            dash = true;
        } else if (c != '|' && c != ':' && c != ' ' && c != '\t') {
            return false;
        }
    }
    return dash && IsTableLine(line);
}

// "| a | b |" -> "a<US>b" (outer pipes optional, cells trimmed).
std::string JoinTableCells(std::string_view line) {
    std::string_view t = TrimRight(TrimLeft(line));
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);
    std::string out;
    size_t start = 0;
    while (start <= t.size()) {
        size_t bar = t.find('|', start);
        size_t end = (bar == std::string_view::npos) ? t.size() : bar;
        std::string_view cell = TrimRight(TrimLeft(t.substr(start, end - start)));
        if (!out.empty()) out += kCellSep;
        out.append(cell);
        if (bar == std::string_view::npos) break;
        start = bar + 1;
    }
    return out;
}

}  // namespace

void StreamParser::Feed(std::string_view delta) {
    if (delta.empty()) return;
    partial_line_ += delta;
    size_t nl;
    while ((nl = partial_line_.find('\n')) != std::string::npos) {
        std::string_view line(partial_line_.data(), nl);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ProcessLine(line);
        partial_line_.erase(0, nl + 1);
    }
    RebuildOpenView();
    open_rev_++;
}

void StreamParser::ProcessLine(std::string_view line) {
    if (state_ == State::kFence) {
        if (IsFenceClose(line)) {
            CloseOpen();
        } else {
            AppendLine(&open_.text, line);
        }
        return;
    }

    if (state_ == State::kTablePending) {
        if (IsTableSeparator(line)) {
            table_headers_ = JoinTableCells(table_pending_line_);
            table_pending_line_.clear();
            state_ = State::kTable;
            return;
        }
        // No separator -> the stashed line was an ordinary paragraph line;
        // reopen it and let the current line classify normally below.
        open_ = Block{BlockType::kParagraph, 0, std::move(table_pending_line_), ""};
        has_open_ = true;
        state_ = State::kParagraph;
        table_pending_line_.clear();
    } else if (state_ == State::kTable) {
        if (IsTableLine(line)) {
            closed_.push_back(Block{BlockType::kTableRow, 0, JoinTableCells(line), table_headers_});
            return;
        }
        table_headers_.clear();
        state_ = State::kIdle;
    }

    if (IsTableLine(line)) {
        CloseOpen();
        table_pending_line_ = std::string(line);
        state_ = State::kTablePending;
        return;
    }

    std::string info;
    if (MatchFenceOpen(line, &info)) {
        CloseOpen();
        open_ = Block{BlockType::kFence, 0, "", std::move(info)};
        has_open_ = true;
        state_ = State::kFence;
        return;
    }
    if (IsBlank(line)) {
        CloseOpen();
        return;
    }

    int level = 0;
    std::string_view text;
    if (MatchHeading(line, &level, &text)) {
        CloseOpen();
        closed_.push_back(Block{BlockType::kHeading, level, std::string(text), ""});
        return;
    }
    if (IsRule(line)) {
        CloseOpen();
        closed_.push_back(Block{BlockType::kRule, 0, "", ""});
        return;
    }
    if (MatchBullet(line, &text)) {
        CloseOpen();
        bool checked = false;
        std::string_view rest;
        if (MatchTask(text, &checked, &rest)) {
            closed_.push_back(Block{BlockType::kTask, checked ? 1 : 0, std::string(rest), ""});
        } else {
            closed_.push_back(Block{BlockType::kBullet, 0, std::string(text), ""});
        }
        return;
    }
    int number = 0;
    if (MatchOrdered(line, &number, &text)) {
        CloseOpen();
        closed_.push_back(Block{BlockType::kOrdered, number, std::string(text), ""});
        return;
    }
    if (MatchQuote(line, &text)) {
        if (state_ == State::kQuote) {
            AppendLine(&open_.text, text);
        } else {
            CloseOpen();
            open_ = Block{BlockType::kQuote, 0, std::string(text), ""};
            has_open_ = true;
            state_ = State::kQuote;
        }
        return;
    }

    // Plain text: continue a paragraph (single '\n' kept as a hard break --
    // chat replies read better that way) or start one.
    if (state_ == State::kParagraph) {
        AppendLine(&open_.text, line);
    } else {
        CloseOpen();
        open_ = Block{BlockType::kParagraph, 0, std::string(line), ""};
        has_open_ = true;
        state_ = State::kParagraph;
    }
}

void StreamParser::CloseOpen() {
    if (has_open_) {
        closed_.push_back(std::move(open_));
        open_ = Block{};
        has_open_ = false;
    }
    state_ = State::kIdle;
}

// Tentative view of the tail for live display. Closed blocks are only ever
// produced by ProcessLine on real newlines; this is display-only and may
// reclassify as more bytes arrive (the renderer rebuilds the tail widget on
// type change).
void StreamParser::RebuildOpenView() {
    has_open_view_ = false;
    if (state_ == State::kTablePending || state_ == State::kTable) return;  // rows pop in whole
    if (state_ == State::kFence) {
        open_view_ = open_;
        if (!partial_line_.empty()) AppendLine(&open_view_.text, partial_line_);
        has_open_view_ = true;
        return;
    }
    if (has_open_) {
        open_view_ = open_;
        if (!partial_line_.empty()) {
            std::string_view text;
            if (state_ == State::kQuote) {
                // Only extend the quote if the partial line still looks like
                // one; otherwise let it appear once its newline decides.
                if (MatchQuote(partial_line_, &text)) AppendLine(&open_view_.text, text);
            } else {
                AppendLine(&open_view_.text, partial_line_);
            }
        }
        has_open_view_ = true;
        return;
    }
    if (partial_line_.empty() || IsBlank(partial_line_)) return;
    if (IsTableLine(partial_line_)) return;  // suppress raw pipes until the line completes

    // Nothing committed: classify the partial line as if it were complete so a
    // half-received heading/list item/quote already renders in style.
    std::string info;
    int level = 0;
    std::string_view text;
    if (MatchFenceOpen(partial_line_, &info)) {
        open_view_ = Block{BlockType::kFence, 0, "", std::move(info)};
    } else if (MatchHeading(partial_line_, &level, &text)) {
        open_view_ = Block{BlockType::kHeading, level, std::string(text), ""};
    } else if (IsRule(partial_line_)) {
        open_view_ = Block{BlockType::kRule, 0, "", ""};
    } else if (MatchBullet(partial_line_, &text)) {
        bool checked = false;
        std::string_view rest;
        if (MatchTask(text, &checked, &rest)) {
            open_view_ = Block{BlockType::kTask, checked ? 1 : 0, std::string(rest), ""};
        } else {
            open_view_ = Block{BlockType::kBullet, 0, std::string(text), ""};
        }
    } else if (MatchOrdered(partial_line_, &level, &text)) {
        open_view_ = Block{BlockType::kOrdered, level, std::string(text), ""};
    } else if (MatchQuote(partial_line_, &text)) {
        open_view_ = Block{BlockType::kQuote, 0, std::string(text), ""};
    } else {
        open_view_ = Block{BlockType::kParagraph, 0, partial_line_, ""};
    }
    has_open_view_ = true;
}

std::vector<Block> StreamParser::TakeClosed() {
    std::vector<Block> out = std::move(closed_);
    closed_.clear();
    return out;
}

const Block* StreamParser::Open() const { return has_open_view_ ? &open_view_ : nullptr; }

std::vector<Block> StreamParser::Finish() {
    if (!partial_line_.empty()) {
        std::string_view line(partial_line_);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ProcessLine(line);
        partial_line_.clear();
    }
    if (state_ == State::kTablePending) {  // header candidate never got its separator
        open_ = Block{BlockType::kParagraph, 0, std::move(table_pending_line_), ""};
        has_open_ = true;
        table_pending_line_.clear();
    }
    table_headers_.clear();
    CloseOpen();
    has_open_view_ = false;
    open_rev_++;
    return TakeClosed();
}

}  // namespace lvmd
