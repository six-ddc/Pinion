/* pi-c — best-effort partial JSON parsing (PI_FEATURE_PARTIAL_JSON, ≙ pi's
 * partial-json use in json-parse.ts): repair a truncated streaming fragment
 * (close open strings/objects/arrays, trim dangling separators) and parse.
 * For consuming tool-call arguments mid-stream (TOOLCALL_DELTA partials).
 * SPDX-License-Identifier: MIT */
#ifndef PI_PARTIAL_JSON_H
#define PI_PARTIAL_JSON_H
#include "cJSON.h" /* a forward typedef would collide with cJSON.h's own
                      typedef — an error under strict C99 (-Wtypedef-redefinition) */
#include "pi_port.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Returns a parsed tree (caller cJSON_Delete's) or NULL when unrecoverable. */
cJSON *pi_partial_json_parse(const pi_alloc_t *a, const char *fragment);

/* Streaming parse with five-level fallback — ALWAYS returns a tree (empty object
 * when nothing parses), never NULL (≙ parseStreamingJson, json-parse.ts:104). */
cJSON *pi_partial_json_parse_streaming(const pi_alloc_t *a, const char *fragment);

/* Strict parse, then a repairJson (control-char/escape repair) retry. Returns a
 * tree or NULL on failure (≙ parseJsonWithRepair, json-parse.ts:85). */
cJSON *pi_partial_json_repair(const pi_alloc_t *a, const char *json);
#ifdef __cplusplus
}
#endif
#endif
