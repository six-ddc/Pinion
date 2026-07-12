/* sim shim — esp_log to stderr + pi-c ESP32 port constructors mapped onto the
 * POSIX port (pi_agent_task.c stays byte-identical to the firmware). */
#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "pi_esp32.h"
#include "pi_posix.h"

void sim_log_write(char level, const char* tag, const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%c][%s] %s\n", level, tag, line);
}

const pi_alloc_t* pi_esp32_alloc(void) { return pi_alloc_default(); }
const pi_sys_t* pi_esp32_sys(void) { return pi_posix_sys(); }
pi_transport_t* pi_esp32_transport(void) { return pi_posix_curl_transport(); }
