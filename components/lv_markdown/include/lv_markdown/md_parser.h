#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Streaming line-oriented markdown block parser. No LVGL dependency -- unit
// testable on the host. Feed() accepts arbitrarily split UTF-8 deltas (only
// '\n' is a split point, so multi-byte characters crossing a delta boundary
// are safe); blocks are finalized ("closed") strictly on newline evidence,
// while Open() exposes a best-effort tentative view of the trailing
// still-open block (including the current partial line) for live display.

namespace lvmd {

enum class BlockType : uint8_t {
    kParagraph,
    kHeading,  // level = 1..6
    kBullet,
    kOrdered,  // level = source item number
    kTask,      // - [ ] / - [x]; level = 1 when checked
    kFence,     // info = fence info string (language tag)
    kQuote,
    kRule,
    kTableRow,  // one data row of a GFM table; see Block comment
    kCount,
};

struct Block {
    BlockType type = BlockType::kParagraph;
    int level = 0;
    std::string text;  // content with block prefixes stripped; multi-line joined with '\n'
    std::string info;
    // kTableRow: text = row cells joined with kCellSep, info = header cells
    // joined with kCellSep (tables render expanded, one block per data row).
};

constexpr char kCellSep = '\x1f';

class StreamParser {
  public:
    void Feed(std::string_view delta);

    // Blocks finalized since the last call, in order. Call before Open().
    std::vector<Block> TakeClosed();

    // Trailing open block (tentative classification includes the partial
    // line); nullptr when nothing is open. Valid until the next Feed/Finish.
    const Block* Open() const;

    // Bumped whenever the open block's visible content changes; lets the
    // renderer skip re-rendering an unchanged tail.
    uint32_t OpenRevision() const { return open_rev_; }

    // End of stream: force-close the tail (an unterminated fence is finalized
    // with the content received so far). Returns the remaining closed blocks.
    std::vector<Block> Finish();

  private:
    enum class State : uint8_t { kIdle, kParagraph, kFence, kQuote, kTablePending, kTable };

    void ProcessLine(std::string_view line);
    void CloseOpen();
    void RebuildOpenView();

    State state_ = State::kIdle;
    std::string partial_line_;      // bytes after the last '\n'
    std::string table_pending_line_;  // header-row candidate awaiting its |---| separator
    std::string table_headers_;       // header cells joined with kCellSep while in kTable
    Block open_;                // committed open block (complete lines only)
    bool has_open_ = false;
    Block open_view_;  // open_ + tentative partial line, what Open() returns
    bool has_open_view_ = false;
    uint32_t open_rev_ = 0;
    std::vector<Block> closed_;
};

}  // namespace lvmd
