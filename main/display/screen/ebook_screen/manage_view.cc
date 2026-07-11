// manage_view.cc — 书籍管理入口页。见头文件。

#include "manage_view.h"

#include "book_config_server.h"
#include "ebook_ui_theme.h"
#include "screen_util.h"

#include "wifi_station.h"

#include <cstdio>
#include <string>

namespace manage_view {
namespace {

using ebook_ui::Hex;

Callbacks s_cb;
uint8_t s_theme_idx = 1;
lv_obj_t* s_root = nullptr;
lv_obj_t* s_url = nullptr;
lv_obj_t* s_hint = nullptr;
lv_obj_t* s_count = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_tip = nullptr;

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

lv_obj_t* Build(lv_obj_t* parent, const Callbacks& cb, uint8_t theme_idx) {
    s_cb = cb;
    s_theme_idx = theme_idx;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(theme_idx);

    s_root = lv_obj_create(parent);
    screen_strip_obj_chrome(s_root);
    lv_obj_set_size(s_root, ebook_ui::kPanelW, ebook_ui::kPanelH);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, Hex(th.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* back = lv_button_create(s_root);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackCb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);

    s_title = lv_label_create(s_root);
    lv_label_set_text(s_title, "书籍管理");
    lv_obj_set_style_text_font(s_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title, Hex(th.text), LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 30);

    s_tip = lv_label_create(s_root);
    lv_label_set_text(s_tip, "手机连同一 WiFi，浏览器打开下面地址");
    lv_obj_set_style_text_font(s_tip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_tip, Hex(th.dim), LV_PART_MAIN);
    lv_obj_align(s_tip, LV_ALIGN_CENTER, 0, -80);

    s_url = lv_label_create(s_root);
    lv_label_set_text(s_url, "");
    lv_obj_set_style_text_font(s_url, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_url, Hex(th.accent), LV_PART_MAIN);
    lv_obj_align(s_url, LV_ALIGN_CENTER, 0, -20);

    s_hint = lv_label_create(s_root);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, ebook_ui::kPanelW - 80);
    lv_label_set_text(s_hint, "在网页里上传 / 删除 书籍(TXT/EPUB) 与字体(TTF/OTF)；操作后在本机点返回即可刷新。");
    lv_obj_set_style_text_font(s_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, Hex(th.dim), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 60);

    s_count = lv_label_create(s_root);
    lv_label_set_text(s_count, "");
    lv_obj_set_style_text_font(s_count, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_count, Hex(th.dim), LV_PART_MAIN);
    lv_obj_align(s_count, LV_ALIGN_BOTTOM_MID, 0, -40);

    return s_root;
}

void Show(int book_count) {
    if (s_root) lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(s_theme_idx);

    std::string ip;
    if (WifiReady(ip)) {
        book_config_server::Start();
        std::string url = "http://" + ip;
        lv_label_set_text(s_url, url.c_str());
        lv_obj_set_style_text_color(s_url, Hex(th.accent), LV_PART_MAIN);
        lv_label_set_text(s_hint, "在网页里上传 / 删除 书籍(TXT/EPUB) 与字体(TTF/OTF)；操作后在本机点返回即可刷新。");
    } else {
        lv_label_set_text(s_url, "Web 管理需连接 WiFi");
        lv_obj_set_style_text_color(s_url, Hex(0xFF6B6B), LV_PART_MAIN);
        lv_label_set_text(s_hint, "当前无 WiFi（或使用 4G）。请先连接 WiFi 后再进入本页。");
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "当前 %d 本", book_count);
    lv_label_set_text(s_count, buf);
}

void Hide() {
    book_config_server::Stop();
    if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void SetThemeIdx(uint8_t idx) {
    s_theme_idx = idx;
    const ebook_ui::Theme& th = ebook_ui::ThemeAt(idx);
    if (s_root) lv_obj_set_style_bg_color(s_root, Hex(th.bg), LV_PART_MAIN);
    if (s_title) lv_obj_set_style_text_color(s_title, Hex(th.text), LV_PART_MAIN);
    if (s_tip) lv_obj_set_style_text_color(s_tip, Hex(th.dim), LV_PART_MAIN);
    if (s_hint) lv_obj_set_style_text_color(s_hint, Hex(th.dim), LV_PART_MAIN);
    if (s_count) lv_obj_set_style_text_color(s_count, Hex(th.dim), LV_PART_MAIN);
}

void Reset() {
    book_config_server::Stop();
    s_root = nullptr;
    s_url = nullptr;
    s_hint = nullptr;
    s_count = nullptr;
    s_title = nullptr;
    s_tip = nullptr;
}

}  // namespace manage_view
