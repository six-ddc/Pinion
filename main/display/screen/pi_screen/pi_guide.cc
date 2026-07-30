// pi_guide.cc — 见头文件。

#include "pi_guide.h"

#include <cstdio>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_config.h"
#include "metalio_hal/network.h"
#include "pi_fonts.h"
#include "pi_qr.h"
#include "pi_theme.h"
#include "screen_util.h"
#include "web_admin_httpd.h"

namespace pi_guide {
namespace {

using Tok = pi_theme::Tok;

constexpr int32_t kCardW = 560;
constexpr int32_t kQrSize = 200;

lv_obj_t* s_card = nullptr;
lv_obj_t* s_llm_lbl = nullptr;
lv_obj_t* s_voice_lbl = nullptr;
lv_obj_t* s_step_lbl = nullptr;  // 下一步做什么（含地址）
lv_obj_t* s_qr = nullptr;
lv_obj_t* s_btn = nullptr;  // 「开始配网」，仅未连 WiFi 且不在配网态时出现
lv_obj_t* s_btn_lbl = nullptr;
bool s_portal_requested = false;

void SetLabel(lv_obj_t* l, const lv_font_t* font, Tok tone) {
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    pi_theme::ApplyText(l, tone);
}

void Show(lv_obj_t* o, bool on) {
    if (o == nullptr) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// 「开始配网」：与设置页网络卡同一条真机可靠路径——置 force_ap 后重启进干净 AP 态，
// 绝不就地 StartConfigPortal（STA→AP 切换在 C5 上会崩）。
void OnPortalClicked(lv_event_t*) {
    if (s_portal_requested || mhal::network::IsConfigPortalActive()) return;
    s_portal_requested = true;
    if (s_step_lbl != nullptr) lv_label_set_text(s_step_lbl, "即将重启进入配网…");
    // 延迟一拍让提示先绘出，再重启（RequestConfigPortalReboot 内部 esp_restart，不返回）。
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(700));
            mhal::network::RequestConfigPortalReboot();
            vTaskDelete(nullptr);
        },
        "cfg_reboot", 4096, nullptr, 5, nullptr);
}

}  // namespace

bool Needed() { return !device_config::LlmReady() || !device_config::VoiceReady(); }

void Build(lv_obj_t* parent) {
    if (s_card != nullptr || parent == nullptr) return;

    s_card = lv_obj_create(parent);
    screen_strip_obj_chrome(s_card);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_card, kCardW);
    lv_obj_set_height(s_card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_card, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_card, 1, LV_PART_MAIN);
    pi_theme::ApplyBg(s_card, Tok::Card);
    pi_theme::ApplyBorder(s_card, Tok::Line);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(s_card);
    lv_label_set_text(title, "先完成配置");
    SetLabel(title, &font_puhui_20_4, Tok::Tx);

    s_llm_lbl = lv_label_create(s_card);
    SetLabel(s_llm_lbl, &font_puhui_20_4, Tok::Dim);
    s_voice_lbl = lv_label_create(s_card);
    SetLabel(s_voice_lbl, &font_puhui_20_4, Tok::Dim);

    s_qr = pi_qr::Make(s_card, kQrSize);

    s_step_lbl = lv_label_create(s_card);
    SetLabel(s_step_lbl, &font_puhui_20_4, Tok::Accent);
    lv_obj_set_width(s_step_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_step_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_step_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_btn = lv_button_create(s_card);
    screen_strip_obj_chrome(s_btn);
    lv_obj_set_style_radius(s_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_btn, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn, 1, LV_PART_MAIN);
    pi_theme::ApplyBg(s_btn, Tok::Card2);
    pi_theme::ApplyBorder(s_btn, Tok::Accent);
    lv_obj_add_event_cb(s_btn, OnPortalClicked, LV_EVENT_CLICKED, nullptr);
    s_btn_lbl = lv_label_create(s_btn);
    lv_label_set_text(s_btn_lbl, "开始配网");
    SetLabel(s_btn_lbl, &font_puhui_20_4, Tok::Accent);
    lv_obj_center(s_btn_lbl);

    Refresh();
}

void Refresh() {
    if (s_card == nullptr) return;

    const bool llm = device_config::LlmReady();
    const bool voice = device_config::VoiceReady();
    lv_label_set_text(s_llm_lbl, llm ? "大模型 已配置" : "大模型 未配置");
    pi_theme::ApplyText(s_llm_lbl, llm ? Tok::Ok : Tok::Err);
    lv_label_set_text(s_voice_lbl, voice ? "语音 已配置" : "语音 未配置");
    pi_theme::ApplyText(s_voice_lbl, voice ? Tok::Ok : Tok::Err);

    const bool is_wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
    const bool wifi_up = is_wifi && mhal::network::IsConnected();

    if (!is_wifi) {
        // 4G：运营商 NAT，手机无法访问设备上的服务。
        pi_qr::Update(s_qr, "");
        lv_label_set_text(s_step_lbl, "当前是 4G，手机无法访问设备。请到 设置 → 网络 切到 WiFi");
        Show(s_btn, false);
        return;
    }

    if (wifi_up) {
        // 幂等：后台可能被 10min 闲置自停掉，引导可见期间每次刷新都补一次。
        if (!web_admin::httpd::IsRunning()) web_admin::httpd::Start();
        std::string url = web_admin::httpd::GetUrl();
        pi_qr::Update(s_qr, url);
        char buf[128];
        if (url.empty()) {
            lv_label_set_text(s_step_lbl, "正在获取地址…");
        } else {
            std::snprintf(buf, sizeof(buf), "手机扫码，或浏览器打开 %s", url.c_str());
            lv_label_set_text(s_step_lbl, buf);
        }
        Show(s_btn, false);
        return;
    }

    // WiFi 模式但没连上：已在配网态 → 给热点二维码（扫码直连热点，再开 192.168.4.1）；
    // 否则引导点按重启进配网。
    if (mhal::network::IsConfigPortalActive()) {
        std::string ssid = mhal::network::GetConfigPortalSsid();
        // 事件里 data 是 "ssid|url"，这里只要 ssid 部分。
        size_t bar = ssid.find('|');
        if (bar != std::string::npos) ssid = ssid.substr(0, bar);
        // WIFI: 二维码格式（开放热点）——相机扫一下即可加入。
        pi_qr::Update(s_qr, ssid.empty() ? "" : ("WIFI:T:nopass;S:" + ssid + ";;"));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "扫码连热点 %s，再打开 http://192.168.4.1 填 WiFi",
                      ssid.empty() ? "(设备热点)" : ssid.c_str());
        lv_label_set_text(s_step_lbl, buf);
        Show(s_btn, false);
        return;
    }

    pi_qr::Update(s_qr, "");
    lv_label_set_text(s_step_lbl, s_portal_requested ? "即将重启进入配网…"
                                                     : "先连 WiFi：点下面开始配网");
    Show(s_btn, !s_portal_requested);
}

}  // namespace pi_guide
