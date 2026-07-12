/* sim shim — esp_err_t and the handful of error constants the pi_screen
 * sources / volc headers reference. */
#ifndef SIM_ESP_ERR_H
#define SIM_ESP_ERR_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107

#define ESP_ERROR_CHECK(x)                  \
    do {                                    \
        esp_err_t err_rc_ = (x);            \
        if (err_rc_ != ESP_OK) abort();     \
    } while (0)

#ifdef __cplusplus
}
#endif
#endif /* SIM_ESP_ERR_H */
