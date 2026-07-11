// stock_settings_view.cc — 见头文件。

#include "stock_settings_view.h"

#include "screen_util.h"
#include "stock_config_server.h"
#include "stock_store.h"
#include "stock_ui_theme.h"

#include "wifi_station.h"

#include <cstdio>
#include <string>

namespace stock_settings_view {
namespace {

constexpr int32_t kPanelW = 720;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_url = nullptr;
lv_obj_t* s_hint = nullptr;
lv_obj_t* s_count = nullptr;
Callbacks s_cb;

void OnBackCb(lv_event_t*) {
    if (s_cb.on_back) s_cb.on_back();
}

bool WifiReady(std::string& ip) {
    auto& sta = WifiStation::GetInstance();
    if (!sta.IsConnected()) return false;
    ip = sta.GetIpAddress();
    return !ip.empty() && ip != "0.0.0.0";
}

}  // namespace

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb) {
    s_cb = cb;
    lv_obj_t* root = lv_obj_create(parent);
    screen_strip_obj_chrome(root);
    lv_obj_set_size(root, kPanelW, kPanelW);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    s_root = root;

    lv_obj_t* back = lv_button_create(root);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 56, 56);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 20);
    lv_obj_add_event_cb(back, OnBackCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "自选股配置");
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, stock_ui::Text(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t* tip = lv_label_create(root);
    lv_label_set_text(tip, "手机连同一 WiFi，浏览器打开下面地址");
    lv_obj_set_style_text_font(tip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(tip, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_align(tip, LV_ALIGN_CENTER, 0, -80);

    s_url = lv_label_create(root);
    lv_label_set_text(s_url, "");
    lv_obj_set_style_text_font(s_url, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_url, lv_color_hex(0x26C281), LV_PART_MAIN);
    lv_obj_align(s_url, LV_ALIGN_CENTER, 0, -20);

    s_hint = lv_label_create(root);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, kPanelW - 80);
    lv_label_set_text(s_hint, "搜索、添加、删除、改名、排序自选股；编辑后点返回列表生效。");
    lv_obj_set_style_text_font(s_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 60);

    s_count = lv_label_create(root);
    lv_label_set_text(s_count, "");
    lv_obj_set_style_text_font(s_count, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_count, stock_ui::Dim(), LV_PART_MAIN);
    lv_obj_align(s_count, LV_ALIGN_BOTTOM_MID, 0, -40);

    return root;
}

void Show() {
    if (s_root) lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    std::string ip;
    if (WifiReady(ip)) {
        stock_config_server::Start();
        std::string url = "http://" + ip;
        lv_label_set_text(s_url, url.c_str());
        lv_obj_set_style_text_color(s_url, lv_color_hex(0x26C281), LV_PART_MAIN);
        lv_label_set_text(s_hint, "搜索、添加、删除、改名、排序自选股；编辑后点返回列表生效。");
    } else {
        lv_label_set_text(s_url, "Web 配置需连接 WiFi");
        lv_obj_set_style_text_color(s_url, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
        lv_label_set_text(s_hint, "当前无 WiFi（或使用 4G）。请先连接 WiFi 后再进入本页配置。");
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "当前 %u/16 支", (unsigned)stock_store::Count());
    lv_label_set_text(s_count, buf);
}

void Hide() {
    stock_config_server::Stop();
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void Reset() {
    stock_config_server::Stop();
    s_root = nullptr;
    s_url = nullptr;
    s_hint = nullptr;
    s_count = nullptr;
}

}  // namespace stock_settings_view
