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
    UI_ERROR
} pi_ui_kind_t;

typedef struct {
    pi_ui_kind_t kind;
    char *s1;
    char *s2;
    int i1;
    int i2;
    uint32_t gen; /* 会话代次：new_session 时自增；drain 丢弃 gen != 当前代次的残余事件 */
} pi_ui_evt_t;
/* s1/s2 由 agent 线程 strdup（普通 malloc），drain 侧 free。
   TOOL_START: s1=tool_name; TOOL_ARGS: s1=partial_json;
   TOOL_END: s1=name,s2=output,i1=elapsed_ms;
   TEXT_DELTA: s1=fragment（i2 保留：流式期间无 token 计数，输出量以 DONE 的 usage
   为准）; ERROR: s1=msg;
   DONE: i1=usage.input,i2=usage.output（本次 run 最后一条 assistant 消息的真实
   pi_usage_t 用量，来自 pi-c MESSAGE_END 事件的 message->usage，非估算值）。 */

QueueHandle_t pi_ui_queue(void);      /* 懒创建，长度 ~32，元素 sizeof(pi_ui_evt_t) */
void pi_agent_task_start(void);       /* 建 env/agent/task，幂等 */
void pi_agent_task_send_prompt(const char *preset);
void pi_agent_task_abort(void);       /* -> pi_agent_abort，线程安全 */
void pi_agent_task_new_session(void); /* 非阻塞：代次自增+abort+唤醒 worker，destroy+重建
                                         由 worker 自己做（mock.next 归零），不阻塞 LVGL */
uint32_t pi_agent_task_session_gen(void); /* 当前会话代次，drain 用它丢弃旧会话残余事件 */

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

#ifdef __cplusplus
}
#endif
#endif /* PI_UI_BRIDGE_H */
