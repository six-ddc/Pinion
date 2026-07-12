/* sim shim — minimal FreeRTOS types/constants for host builds of the pi_screen
 * sources. Only the surface pi_screen.cc / pi_agent_task.c / pi_mock_paced.c
 * actually use; ticks are plain milliseconds (pdMS_TO_TICKS is identity). */
#ifndef SIM_FREERTOS_H
#define SIM_FREERTOS_H

#include <stdint.h>
#include <stdio.h> /* IDF's FreeRTOS header chain pulls this in; firmware sources rely on it */

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)
#define pdFAIL pdFALSE
#define pdPASS pdTRUE

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#ifdef __cplusplus
}
#endif
#endif /* SIM_FREERTOS_H */
