#include "pi_card_preview.h"

#include <cstring>
#include <vector>

#include "esp_log.h"

#include "cJSON.h"
#include "pi_card_host.h"
#include "pi_card_media.h"  // SpecUsesMedia：媒体卡实验屏蔽，预览期即拦
#include "pi_card_preview_sig.h"
#include "pi_card_render.h"
#include "pi_card_solver.h"

#define TAG "pi_card_preview"

namespace pi_card {

namespace {

constexpr int kMaxGridsPreview = 8;  // 同 Validate 的 kMaxGrids（§6.1），预览多出的 grid 静默不建

struct GridSlot {
    uint32_t sig = 0;
    lv_obj_t* obj = nullptr;
};

struct PreviewState {
    bool active = false;         // 当前是否有一张存活的预览行（row 非空）
    bool disqualified = false;   // 本次工具调用已确认不预览（display 非 chat）
    uint32_t gen = 0;            // 预览诞生时的 session gen
    lv_obj_t* row = nullptr;     // CardBeginRow() 建出的透明占位行（预览与正式渲染共用同一把
                                  // FeedHooks.begin_row，见 pi_card_host.cc 的 FeedBeginRow）
    lv_obj_t* card_root = nullptr;  // MakePreviewCardRoot 建出的卡片外观容器；各 grid 块是它的
                                     // 子节点，§4.2 的整块建/整块删都作用在这一层
    std::vector<GridSlot> grids;    // 索引 = root 数组里的 grid 下标（§4.2 s_grid_sig[]）
};
PreviewState s_preview;
bool s_pending_tool_is_render = false;  // 当前正在流式吐字的工具是不是 ui_render

void PreviewRowDeletedCb(lv_event_t*) {
    // row 被外部删除（ClearFeed/屏卸载/adopt 后正式卡自己的生命周期……不对，adopt 会先取走
    // row 指针再清空 s_preview，不会落到这里）——防悬垂：后续任何 PreviewTeardown 调用都不会
    // 再对着这个已经不存在的对象调 lv_obj_delete 第二次。card_root 是 row 的子节点，随 row 一
    // 起被删，不需要单独处理。
    s_preview.row = nullptr;
    s_preview.card_root = nullptr;
    s_preview.active = false;
    s_preview.grids.clear();
}

void PreviewTeardownInternal() {
    if (s_preview.row) lv_obj_delete(s_preview.row);  // 触发 PreviewRowDeletedCb 自清指针
    s_preview.active = false;
    s_preview.row = nullptr;
    s_preview.card_root = nullptr;
    s_preview.grids.clear();
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

    // display：值确定不是 "chat" 时永久放弃（仅 chat 模式预览；overlay/standby 走各自的渲染
    // 通路，跟聊天流内联生长无关）。没出现这个键就按默认口径（chat）继续。注意 partial parser
    // 会把未闭合的字符串**值**补全成完整节点——快照切在 "display":"ch 中间时树里就是 "ch"，
    // 按简单 != "chat" 判会把一次正常的 chat 渲染整场误杀，所以口径是"不再可能长成 chat"：值
    // 仍是 "chat" 的前缀（含空串）就继续等；只有 "o"/"overlay"/"standby" 这类前缀已分叉的才
    // 放弃。已完整的 "chat" 不会再变长（万一真变出 "chatty" 也过不了 execute 期校验，预览
    // 兜底撤除，不留孤儿）。
    const cJSON* dispj = cJSON_GetObjectItem(snap, "display");
    if (cJSON_IsString(dispj) && dispj->valuestring &&
        std::strncmp("chat", dispj->valuestring, std::strlen(dispj->valuestring)) != 0) {
        s_preview.disqualified = true;
        if (s_preview.active) PreviewTeardownInternal();
        cJSON_Delete(snap);
        return;
    }

    const cJSON* root_spec = cJSON_GetObjectItem(snap, "root");
    if (!cJSON_IsArray(root_spec)) {
        cJSON_Delete(snap);
        return;  // v2：root 是 grid 块数组，还没吐出来（或压根不是数组）时安静等下一帧
    }

    // 播放器卡片已整体删除（播控只走内置播放器 UI，见 pi_card_host 的正式渲染拦截）：
    // spec 一旦出现 media.* 即整场放弃预览——媒体卡的重绘负载正是发生在流式预览期，只拦
    // 正式渲染等于没拦。partial parser 只补全字符串值不发明内容，"media." 前缀一旦出现
    // 不会回退，永久 disqualify 是安全的。
    if (pi_card_media::SpecUsesMedia(root_spec)) {
        s_preview.disqualified = true;
        if (s_preview.active) PreviewTeardownInternal();
        cJSON_Delete(snap);
        return;
    }

    if (!s_preview.active) {
        lv_obj_t* row = FeedBeginRow();
        if (!row) {
            cJSON_Delete(snap);
            return;  // feed 未就绪：这次工具调用就没有预览，不是错误
        }
        s_preview.row = row;
        s_preview.gen = gen;
        s_preview.grids.clear();
        s_preview.card_root = MakePreviewCardRoot(row, solver::kCardWChat);
        s_preview.active = true;
        lv_obj_add_event_cb(row, PreviewRowDeletedCb, LV_EVENT_DELETE, nullptr);
        FeedEndRowOnce();  // 首次建行滚到可见；后续每次生长不再重复滚，不跟用户抢滚动位置
    }

    // data：v2 §4.2 的签名把 data 折了进去（GridSignature），data 迟到/变化会让引用它的 grid
    // 签名跟着变、自然触发整块重渲——不需要 v1 那种全树回刷通道。
    const cJSON* data = cJSON_GetObjectItem(snap, "data");
    PreviewSetData(data);

    const int ngrid_raw = cJSON_GetArraySize(root_spec);
    const int ngrid = ngrid_raw > kMaxGridsPreview ? kMaxGridsPreview : ngrid_raw;
    for (int i = 0; i < ngrid; ++i) {
        const cJSON* grid_json = cJSON_GetArrayItem(root_spec, i);
        const bool is_last = (i == ngrid_raw - 1);  // "当前最后一个" 按未截断的真实末尾判定
        if (static_cast<size_t>(i) < s_preview.grids.size()) {
            if (!is_last) continue;  // 前序 grid 一旦其后出现新 grid 即冻结（§4.1），不再触碰
            const uint32_t sig = preview_sig::GridSignature(grid_json, data);
            GridSlot& slot = s_preview.grids[i];
            if (slot.sig == sig) continue;  // 签名未变：这个位置一动不动
            if (slot.obj) lv_obj_delete(slot.obj);
            slot.obj = RenderGridBlockPreview(s_preview.card_root, grid_json, data,
                                              solver::kCardWChat, solver::kStackGap);
            slot.sig = sig;
        } else {
            // 第一次出现（可能一帧内一次性追加了好几个，见 §4.2："grid 是原子渲染单位"）：
            // 建一次、记签名。
            GridSlot slot;
            slot.sig = preview_sig::GridSignature(grid_json, data);
            slot.obj = RenderGridBlockPreview(s_preview.card_root, grid_json, data,
                                              solver::kCardWChat, solver::kStackGap);
            s_preview.grids.push_back(slot);
        }
    }

    PreviewSetData(nullptr);
    cJSON_Delete(snap);
}

void PreviewOnToolEnd() {
    // execute 已经跑完：若预览仍然存活，说明没有被 PreviewAdopt 取走——校验失败/未生成合法
    // root/其它原因，没有真卡片跟上，撤除，不留孤儿。
    if (s_preview.active) {
        ESP_LOGI(TAG, "preview torn down at tool end (no adopt — likely validate/queue reject)");
        PreviewTeardownInternal();
    }
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
    ESP_LOGI(TAG, "preview adopted by formal render (%u grids)",
             static_cast<unsigned>(s_preview.grids.size()));
    lv_obj_t* row = s_preview.row;
    // 放弃所有权：row 现在归正式渲染管，预览会话复位——不再是"预览"了，不能再被 Teardown 删掉。
    lv_obj_remove_event_cb(row, PreviewRowDeletedCb);
    s_preview.active = false;
    s_preview.row = nullptr;
    s_preview.card_root = nullptr;
    s_preview.grids.clear();
    // 队列序正常不可达（UI_CARD_RENDER 处理前才会 Adopt，TOOL_END 紧随其后才会复位这个标志），
    // 但顺手补上防御：adopt 之后、TOOL_END 之前万一又来一条 ARGS，不能再把它当 ui_render 在
    // 吐字处理——否则会在真卡片已经接管这一行之后又建一个孤儿预览行。
    s_pending_tool_is_render = false;
    return row;
}

lv_obj_t* PreviewDebugTree() { return s_preview.card_root; }

}  // namespace pi_card
