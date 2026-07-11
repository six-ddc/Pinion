/* pi-c — MetalioClaw5 vendor copy: fixture-replay transport (paced), forked
 * from pi-c/tests/mock_transport.{h,c} (DESIGN.md §10.1) with one addition —
 * chunk_delay_ms — so SSE playback on real hardware unfolds over time instead
 * of returning instantly (blueprint §2 "pi_mock_paced.h / .c"). Vendored
 * rather than pointed at pi-c's tests/ tree because tests/ is not part of the
 * pi-c ESP-IDF component (see pi-c/CMakeLists.txt: only src/ + port/esp32 +
 * port/posix are idf_component_register'd) and this is a 3KB self-owned
 * change with no upstream-pollution concern (blueprint §1 3a).
 * SPDX-License-Identifier: MIT */
#ifndef PI_MOCK_PACED_H
#define PI_MOCK_PACED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pi/pi_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pi_mock_response {
    int status;
    const char *body;
    /* error injection, honored only when pi_mock_t.honor_fail_fields is set */
    size_t fail_after_bytes;
    int fail_code;
} pi_mock_response_t;

#define PI_MOCK_MAX_HEADERS 32

typedef struct pi_mock {
    pi_transport_t t;
    const pi_mock_response_t *responses;
    size_t count;
    size_t next;
    size_t chunk;            /* max bytes served per read() */
    uint32_t chunk_delay_ms; /* vTaskDelay before each read() returns; paces UI playback */
    char last_url[512];
    char *last_body; /* malloc'd copy of the most recent request body */
    char *last_headers[PI_MOCK_MAX_HEADERS];
    size_t last_header_count;
    bool honor_fail_fields;
} pi_mock_t;

void pi_mock_init(pi_mock_t *m, const pi_mock_response_t *responses, size_t count, size_t chunk);
pi_transport_t *pi_mock_transport(pi_mock_t *m);
void pi_mock_deinit(pi_mock_t *m);

/* Find a captured request header by name (case-insensitive, matched up to ':'). */
const char *pi_mock_find_header(const pi_mock_t *m, const char *name);

#ifdef __cplusplus
}
#endif
#endif /* PI_MOCK_PACED_H */
