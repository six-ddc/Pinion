/* sim shim — FreeRTOS tasks on pthreads (detached). Stack size / priority /
 * core pinning are accepted and ignored. vTaskDelete supports self (NULL)
 * only, matching how the firmware uses it. */
#ifndef SIM_FREERTOS_TASK_H
#define SIM_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* arg,
                       UBaseType_t priority, TaskHandle_t* out_handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                                   void* arg, UBaseType_t priority, TaskHandle_t* out_handle,
                                   BaseType_t core_id);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t handle); /* NULL (self) only */

#ifdef __cplusplus
}
#endif
#endif /* SIM_FREERTOS_TASK_H */
