/* pi-c — MetalioClaw5 pi_screen App: the only contract between the agent
 * thread (pi_agent_task.c, C) and the screen (pi_screen.cc, C++). POD types
 * and a tiny C API only, no logic here. See blueprint §2/§4 for the full
 * event -> queue -> widget mapping owned by pi_agent_task.c / pi_screen.cc.
 * SPDX-License-Identifier: MIT */
#ifndef PI_UI_BRIDGE_H
#define PI_UI_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_AGENT_START,
    UI_THINKING_START,
    UI_THINKING_END,
    UI_TOOL_START,
    UI_TOOL_ARGS,
    UI_TOOL_END,
    UI_TEXT_DELTA,
    UI_DONE,
    UI_ERROR,
    /* pi_card 声明式 UI 卡片（见 pi_card/）。工具 execute 在 worker 线程校验后入队，
       drain（LVGL 线程）按流式顺序真正建/改/删控件。 */
    UI_CARD_RENDER,
    UI_CARD_UPDATE,
    UI_CARD_CLOSE
} pi_ui_kind_t;

typedef struct {
    pi_ui_kind_t kind;
    char *s1;
    char *s2;
    char *s3;
    int i1;
    int i2;
} pi_ui_evt_t;
/* s1/s2/s3 由 agent 线程 strdup（普通 malloc），drain 侧 free。
   TOOL_START: s1=tool_name; TOOL_ARGS: s1=partial_json;
   TOOL_END: s1=name,s2=output,i1=elapsed_ms;
   TEXT_DELTA: s1=fragment,i2=out_tokens; ERROR: s1=msg;
   DONE: i1=usage.input,i2=usage.output（本次 run 最后一条 assistant 消息的真实
   pi_usage_t 用量，来自 pi-c MESSAGE_END 事件的 message->usage，非估算值）。
   CARD_RENDER: s1=root 子树 json,s2=card_id,i1=display(0chat/1overlay),i2=ttl_ms;
   CARD_UPDATE: s1=card_id,s2=node_id,s3=props json; CARD_CLOSE: s1=card_id。 */

QueueHandle_t pi_ui_queue(void);      /* 懒创建，长度 ~32，元素 sizeof(pi_ui_evt_t) */
void pi_agent_task_start(void);       /* 建 env/agent/task，幂等 */
void pi_agent_task_send_prompt(const char *preset);
void pi_agent_task_abort(void);       /* -> pi_agent_abort，线程安全 */
void pi_agent_task_new_session(void); /* abort+wait+destroy+重建 agent（mock.next 归零） */

/* 只读 getter（加不减扩展，需求变更：真实 API 取代 mock 后新增）。返回值指向
   models.json 目录常驻存储（agent 生命周期内不变），线程安全，pi_agent_task_start()
   之后任意时刻可调；模型未加载成功时返回 "?"。UI_ERROR 的文案沿用既有
   pi_ui_evt_t.s1（TEXT above: "ERROR: s1=msg"）——POD 已经够用，未新增字段。 */
const char *pi_agent_model_name(void); /* 真实模型名（model.name，缺省退回 model.id） */
uint32_t pi_agent_context_window(void); /* model.context_window（deepseek=1000000）；模型未加载时 0 */

/* TTS 播报开关（火山 volc_tts；同样是"加不减"扩展）。开关本体在 agent 线程侧
   消费（text_delta 是否喂给 volc_tts）；持久化（NVS "pi_screen"/"tts_on"）由
   pi_screen 负责，LOAD 与状态栏开关翻转时调 set。关闭时立即打断当前播报
  （内部异步执行，不阻塞调用线程）。 */
bool pi_agent_tts_enabled(void);
void pi_agent_tts_set_enabled(bool enable);

/* 打断当前播报并作废本次回答尚未播出的文本缓冲（barge-in / 用户开口 / STOP）。
   内部异步停播、不阻塞调用线程，可从任意任务调用。UI 侧 barge-in 必须走它而非
   裸 volc_tts_stop()——否则 TTS pump 会继续念缓冲里的旧文本。 */
void pi_agent_task_tts_cancel(void);

/* TTS 朗读文本的生命周期由 UI 侧驱动（UI 侧才有 markdown 解析上下文，能把回复剥
   成纯文本再喂，使朗读内容 == 屏幕显示内容）。三者都非阻塞（会阻塞的 volc_tts
   调用在内部 pump 任务上执行），从 LVGL 线程调用即可：
   - run_start：新回复开始（收到 UI_AGENT_START 时）——重置本 run 的朗读缓冲；
   - feed：追加一段已剥离 markdown 的纯文本（加粗/标题/URL 等符号已去掉）；
   - run_end：本回复文本结束（收到 UI_DONE 时）——pump 排空后收尾，余音播完。 */
void pi_agent_task_tts_run_start(void);
void pi_agent_task_tts_feed(const char *plain_utf8);
void pi_agent_task_tts_run_end(void);

/* pi_card 交互回传：把 UI 卡片上的用户操作（选择/开关…）注入回 LLM。运行中用
   pi_agent_steer 插到下一轮之前；空闲则起一轮让助手回应用户的选择。线程安全，
   从 LVGL 线程（动作分发）调用。见 pi_card/pi_card_actions.cc。 */
void pi_agent_task_inject(const char *text);

#ifdef __cplusplus
}
#endif
#endif /* PI_UI_BRIDGE_H */
