/* sim shim — same declarations as pi-c port/esp32/pi_esp32.h, so
 * pi_agent_task.c compiles unchanged. On host the implementations forward to
 * pi-c's POSIX port: pi_alloc_default / pi_posix_sys / pi_posix_curl_transport
 * (real DeepSeek over libcurl TLS). */
#ifndef PI_ESP32_H
#define PI_ESP32_H

#include "pi/pi_port.h"

#ifdef __cplusplus
extern "C" {
#endif

pi_transport_t* pi_esp32_transport(void);
const pi_sys_t* pi_esp32_sys(void);
const pi_alloc_t* pi_esp32_alloc(void);

#ifdef __cplusplus
}
#endif
#endif
