#pragma once

#include "lvgl.h"
#include "md_parser.h"
#include "md_theme.h"

namespace lvmd {

// Widget pair a block renderer hands back: `root` is the flex child inserted
// into the view column, `label` the text carrier inside it that streaming
// updates rewrite (nullptr for label-less blocks such as rules).
struct BlockWidget {
    lv_obj_t* root = nullptr;
    lv_obj_t* label = nullptr;
};

// Custom per-block-type renderer. Must create the widget under `parent`
// (full width) and style it; text is applied afterwards by the view.
using BlockRenderer = BlockWidget (*)(lv_obj_t* parent, const Block& blk, const MdTheme& th);

// Streaming markdown view: one instance renders one reply stream as a column
// of block widgets. Its lifetime is bound to the LVGL tree -- deleting the
// root object (directly or via an ancestor) destroys the MdView; never
// `delete` it manually, just drop the pointer.
class MdView {
  public:
    static MdView* Create(lv_obj_t* parent, const MdTheme& theme, int32_t width);

    void SetBlockRenderer(BlockType t, BlockRenderer r);

    // Feed a stream delta and incrementally re-render (only the open tail
    // block is rewritten). Shows the cursor but does NOT position it -- call
    // SyncCursor() once per UI tick, after layout is fresh. No-op after
    // Finish().
    void Append(const char* delta);

    // Reposition the cursor from the current layout. Deliberately does not
    // force a layout pass: call it right after the caller's own
    // lv_obj_update_layout (e.g. pi_screen's scroll-to-bottom) so the whole
    // tick pays for exactly one global layout.
    void SyncCursor();

    // Force-close the tail block and remove the cursor. Idempotent.
    void Finish();

    // Toggle cursor visibility; wire to the app's blink timer.
    void BlinkCursor();

    bool finished() const { return finished_; }
    lv_obj_t* root() const { return root_; }

  private:
    MdView(const MdTheme& theme, int32_t width);
    ~MdView() = default;
    static void OnRootDeleted(lv_event_t* e);

    void FinalizeBlock(const Block& blk);
    void RenderTail();
    BlockWidget CreateBlockWidget(const Block& blk);
    void ApplyBlockText(const BlockWidget& w, const Block& blk);
    void UpdateCursorPos();

    StreamParser parser_;
    MdTheme theme_;
    int32_t width_ = 0;
    BlockRenderer renderers_[static_cast<size_t>(BlockType::kCount)] = {};
    lv_obj_t* root_ = nullptr;
    BlockWidget tail_{};
    BlockType tail_type_ = BlockType::kParagraph;
    int tail_level_ = 0;
    uint32_t tail_rev_ = 0;
    bool tail_fence_cjk_ = false;     // sticky mono->mono_cjk fallback for the open fence
    uint32_t finalized_count_ = 0;    // first block in the column gets margin_top 0
    lv_obj_t* last_label_ = nullptr;  // cursor anchor: tail label, else last finalized label
    lv_obj_t* last_finalized_label_ = nullptr;
    lv_obj_t* cursor_ = nullptr;
    bool finished_ = false;
};

}  // namespace lvmd
