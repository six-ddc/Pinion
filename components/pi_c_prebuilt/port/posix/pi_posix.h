/* pi-c — POSIX host port (tests / examples / development).
 * SPDX-License-Identifier: MIT */
#ifndef PI_POSIX_H
#define PI_POSIX_H

#include "pi/pi_port.h"

#ifdef __cplusplus
extern "C" {
#endif

const pi_sys_t *pi_posix_sys(void); /* pthread mutexes + stderr logging */
const pi_fs_t *pi_posix_fs(void);   /* stdio + dirent */

/* libcurl-backed streaming transport (thread + pipe). NULL when the library was
 * built without curl (PI_HAVE_CURL undefined). */
pi_transport_t *pi_posix_curl_transport(void);

#ifdef __cplusplus
}
#endif
#endif
