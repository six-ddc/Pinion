#include "pi_card_actions.h"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"

#include "pi_card_cmd.h"     // CommandRegistry（invoke 动作）
#include "pi_card_data.h"
#include "pi_card_host.h"
#include "pi_card_render.h"  // ApplyProps（patch 复用）、ChoiceValue（choice 取值）
#include "pi_ui_bridge.h"    // pi_agent_task_inject（C 桥，把交互事件注入回 LLM）

#define TAG "pi_card_act"

namespace pi_card {

namespace {

ActionKind KindFromString(const char* s) {
    if (!s) return ActionKind::Unknown;
    if (std::strcmp(s, "close") == 0) return ActionKind::Close;
    if (std::strcmp(s, "set") == 0) return ActionKind::Set;
    if (std::strcmp(s, "report") == 0) return ActionKind::Report;
    if (std::strcmp(s, "toggle") == 0) return ActionKind::Toggle;
    if (std::strcmp(s, "show") == 0) return ActionKind::Show;
    if (std::strcmp(s, "hide") == 0) return ActionKind::Hide;
    if (std::strcmp(s, "patch") == 0) return ActionKind::Patch;
    if (std::strcmp(s, "invoke") == 0) return ActionKind::Invoke;
    return ActionKind::Unknown;
}

// 同卡 id → 控件。校验器已挡过不存在的 target，运行时查不到只可能是节点已被删。
lv_obj_t* FindNode(const UiCard* card, const std::string& id) {
    if (!card) return nullptr;
    auto it = card->nodes.find(id);
    return it == card->nodes.end() ? nullptr : it->second;
}

// 读控件当前值；非「有值控件」（button / label / 容器…）返回 false。WidgetValue() 与
// CollectState() 共用它，免得判型逻辑在两处各写一遍、日后漂移。
bool TryWidgetValue(lv_obj_t* widget, int& out) {
    if (!widget) return false;
    if (lv_obj_check_type(widget, &lv_slider_class)) {
        out = lv_slider_get_value(widget);
        return true;
    }
    if (lv_obj_check_type(widget, &lv_bar_class)) {
        out = lv_bar_get_value(widget);
        return true;
    }
    if (lv_obj_check_type(widget, &lv_arc_class)) {
        out = lv_arc_get_value(widget);
        return true;
    }
    if (lv_obj_check_type(widget, &lv_switch_class)) {
        out = lv_obj_has_state(widget, LV_STATE_CHECKED) ? 1 : 0;
        return true;
    }
    if (ChoiceValue(widget, out)) return true;
    return false;
}

// 把 text 里的 {v} / {value} 替换成 value，{label} 替换成 label（choice 选中段文本；
// 非 choice 触发时 label 为空串，{label} 替换成空）。
std::string SubstValue(const std::string& tpl, int value, const std::string& label) {
    std::string out;
    const std::string rep = std::to_string(value);
    for (size_t i = 0; i < tpl.size();) {
        if (tpl.compare(i, 3, "{v}") == 0) {
            out += rep;
            i += 3;
        } else if (tpl.compare(i, 7, "{value}") == 0) {
            out += rep;
            i += 7;
        } else if (tpl.compare(i, 7, "{label}") == 0) {
            out += label;
            i += 7;
        } else {
            out += tpl[i++];
        }
    }
    return out;
}

// 同卡所有「带 id 的有值控件」拼成 "id=v id=v"，附在 report 文本尾巴上。
//
// 为什么以 id 为准：id 是 LLM 声明「我关心这个值」的**既有**表面积（ui_update 本就要求节点
// 先给过 id），复用它就不必新增语法；纯装饰节点（divider/icon）没 id，噪音天然进不来。
// 为什么 bind 了硬件路径的控件也带：模型**读不到** DataHub——agent 只注册了 render/update/
// close 三个写工具，report 是它唯一的回流通道，音量/亮度这类值搭顺风车回来零成本。
// 顺序按 map 的字典序，不是声明序；key=value 自解释，模型不依赖顺序。
std::string CollectState(const UiCard* card) {
    std::string out;
    if (!card) return out;
    for (const auto& [id, widget] : card->nodes) {
        int v = 0;
        if (!TryWidgetValue(widget, v)) continue;
        if (!out.empty()) out += ' ';
        out += id;
        out += '=';
        out += std::to_string(v);
        // choice 额外带上选中段的文本：id=idx(label)，模型不必反查 options[idx] 是什么。
        std::string lbl;
        if (ChoiceLabel(widget, lbl) && !lbl.empty()) {
            out += "(" + lbl + ")";
        }
        // 预留：未来 text 控件回流用 id="str" 引号通道，暂无该控件类型。
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
    bool reflow_needed = false;  // toggle/show/hide/patch 里任一改了显隐/尺寸都要重跑 overlay 收口
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
            case ActionKind::Report: {
                // 载荷 = LLM 写的人话 + 同卡状态快照。快照在「事件发生的此刻」算，节流 flush 时
                // 不重算——届时控件可能已删，且本就该发当时的快照。{label}=触发控件若是 choice，
                // 其选中段文本（非 choice 时为空）。
                std::string lbl;
                ChoiceLabel(target, lbl);
                std::string text = SubstValue(a.text, WidgetValue(target), lbl);
                if (binding->card) {
                    const std::string state = CollectState(binding->card);
                    if (!state.empty()) text += " ｜ 卡片 " + binding->card->id + "：" + state;
                }
                ReportThrottled(text);
                break;
            }
            case ActionKind::Toggle:
            case ActionKind::Show:
            case ActionKind::Hide: {
                lv_obj_t* node = FindNode(binding->card, a.target);
                if (!node) break;
                const bool hide = a.kind == ActionKind::Hide ||
                                  (a.kind == ActionKind::Toggle && !lv_obj_has_flag(node, LV_OBJ_FLAG_HIDDEN));
                if (hide) {
                    lv_obj_add_flag(node, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_remove_flag(node, LV_OBJ_FLAG_HIDDEN);
                }
                reflow_needed = true;
                break;
            }
            case ActionKind::Patch: {
                lv_obj_t* node = FindNode(binding->card, a.target);
                if (!node) break;
                cJSON* props = cJSON_Parse(a.props_json.c_str());
                if (cJSON_IsObject(props)) {
                    cJSON* txt = cJSON_GetObjectItem(props, "text");
                    if (cJSON_IsString(txt)) {
                        std::string lbl;
                        ChoiceLabel(target, lbl);
                        std::string sub = SubstValue(txt->valuestring, WidgetValue(target), lbl);
                        cJSON_ReplaceItemInObject(props, "text", cJSON_CreateString(sub.c_str()));
                    }
                    std::string err;
                    ApplyProps(node, props, err);  // 复用 render.cc 的 ApplyProps
                    if (cJSON_GetObjectItem(props, "text") || cJSON_GetObjectItem(props, "hidden"))
                        reflow_needed = true;
                }
                cJSON_Delete(props);
                break;
            }
            case ActionKind::Invoke:
                CommandRegistry::Instance().Invoke(a.cmd);
                break;
            case ActionKind::Unknown:
                break;
        }
    }
    // 显隐/文本一变，overlay 卡的内容高度就可能变了，必须重跑收口（固定高+滚动 ↔ SIZE_CONTENT
    // 的判定，见 pi_card_host.cc 的 ReflowOverlay）。放在循环外只调一次——一个动作数组可能连
    // 切好几个节点。
    if (reflow_needed && binding->card && binding->card->display == Display::Overlay) {
        ReflowOverlay(binding->card);
    }
}

void FreeBindingCb(lv_event_t* e) {
    delete static_cast<EventBinding*>(lv_event_get_user_data(e));
}

}  // namespace

int WidgetValue(lv_obj_t* widget) {
    int v = 0;
    // 非取值类控件（含 button）兜底 0 —— 工具描述明说「{v} 在 button 上恒为 0」，别改这语义。
    TryWidgetValue(widget, v);
    return v;
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
        } else if (valj && cJSON_IsString(valj) && valj->valuestring) {
            // list 行模板里 {i}/{n} 替换后是字符串（如 set media.play_index value:"{i}" → "0"）：
            // atoi 转整数，让「行 tap 切曲」这类 set 生效。非数字串 atoi 得 0，无害。
            a.has_value = true;
            a.value = std::atoi(valj->valuestring);
        }
        const cJSON* pathj = cJSON_GetObjectItem(item, "path");
        if (pathj && cJSON_IsString(pathj)) a.path = pathj->valuestring;
        const cJSON* txtj = cJSON_GetObjectItem(item, "text");
        if (txtj && cJSON_IsString(txtj)) a.text = txtj->valuestring;
        const cJSON* tgtj = cJSON_GetObjectItem(item, "target");
        if (tgtj && cJSON_IsString(tgtj)) a.target = tgtj->valuestring;
        const cJSON* cmdj = cJSON_GetObjectItem(item, "cmd");
        if (cmdj && cJSON_IsString(cmdj)) a.cmd = cmdj->valuestring;
        if (a.kind == ActionKind::Patch) {
            // props 脱离 cJSON 生命周期：序列化成串存进 Action，dispatch 时重新 Parse。
            const cJSON* propsj = cJSON_GetObjectItem(item, "props");
            if (cJSON_IsObject(propsj)) {
                if (char* s = cJSON_PrintUnformatted(propsj)) {
                    a.props_json = s;
                    cJSON_free(s);
                }
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

bool ValidateActions(const cJSON* arr, const std::set<std::string>& node_ids, std::string& err,
                     bool in_list_row) {
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
            err = "unknown action 'do' (use close|set|report|toggle|show|hide|patch|invoke)";
            return false;
        }
        if (in_list_row && (k == ActionKind::Toggle || k == ActionKind::Show ||
                            k == ActionKind::Hide || k == ActionKind::Patch)) {
            err = "list row templates can't use toggle/show/hide/patch; use report/set/close "
                  "(row identity via {i}/{n}/{item.*})";
            return false;
        }
        if (k == ActionKind::Toggle || k == ActionKind::Show || k == ActionKind::Hide ||
            k == ActionKind::Patch) {
            const cJSON* tgtj = cJSON_GetObjectItem(item, "target");
            if (!cJSON_IsString(tgtj)) {
                err = "toggle/show/hide/patch action needs a target node id";
                return false;
            }
            if (node_ids.find(tgtj->valuestring) == node_ids.end()) {
                err = std::string("toggle/show/hide/patch target '") + tgtj->valuestring +
                      "' is not an \"id\" declared anywhere in this card";
                return false;
            }
        }
        if (k == ActionKind::Patch) {
            const cJSON* propsj = cJSON_GetObjectItem(item, "props");
            if (!cJSON_IsObject(propsj)) {
                err = "patch action needs a 'props' object";
                return false;
            }
        }
        if (k == ActionKind::Set) {
            const cJSON* pathj = cJSON_GetObjectItem(item, "path");
            if (!cJSON_IsString(pathj) || !DataHub::Instance().Has(pathj->valuestring) ||
                !DataHub::Instance().Writable(pathj->valuestring)) {
                err = "set action needs a writable path (" +
                      DataHub::Instance().WritablePathsJoined() + ")";
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
        if (k == ActionKind::Invoke) {
            const cJSON* cmdj = cJSON_GetObjectItem(item, "cmd");
            if (!cJSON_IsString(cmdj) || !CommandRegistry::Instance().Has(cmdj->valuestring)) {
                std::string avail;
                for (const auto& m : CommandRegistry::Instance().ListCommands()) {
                    if (!avail.empty()) avail += ", ";
                    avail += m.name;
                }
                err = "unknown invoke cmd; available: " + avail;
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
