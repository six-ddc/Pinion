/* sim shim — FreeRTOS queues as a fixed-capacity ring buffer (by-copy items)
 * with pthread mutex+conds. Timeout 0 = non-blocking poll, portMAX_DELAY =
 * wait forever, else milliseconds. */
#ifndef SIM_FREERTOS_QUEUE_H
#define SIM_FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_queue* QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t ticks_to_wait);
void vQueueDelete(QueueHandle_t q);

#ifdef __cplusplus
}
#endif
#endif /* SIM_FREERTOS_QUEUE_H */
