#include "screen_util.h"

#include <cstdlib>

namespace {

// ---------------------------------------------------------------------------
// Edge-swipe navigation（indev 级，见头文件说明）
// ---------------------------------------------------------------------------

// 边缘带宽。720px 掌上屏（物理约 3.5"）上 48px 只有 ~3mm，比指腹窄、起手容易
// miss；64px（~4.3mm）是首版手感值，真机验证后可调。
constexpr int32_t kEdgeNavZonePx = 64;
// 慢拖兜底的位移阈值（沿用旧 swipe-back 的 80px 口径，约屏宽 11%）。
constexpr int32_t kEdgeNavReleaseThreshold = 80;
// 状态栏高度：顶部两角让给状态栏下拉（pi_screen 的 quick panel 手势）。此值必须
// 与 pi_screen 的 kSbarH 一致；那边不导出，这里独立定义并以注释锚定。
constexpr int32_t kEdgeNavTopExcludePx = 56;

struct EdgeNavState {
    screen_edge_nav_cb_t cb = nullptr;
    lv_point_t start{0, 0};
    bool armed_left = false;
    bool armed_right = false;
    bool handled = false;  // 每次按压至多派发一次（GESTURE 命中后 RELEASED 不再兜底）
};

// 同一时刻只有一个 pointer 在动（真机单触摸；sim 的 mouse/vtouch 不会并发拖动），
// 单全局状态机足够。
EdgeNavState s_edge;

// 按压对象本身拥有横向拖拽语义（滑条可能横跨边缘带）→ 该按压不参与边缘导航。
// 这是免打标后仅存的类型守卫，零维护：新控件无需登记。
bool EdgeNavPressOwnsHorizontalDrag() {
    for (lv_obj_t* obj = lv_indev_get_active_obj(); obj != nullptr; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_arc_class) ||
            lv_obj_check_type(obj, &lv_roller_class)) {
            return true;
        }
    }
    return false;
}

void EdgeNavDispatch(lv_indev_t* indev, screen_edge_nav_dir_t dir, bool suppress_click) {
    s_edge.handled = true;
    s_edge.armed_left = s_edge.armed_right = false;
    // GESTURE 路径在按压中途命中：wait_release 让底下控件收 PRESS_LOST 而非
    // CLICKED（lv_indev.c 松手分支），滑过按钮不会误点。RELEASED 兜底路径此刻
    // CLICKED 已不可抑制（LVGL 的 CLICKED 分支不查 wait_until_release），可接受：
    // 边缘带内几乎无可点击件。
    if (suppress_click && indev != nullptr) lv_indev_wait_release(indev);
    if (s_edge.cb != nullptr) s_edge.cb(dir);
}

void EdgeNavIndevCb(lv_event_t* e) {
    lv_indev_t* indev = static_cast<lv_indev_t*>(lv_event_get_current_target(e));
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_edge.start = p;
        s_edge.handled = false;
        const bool in_band_y = p.y >= kEdgeNavTopExcludePx;  // 顶角归状态栏下拉
        const int32_t hor = lv_display_get_horizontal_resolution(lv_display_get_default());
        s_edge.armed_left = in_band_y && p.x < kEdgeNavZonePx;
        s_edge.armed_right = in_band_y && p.x > hor - kEdgeNavZonePx;
        return;
    }

    if (s_edge.handled || (!s_edge.armed_left && !s_edge.armed_right)) return;

    if (code == LV_EVENT_GESTURE) {
        // 快甩路径：LVGL 手势（速度 ≥3px/帧累计 50px）。与 scroll 天然互斥（LVGL
        // 内部 scroll_obj 非空时不产手势），无需再查滚动。
        if (EdgeNavPressOwnsHorizontalDrag()) return;
        const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_RIGHT && s_edge.armed_left) {
            EdgeNavDispatch(indev, SCREEN_EDGE_NAV_FROM_LEFT, /*suppress_click=*/true);
        } else if (dir == LV_DIR_LEFT && s_edge.armed_right) {
            EdgeNavDispatch(indev, SCREEN_EDGE_NAV_FROM_RIGHT, /*suppress_click=*/true);
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        // 慢拖兜底：不满足手势速度阈值的拖动按松手位移判定。RELEASED 时 LVGL 的
        // scroll_obj 尚未清零——若这次按压其实在滚动某容器（feed 竖滚 / 未来的横向
        // 可滚区），拖拽归它，不导航。
        if (lv_indev_get_scroll_obj(indev) != nullptr) return;
        if (EdgeNavPressOwnsHorizontalDrag()) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        const int32_t dx = p.x - s_edge.start.x;
        const int32_t dy = p.y - s_edge.start.y;
        if (std::abs(dx) <= std::abs(dy)) return;  // 方向占优才算横滑
        if (s_edge.armed_left && dx > kEdgeNavReleaseThreshold) {
            EdgeNavDispatch(indev, SCREEN_EDGE_NAV_FROM_LEFT, /*suppress_click=*/false);
        } else if (s_edge.armed_right && dx < -kEdgeNavReleaseThreshold) {
            EdgeNavDispatch(indev, SCREEN_EDGE_NAV_FROM_RIGHT, /*suppress_click=*/false);
        }
        return;
    }
}

}  // namespace

void screen_edge_nav_init(screen_edge_nav_cb_t cb) {
    if (s_edge.cb != nullptr) {  // 幂等：只更新回调，不重复挂 indev
        s_edge.cb = cb;
        return;
    }
    s_edge.cb = cb;
    for (lv_indev_t* d = lv_indev_get_next(nullptr); d != nullptr; d = lv_indev_get_next(d)) {
        if (lv_indev_get_type(d) != LV_INDEV_TYPE_POINTER) continue;
        lv_indev_add_event_cb(d, EdgeNavIndevCb, LV_EVENT_PRESSED, nullptr);
        lv_indev_add_event_cb(d, EdgeNavIndevCb, LV_EVENT_GESTURE, nullptr);
        lv_indev_add_event_cb(d, EdgeNavIndevCb, LV_EVENT_RELEASED, nullptr);
    }
}

void screen_make_input_passive(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_ADV_HITTEST);

    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        screen_make_input_passive(lv_obj_get_child(obj, i));
    }
}

void screen_strip_obj_chrome(lv_obj_t* obj) {
    if (obj == nullptr) return;
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

// ---------------------------------------------------------------------------
// Screen lifecycle
//
// We piggy-back on LVGL's existing LV_EVENT_SCREEN_LOADED / UNLOADED so the
// user callback runs at the exact same moments per-screen unload handlers
// already do.  Note: we deliberately do NOT delete the screen object here
// (the home screen's launch helpers already async-delete the old screen
// after switching); the lifecycle hook is purely an observer.
// ---------------------------------------------------------------------------

namespace {

void lifecycle_loaded_cb(lv_event_t* e) {
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_LOAD);
    }
}

void lifecycle_unloaded_cb(lv_event_t* e) {
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_UNLOAD);
    }
}

}  // namespace

void screen_attach_lifecycle(lv_obj_t* scr, screen_lifecycle_cb_t cb) {
    if (scr == nullptr || cb == nullptr) {
        return;
    }
    void* user_data = reinterpret_cast<void*>(cb);
    lv_obj_add_event_cb(scr, lifecycle_loaded_cb, LV_EVENT_SCREEN_LOADED,
                        user_data);
    lv_obj_add_event_cb(scr, lifecycle_unloaded_cb, LV_EVENT_SCREEN_UNLOADED,
                        user_data);
}
