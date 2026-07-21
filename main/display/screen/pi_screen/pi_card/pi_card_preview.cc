#include "pi_card_preview.h"

#include <cstring>

#include "esp_log.h"

#include "cJSON.h"
#include "pi_card_host.h"
#include "pi_card_render.h"

#define TAG "pi_card_preview"

namespace pi_card {

namespace {

struct PreviewState {
    bool active = false;         // 当前是否有一张存活的预览行（row 非空）
    bool disqualified = false;   // 本次工具调用已确认不预览（preset/非chat display）
    uint32_t gen = 0;            // 预览诞生时的 session gen
    lv_obj_t* row = nullptr;     // CardBeginRow() 建出的透明占位行（预览与正式渲染共用同一把
                                  // FeedHooks.begin_row，见 pi_card_host.cc 的 FeedBeginRow）
    lv_obj_t* tree = nullptr;    // 当前渲染出的预览子树根（对应 args.root），可为 nullptr
    int node_count = 0;          // 预览专用的节点预算计数，跨多次 delta 累积
};
PreviewState s_preview;
bool s_pending_tool_is_render = false;  // 当前正在流式吐字的工具是不是 ui_render

void PreviewRowDeletedCb(lv_event_t*) {
    // row 被外部删除（ClearFeed/屏卸载/adopt 后正式卡自己的生命周期……不对，adopt 会先取走
    // row 指针再清空 s_preview，不会落到这里）——防悬垂：后续任何 PreviewTeardown 调用都不会
    // 再对着这个已经不存在的对象调 lv_obj_delete 第二次。
    s_preview.row = nullptr;
    s_preview.tree = nullptr;
    s_preview.active = false;
}

void PreviewTeardownInternal() {
    if (s_preview.row) lv_obj_delete(s_preview.row);  // 触发 PreviewRowDeletedCb 自清指针
    s_preview.active = false;
    s_preview.row = nullptr;
    s_preview.tree = nullptr;
    s_preview.node_count = 0;
}

}  // namespace

void PreviewOnToolStart(const char* tool_name) {
    if (s_preview.active) {
        ESP_LOGW(TAG, "tool start with a still-active preview — tearing down stale one first");
        PreviewTeardownInternal();
    }
    s_preview.disqualified = false;
    s_pending_tool_is_render = tool_name && std::strcmp(tool_name, "ui_render") == 0;
}

void PreviewOnArgs(const char* partial_json, uint32_t gen) {
    if (!s_pending_tool_is_render || s_preview.disqualified) return;

    cJSON* snap = cJSON_Parse(partial_json ? partial_json : "");
    if (!cJSON_IsObject(snap)) {
        cJSON_Delete(snap);
        return;
    }

    // preset：顶层一旦出现这个键（不论值是否已吐完），本次调用永久放弃预览——preset 展开只
    // 在 execute 阶段发生（ExpandPreset），partial 树里的 root（如果凑巧也有）跟最终渲染的
    // 东西毫无关系，硬渲只会渲出错的内容。
    if (cJSON_GetObjectItem(snap, "preset") != nullptr) {
        s_preview.disqualified = true;
        if (s_preview.active) PreviewTeardownInternal();
        cJSON_Delete(snap);
        return;
    }
    // display：值确定不是 "chat" 时永久放弃（仅 chat 模式预览；overlay/standby 走各自的渲染
    // 通路，跟聊天流内联生长无关）。没出现这个键就按默认口径（chat）继续。注意 partial parser
    // 会把未闭合的字符串**值**补全成完整节点——快照切在 "display":"ch 中间时树里就是 "ch"，
    // 按简单 != "chat" 判会把一次正常的 chat 渲染整场误杀（7 字节步长 previewfeed 抓到的真实
    // bug），所以口径是"不再可能长成 chat"：值仍是 "chat" 的前缀（含空串）就继续等；只有
    // "o"/"overlay"/"standby" 这类前缀已分叉的才放弃。已完整的 "chat" 不会再变长（万一真变出
    // "chatty" 也过不了 execute 期校验，预览兜底撤除，不留孤儿）。
    const cJSON* dispj = cJSON_GetObjectItem(snap, "display");
    if (cJSON_IsString(dispj) && dispj->valuestring &&
        std::strncmp("chat", dispj->valuestring, std::strlen(dispj->valuestring)) != 0) {
        s_preview.disqualified = true;
        if (s_preview.active) PreviewTeardownInternal();
        cJSON_Delete(snap);
        return;
    }

    const cJSON* root_spec = cJSON_GetObjectItem(snap, "root");
    if (!cJSON_IsObject(root_spec)) {
        cJSON_Delete(snap);
        return;  // root 还没吐出来（或压根不是 object），安静等下一帧，不算错
    }

    if (!s_preview.active) {
        lv_obj_t* row = FeedBeginRow();
        if (!row) {
            cJSON_Delete(snap);
            return;  // feed 未就绪：这次工具调用就没有预览，不是错误
        }
        s_preview.row = row;
        s_preview.gen = gen;
        s_preview.node_count = 0;
        s_preview.tree = nullptr;
        s_preview.active = true;
        lv_obj_add_event_cb(row, PreviewRowDeletedCb, LV_EVENT_DELETE, nullptr);
        FeedEndRowOnce();  // 首次建行滚到可见；后续每次生长不再重复滚，不跟用户抢滚动位置
    }

    s_preview.tree = SyncPreviewNode(s_preview.row, s_preview.tree, root_spec, 0, s_preview.node_count);
    // 帧内 ++node_count 只保证本帧不超预算；删除节点时不精确回补（子树带走几个节点不值得
    // 追踪），故每帧收尾用 CountPreviewNodes 对整树重新计数一遍，做下一帧的准确起点——树很小
    // （≤64），重算成本可忽略，比精确维护增减量更不容易出错。
    s_preview.node_count = CountPreviewNodes(s_preview.tree);
    cJSON_Delete(snap);
}

void PreviewOnToolEnd() {
    // execute 已经跑完：若预览仍然存活，说明没有被 PreviewAdopt 取走——校验失败/未生成合法
    // root/其它原因，没有真卡片跟上，撤除，不留孤儿。
    if (s_preview.active) PreviewTeardownInternal();
    s_pending_tool_is_render = false;
    s_preview.disqualified = false;
}

void PreviewTeardown() {
    if (s_preview.active) PreviewTeardownInternal();
}

void PreviewCheckGen(uint32_t cur_gen) {
    if (s_preview.active && s_preview.gen != cur_gen) {
        ESP_LOGI(TAG, "session gen changed (%u -> %u) while preview active, tearing down",
                 static_cast<unsigned>(s_preview.gen), static_cast<unsigned>(cur_gen));
        PreviewTeardownInternal();
    }
}

lv_obj_t* PreviewAdopt() {
    if (!s_preview.active || s_preview.disqualified || !s_preview.row) return nullptr;
    lv_obj_t* row = s_preview.row;
    // 放弃所有权：row 现在归正式渲染管，预览会话复位——不再是"预览"了，不能再被 Teardown 删掉。
    lv_obj_remove_event_cb(row, PreviewRowDeletedCb);
    s_preview.active = false;
    s_preview.row = nullptr;
    s_preview.tree = nullptr;
    s_preview.node_count = 0;
    // 队列序正常不可达（UI_CARD_RENDER 处理前才会 Adopt，TOOL_END 紧随其后才会复位这个标志），
    // 但顺手补上防御：adopt 之后、TOOL_END 之前万一又来一条 ARGS，不能再把它当 ui_render 在
    // 吐字处理——否则会在真卡片已经接管这一行之后又建一个孤儿预览行。
    s_pending_tool_is_render = false;
    return row;
}

lv_obj_t* PreviewDebugTree() { return s_preview.tree; }

}  // namespace pi_card
