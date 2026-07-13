#include "pi_card_actions.h"

#include <cstring>

#include "esp_log.h"

#include "pi_card_data.h"
#include "pi_card_host.h"
#include "pi_ui_bridge.h"  // pi_agent_task_inject（C 桥，把交互事件注入回 LLM）

#define TAG "pi_card_act"

namespace pi_card {

namespace {

ActionKind KindFromString(const char* s) {
    if (!s) return ActionKind::Unknown;
    if (std::strcmp(s, "close") == 0) return ActionKind::Close;
    if (std::strcmp(s, "set") == 0) return ActionKind::Set;
    if (std::strcmp(s, "report") == 0) return ActionKind::Report;
    return ActionKind::Unknown;
}

// 把 text 里的 {v} / {value} 都替换成 value。
std::string SubstValue(const std::string& tpl, int value) {
    std::string out;
    const std::string rep = std::to_string(value);
    for (size_t i = 0; i < tpl.size();) {
        if (tpl.compare(i, 3, "{v}") == 0) {
            out += rep;
            i += 3;
        } else if (tpl.compare(i, 7, "{value}") == 0) {
            out += rep;
            i += 7;
        } else {
            out += tpl[i++];
        }
    }
    return out;
}

// ---- report → LLM 注入的 500ms 节流（保留最后一条；LVGL 线程独占，无需锁）----
// 计时用 lv_tick_get()（ms，全仓统一，sim/真机通用；uint32 环绕相减仍正确）。
constexpr uint32_t kThrottleMs = 500;
std::string s_pending;
bool s_have_pending = false;
uint32_t s_last_sent_ms = 0;
lv_timer_t* s_flush_timer = nullptr;

void InjectNow(const std::string& text) {
    s_last_sent_ms = lv_tick_get();
    std::string tagged = "「卡片操作」" + text;
    pi_agent_task_inject(tagged.c_str());
    ESP_LOGI(TAG, "report -> %s", text.c_str());
}

void FlushTimerCb(lv_timer_t* t) {
    if (s_have_pending) {
        s_have_pending = false;
        InjectNow(s_pending);
    }
    lv_timer_pause(t);  // 一次性：发完即停，下条 report 再唤醒
}

void ReportThrottled(const std::string& text) {
    uint32_t now = lv_tick_get();
    if (now - s_last_sent_ms >= kThrottleMs) {
        InjectNow(text);
        return;
    }
    s_pending = text;
    s_have_pending = true;
    uint32_t delay_ms = kThrottleMs - (now - s_last_sent_ms) + 1;
    if (s_flush_timer == nullptr) {
        s_flush_timer = lv_timer_create(FlushTimerCb, delay_ms, nullptr);
    } else {
        lv_timer_set_period(s_flush_timer, delay_ms);
        lv_timer_reset(s_flush_timer);
        lv_timer_resume(s_flush_timer);
    }
}

// 挂在控件事件上的堆负载；控件 LV_EVENT_DELETE 时释放。
struct EventBinding {
    UiCard* card = nullptr;
    std::vector<Action> actions;
};

void DispatchCb(lv_event_t* e) {
    auto* binding = static_cast<EventBinding*>(lv_event_get_user_data(e));
    if (!binding) return;
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (const Action& a : binding->actions) {
        switch (a.kind) {
            case ActionKind::Close:
                if (binding->card && binding->card->root) {
                    lv_obj_delete_async(binding->card->root);  // 删祖先须 async
                }
                break;
            case ActionKind::Set: {
                int v = a.has_value ? a.value : WidgetValue(target);
                DataHub::Instance().Write(a.path, v);
                break;
            }
            case ActionKind::Report:
                ReportThrottled(SubstValue(a.text, WidgetValue(target)));
                break;
            case ActionKind::Unknown:
                break;
        }
    }
}

void FreeBindingCb(lv_event_t* e) {
    delete static_cast<EventBinding*>(lv_event_get_user_data(e));
}

}  // namespace

int WidgetValue(lv_obj_t* widget) {
    if (!widget) return 0;
    if (lv_obj_check_type(widget, &lv_slider_class)) return lv_slider_get_value(widget);
    if (lv_obj_check_type(widget, &lv_bar_class)) return lv_bar_get_value(widget);
    if (lv_obj_check_type(widget, &lv_arc_class)) return lv_arc_get_value(widget);
    if (lv_obj_check_type(widget, &lv_switch_class)) {
        return lv_obj_has_state(widget, LV_STATE_CHECKED) ? 1 : 0;
    }
    return 0;
}

std::vector<Action> ParseActions(const cJSON* arr) {
    std::vector<Action> out;
    if (!arr || !cJSON_IsArray(arr)) return out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) continue;
        Action a;
        const cJSON* doj = cJSON_GetObjectItem(item, "do");
        a.kind = KindFromString(cJSON_IsString(doj) ? doj->valuestring : nullptr);
        if (a.kind == ActionKind::Unknown) continue;
        const cJSON* valj = cJSON_GetObjectItem(item, "value");
        if (valj && cJSON_IsNumber(valj)) {
            a.has_value = true;
            a.value = valj->valueint;
        }
        const cJSON* pathj = cJSON_GetObjectItem(item, "path");
        if (pathj && cJSON_IsString(pathj)) a.path = pathj->valuestring;
        const cJSON* txtj = cJSON_GetObjectItem(item, "text");
        if (txtj && cJSON_IsString(txtj)) a.text = txtj->valuestring;
        out.push_back(std::move(a));
    }
    return out;
}

bool ValidateActions(const cJSON* arr, std::string& err) {
    if (!arr) return true;
    if (!cJSON_IsArray(arr)) {
        err = "event handler must be an action array";
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) {
            err = "each action must be an object";
            return false;
        }
        const cJSON* doj = cJSON_GetObjectItem(item, "do");
        ActionKind k = KindFromString(cJSON_IsString(doj) ? doj->valuestring : nullptr);
        if (k == ActionKind::Unknown) {
            err = "unknown action 'do' (use close|set|report)";
            return false;
        }
        if (k == ActionKind::Set) {
            const cJSON* pathj = cJSON_GetObjectItem(item, "path");
            if (!cJSON_IsString(pathj) || !DataHub::Instance().Has(pathj->valuestring) ||
                !DataHub::Instance().Writable(pathj->valuestring)) {
                err = "set action needs a writable path (audio.volume|display.brightness)";
                return false;
            }
        }
        if (k == ActionKind::Report) {
            const cJSON* txtj = cJSON_GetObjectItem(item, "text");
            if (!cJSON_IsString(txtj)) {
                err = "report action needs a text string";
                return false;
            }
        }
    }
    return true;
}

void AttachEvent(lv_obj_t* widget, lv_event_code_t code, UiCard* card, const cJSON* actions_json) {
    if (!actions_json || !cJSON_IsArray(actions_json)) return;
    auto actions = ParseActions(actions_json);
    if (actions.empty()) return;
    auto* binding = new EventBinding{card, std::move(actions)};
    lv_obj_add_event_cb(widget, DispatchCb, code, binding);
    lv_obj_add_event_cb(widget, FreeBindingCb, LV_EVENT_DELETE, binding);
}

}  // namespace pi_card
