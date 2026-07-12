/* sim shim — FreeRTOS semaphores on pthread mutex+cond. Counting semantics
 * capped at max (mutex: 1/1, binary: 0/1) — enough for AsrLock and the
 * agent worker's prompt hand-off. */
#ifndef SIM_FREERTOS_SEMPHR_H
#define SIM_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_sem* SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

#ifdef __cplusplus
}
#endif
#endif /* SIM_FREERTOS_SEMPHR_H */
