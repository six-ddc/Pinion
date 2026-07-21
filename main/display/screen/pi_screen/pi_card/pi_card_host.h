#pragma once

// ---------------------------------------------------------------------------
// pi_card::Host —— 声明式 UI 卡片的宿主 / 注册表 / 生命周期
//
// 一张 LLM 下发的 UI = 一个 UiCard。宿主负责：
//   * 在正确父容器渲染：chat = 塞进 pi_screen 的消息流（经 FeedHooks 回调拿到
//     一行容器）；overlay = 挂 lv_layer_top() 的居中浮层，跨视图存活。
//   * 维护 id -> UiCard 注册表，供 update / close 定位。
//   * 唯一清理通道：每张卡的 root 挂 LV_EVENT_DELETE。无论因何被删（ClearFeed /
//     新会话 / 屏卸载 / close 动作 / ttl），都走 OnRootDeleted → Release 掉所有
//     DataHub 路径 + 出注册表。
//
// 与 pi-c 的接线（见 pi_card_tools.h）：LLM 调 ui_render/update/close 工具，工具
// execute 在 agent worker 线程**同步校验**（不碰 LVGL，错误同步回给 LLM 重试），
// 通过再把 spec 入 pi_ui_queue()；由 pi_screen 的 DrainQueueTick（LVGL 线程）按
// 流式顺序调下面的 On*Event 真正建控件——与既有 tool card 同一条渲染通路。
// ---------------------------------------------------------------------------

#include <list>
#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
#include "lvgl.h"

namespace pi_card {

enum class Display { Chat, Overlay, Pin };

// 渲染/校验的节点数与嵌套深度上限——定义在这里（而非 pi_card_render.h）是因为
// UiCard::DataConsumer（list 重渲用）需要按值持有它，而 render.h 反过来要 include
// 本头取 UiCard，定义挪到这边打破循环 include。
struct RenderLimits {
    int max_nodes = 64;
    int max_depth = 8;
};

// 数值 label 插值动画的 ctx（bind 到非 String 类型 hub 路径的 label 用，见 pi_card_render.cc
// 的 ApplyBind/NumScrollObserverCb/NumScrollExecCb）：subject 变化时不再走原生
// lv_label_bind_text 的一次性跳变，改手动 observer 读到新值后从当前显示值滚动过渡到新值
// （250ms）。anim 的 var 直接是 label 对象本身（不是这个 ctx）——LVGL9 删对象时会自动清掉挂在
// 它上面的动画（lv_obj_destructor 里的 lv_anim_delete(obj, NULL)，已读 lv_obj.c 核实），故不
// 需要像 ChoiceCtx/HwWriteback 那样再挂一个 LV_EVENT_DELETE 回调去手动打断在途动画。ctx 本身
// 借 lv_obj_set_user_data(label, ctx) 挂在 label 上供 exec_cb 反查 fmt；放进 UiCard::num_anims
// （list，地址稳定，不随扩容搬迁）随卡片存活，无需单独 new/free。
struct LabelNumAnim {
    const char* fmt = nullptr;  // 借自 card->str_pool 的稳定地址；nullptr 时退化用 "%d"
    int32_t shown = 0;          // 当前显示值，同时是下一轮插值的起点
};

struct UiCard {
    std::string id;
    lv_obj_t* root = nullptr;                 // 挂 DELETE 清理的对象（chat=行 / overlay=scrim）
    Display display = Display::Chat;
    lv_obj_t* overlay_tree = nullptr;         // overlay 专用：渲染出的 root 子树（LLM 声明的
                                               // column），ReflowOverlay 靠它定位；chat 卡不设
    std::map<std::string, lv_obj_t*> nodes;   // node id -> widget，供 update
    std::vector<std::string> hub_paths;       // 已 Acquire 的 DataHub 路径，供 Release
    // lv_label_bind_text 只借用 fmt 指针不拷贝，而渲染完 cJSON 树立即被 cJSON_Delete
    // 释放。把 fmt 存进这个地址稳定（list 不搬迁元素）、随卡片存活的池里再传给绑定，
    // 覆盖 observer 的整个存活期，杜绝读悬垂 fmt 把后续刷新格式化成乱码。
    std::list<std::string> str_pool;
    std::list<LabelNumAnim> num_anims;        // 数值 label 插值动画状态，见上方结构体注释
    lv_timer_t* ttl_timer = nullptr;          // overlay 自动关闭（一次性）
    bool is_media = false;                    // spec 引用了 media.*：聊天流同类卡只保留最新一张

    // ---- 卡级 data 模型（spec/data 分离，见 Phase2）----
    cJSON* data = nullptr;                    // 卡级 data（owned，object），OnRootDeleted 里 Delete
    std::vector<cJSON*> json_pool;            // owned cJSON（list 行模板克隆），OnRootDeleted 逐个 Delete

    struct DataConsumer {
        lv_obj_t* obj = nullptr;
        enum Kind { Label, List } kind = Label;
        std::string key;                      // 绑定的 data key
        std::string text_tpl;                 // Label：带 {value} 的模板（空=直显原值）
        std::string fmt;                      // Label：可选数值 fmt（预留）
        cJSON* item_tpl = nullptr;             // List：card-owned 模板（在 json_pool 里）
        std::string empty_text;                // List：空数组兜底文案
        int eff_max = 0;                       // List：预留/重渲行上限
        int depth = 0;                         // List：容器所在深度（重渲行传 depth+1）
        RenderLimits limits;                    // List：重渲计数用
        // ---- 改造4：行级 fast path（轻量版）----
        // 行定位不额外存指针数组——靠"list 容器子节点顺序 == 数组下标"这条不变量，直接
        // lv_obj_get_child(dc.obj, i) 按位取行，少一份要跟数据同步的状态。empty_shown 为真时
        // 容器唯一的子节点是 empty_text 占位 label（不是行），fast path 处理任何操作前都要看
        // 一眼这个标志，别把占位 label 当成第 0 行。
        bool empty_shown = false;              // List：当前是否正显示 empty_text 占位（0 行）
        bool tpl_uses_index = false;           // List：行模板序列化后出现过 "{i}"/"{n}" 子串——
                                                // remove 会让后续行的下标整体前移，这类模板的
                                                // 显示内容会跟着过时，remove 操作必须退回全量
                                                // 重建（append/replace 不影响其它行下标，不受
                                                // 此限制）。RenderNode 渲染时用 TemplateUsesIndex
                                                // 扫一次算出存下来，不必每次 update 都重扫模板。
    };
    std::vector<DataConsumer> consumers;
};

// pi_screen 注入的消息流接入点（都在 LVGL 线程调用）。
struct FeedHooks {
    lv_obj_t* (*begin_row)();  // 在 feed 末尾建一个全宽行返回（act_line 保持在最后）
    void (*end_row)();         // 渲染完做滚动到底等收尾
    lv_obj_t* (*pin_host)();          // 常驻卡的父容器（待机屏，仅 Idle 可见）；未就绪返回 nullptr
    void (*on_pin_changed)(bool has_pin);  // pin 有/无变化时通知（ApplyPinLayout 收缩/复原时钟区）
    // 改造1：上一次 begin_row() 是不是命中了流式预览的 adopt（一帧换装）而非新建行——读一次
    // 就清零。OnRenderEvent 收尾据此决定 PlayCardEntrance 的 adopted 参数。
    bool (*last_row_adopted)();
};

// ---- 启动期一次性（PiScreen::Create，LVGL 线程）----
void Init();  // 注册 DataHub 内置路径（幂等）
void SetFeedHooks(const FeedHooks& hooks);
// 转发 FeedHooks 的 begin_row/end_row 给 pi_card_preview.cc（流式生长卡片预览会话），见
// pi_card_host.cc 头注。
lv_obj_t* FeedBeginRow();
void FeedEndRowOnce();

// ---- drain-tick 入口（LVGL 线程，无需显示锁）----
// data_json：卡级 data（object 字面量的 JSON 串，走 pi_ui_evt_t.s3），无则传 nullptr/空。
void OnRenderEvent(const char* spec_json, const char* card_id, int display_mode, int ttl_ms,
                   const char* data_json);
// payload_json：pi_card_tool_update 整份 args 的 JSON 串（{id,props} 或 {data:{...}} 或两者）。
void OnUpdateEvent(const char* card_id, const char* payload_json);
void OnCloseEvent(const char* card_id);

// overlay 卡片的高度稳定器：量出 card->overlay_tree 的自然高度，<=86% 屏高就跟手收缩
// （LV_SIZE_CONTENT，不滚动），超出就钉死为该高度上限并开竖向滚动。渲染时调一次；此后
// 任何会改变内容高度的操作都要重跑它——ui_update 改文本/显隐（见 OnUpdateEvent）、以及
// 未来的本地 toggle/show/hide 节点动作（见 pi_card_actions.cc）。幂等可重入：每次都先
// 复位再重新测量，不在上一次的状态上叠加。非 overlay 卡（overlay_tree==nullptr）直接跳过。
void ReflowOverlay(UiCard* card);

// 息屏门控：有 overlay 卡片开着时不进息屏（同 quick_panel 语义）。
bool HasOpenOverlay();

// 息屏 Off 态门控（pi_sleep 的 on_off 钩子调，LVGL 线程）：off=true 暂停 DataHub 活性
// 刷新与 stock 行情拉取——屏全黑还在刷 subject/拉网络纯属浪费电；keep_history 采样与
// 在途结果落地不受影响。off=false 立即补种/补拉，亮屏第一帧即新值。幂等。
void SetScreenOff(bool off);
// 当前是否处于 SetScreenOff(true) 状态——卡片入场动画（PlayCardEntrance，pi_card_render.cc）
// 与数值 label 插值动画（同文件 NumScrollObserverCb）息屏时都跳过：黑屏没人看得见，白白占
// 一轮 lv_anim tick。
bool IsScreenOff();

// 可见性反查（LVGL 线程）：是否存在「当前可见」（lv_obj_is_visible——含滚出视口、藏在
// 非活跃视图的判定）的卡绑定了以 prefix 开头的 DataHub 路径。stock 的 bind 订阅（背后是
// 周期网络拉取）按它对不可见卡停拉。卡数是个位数量级，逐卡线性扫即可。
bool AnyVisibleCardBindsPrefix(const std::string& prefix);

// ---- Phase3：常驻小组件（display:'standby'，单槽，固定 id "pin"）----
// Create() 末尾调（pi_card::Init + DataHub 注册 + pin host 建好之后，Go(Idle) 之后、drain
// timer 建立前）：读 NVS "ui"/"pin"，解析失败/版本不符/Validate 失败一律 EraseKey 静默丢弃，
// 绝不 assert；成功则渲染（不回写 NVS，因为本来就是从 NVS 读出来的）。LVGL 线程直调。
void RehydratePin();
// 擦 NVS "ui"/"pin" + 删 pin 卡（若存在）。供屏上 ✕ 手势与 ui_close card:'pin' 共用。
void UnpinCard();
bool HasPin();

}  // namespace pi_card
