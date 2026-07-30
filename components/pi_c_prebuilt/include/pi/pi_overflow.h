/* pi-c — context-overflow classification (≙ pi ai/src/utils/overflow.ts).
 * SPDX-License-Identifier: MIT */
#ifndef PI_OVERFLOW_H
#define PI_OVERFLOW_H

#include "pi/pi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Does this assistant message mean "the request exceeded the model's context
 * window"? ≙ isContextOverflow(message, contextWindow). Three independent signals:
 *
 *  1. stop_reason == PI_STOP_ERROR with an error_message that matches one of the 25
 *     provider overflow patterns AND none of the 3 non-overflow (throttling /
 *     rate-limit) patterns — the non-overflow check runs FIRST, so Bedrock's
 *     "Throttling error: Too many tokens…" is not misread as overflow.
 *  2. silent overflow (z.ai): stop_reason == PI_STOP_STOP and
 *     usage.input + usage.cache_read > context_window.
 *  3. length-stop overflow (Xiaomi MiMo): stop_reason == PI_STOP_LENGTH with
 *     usage.output == 0 and usage.input + usage.cache_read >= context_window * 0.99
 *     (the server truncated the input to exactly fill the window).
 *
 * `context_window` == 0 means "unknown" (≙ the optional contextWindow parameter being
 * omitted upstream): only signal 1 is evaluated. Returns false for a NULL message or
 * a non-assistant role.
 *
 * Matching is case-insensitive substring/prefix matching — there is no regex engine —
 * and the patterns are ported one-for-one; see src/ai/pi_overflow.c for the table. */
bool pi_is_context_overflow(const pi_message_t *m, uint32_t context_window);

#ifdef __cplusplus
}
#endif

#endif /* PI_OVERFLOW_H */
