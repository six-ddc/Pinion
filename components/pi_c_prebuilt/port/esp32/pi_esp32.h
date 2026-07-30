/* pi-c — ESP32 (ESP-IDF) port.
 * SPDX-License-Identifier: MIT */
#ifndef PI_ESP32_H
#define PI_ESP32_H

#include "pi/pi_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* esp_http_client-based streaming transport (TLS via esp_crt_bundle).
 * If your firmware routes networking through its own stack (e.g. xiaozhi's C++
 * Http with WiFi/4G dual-network), implement pi_transport_t over it instead —
 * that is exactly what the vtable is for (DESIGN.md §10.2). */
pi_transport_t *pi_esp32_transport(void);

/* FreeRTOS mutexes + esp_log. */
const pi_sys_t *pi_esp32_sys(void);

/* PSRAM-first allocator: heap_caps_*(MALLOC_CAP_SPIRAM) with internal-RAM
 * fallback (strategy borrowed from esp-claw's heap_caps_calloc_prefer usage). */
const pi_alloc_t *pi_esp32_alloc(void);

/* fs note: use pi_posix_fs() from port/posix — ESP-IDF's VFS provides POSIX
 * stdio/dirent, so the same implementation serves SD/FATFS/SPIFFS paths. */

#ifdef __cplusplus
}
#endif
#endif
