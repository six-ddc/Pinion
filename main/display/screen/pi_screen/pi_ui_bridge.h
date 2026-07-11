/* pi-c — MetalioClaw5 pi_screen App: the only contract between the agent
 * thread (pi_agent_task.c, C) and the screen (pi_screen.cc, C++). POD types
 * and a tiny C API only, no logic here. See blueprint §2/§4 for the full
 * event -> queue -> widget mapping owned by pi_agent_task.c / pi_screen.cc.
 * SPDX-License-Identifier: MIT */
#ifndef PI_UI_BRIDGE_H
#define PI_UI_BRIDGE_H

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
    UI_ERROR
} pi_ui_kind_t;

typedef struct {
    pi_ui_kind_t kind;
    char *s1;
    char *s2;
    int i1;
    int i2;
} pi_ui_evt_t;
/* s1/s2 由 agent 线程 strdup（普通 malloc），drain 侧 free。
   TOOL_START: s1=tool_name; TOOL_ARGS: s1=partial_json;
   TOOL_END: s1=name,s2=output,i1=elapsed_ms;
   TEXT_DELTA: s1=fragment,i2=out_tokens; ERROR: s1=msg;
   DONE: i1=usage.input,i2=usage.output（本次 run 最后一条 assistant 消息的真实
   pi_usage_t 用量，来自 pi-c MESSAGE_END 事件的 message->usage，非估算值）。 */

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

#ifdef __cplusplus
}
#endif
#endif /* PI_UI_BRIDGE_H */
