#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// PiScreen -- the "pi" App: pi_agent's four-state conversation UI (720x720).
//
// Visual/interaction spec is the design HTML (single source of truth for
// colors/sizes/copy/animation cadence) plus the implementation blueprint
// (architecture decisions -- see repo docs handed to this work package).
// Three persistent view containers (idle / listen / chat; "tool detail" is
// S2's chat view with a tool card expanded in place, not a fourth view) are
// built once in Create() and toggled via LV_OBJ_FLAG_HIDDEN -- never
// deleted/rebuilt. All other state lives as statics in pi_screen.cc, same
// convention as chat_screen.{h,cc}.
// ---------------------------------------------------------------------------
class PiScreen {
 public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // sim 取证脚手架专用：强制切到 Chat 视图。产品代码不得调用。
    static void DebugGoChat();
};
