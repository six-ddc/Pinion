/* pi-c — application-layer assistant-call retry (≙ packages/ai/src/utils/retry.ts,
 * exported upstream from ai/src/index.ts:43).
 *
 * This is the SECOND of pi's two retry layers, and it is orthogonal to the first:
 *   - transport layer (pi_run_sse_post / retryProviderRequest): retries the request
 *     up to the moment a 2xx body starts streaming, driven by HTTP status and
 *     retry-after headers. It deliberately never retries a stream that already began.
 *   - THIS layer: retries a whole assistant call after it FINISHED with
 *     stop_reason == PI_STOP_ERROR, classified from the error text. It is what covers
 *     mid-stream disconnects ("ended without", "stream ended before message_stop",
 *     "socket hang up") and the transport layer's own retry-delay-cap failure.
 *
 * In-library callers: pi_compact_with_retry / pi_branch_summarize_with_retry thread a
 * caller-supplied policy down to their summarization calls (≙ compact(..., retry,
 * callbacks) at agent-harness.ts:758-767). Everything else is opt-in machinery for
 * embedders. A NULL policy — what the plain pi_compact / pi_branch_summarize pass — is
 * upstream's default (`undefined` ⇒ "no retries"), returning the first response
 * unchanged.
 * SPDX-License-Identifier: MIT */
#ifndef PI_RETRY_H
#define PI_RETRY_H

#include "pi_port.h"
#include "pi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ≙ RetryPolicy (retry.ts:97-103). A NULL policy, or enabled == false, means zero
 * retry attempts — the first response is returned as-is. */
typedef struct pi_retry_policy {
    bool enabled;
    int max_retries;        /* max RETRY attempts; the initial call never counts */
    uint32_t base_delay_ms; /* per-attempt delay is base_delay_ms * 2^(attempt-1) */
} pi_retry_policy_t;

/* ≙ RetryCallbacks (retry.ts:106-118). Every field may be NULL. `user` is passed
 * back verbatim; attempts are 1-indexed. */
typedef struct pi_retry_callbacks {
    /* before the backoff sleep of each retry attempt */
    void (*on_retry_scheduled)(int attempt, int max_attempts, uint32_t delay_ms,
                               const char *error_message, void *user);
    /* after the backoff sleep, immediately before the retried call starts */
    void (*on_retry_attempt_start)(void *user);
    /* once when the loop ends, and ONLY if at least one retry was scheduled
     * (≙ the `if (lastRetry)` guard upstream). final_error may be NULL. */
    void (*on_retry_finished)(bool success, int attempt, const char *final_error, void *user);
    void *user;
} pi_retry_callbacks_t;

/* Produce one assistant message (≙ the `produce()` thunk). Must return PI_OK and set
 * *out to an owned message, or a PI_ERR_* code (which pi_retry_assistant_call
 * propagates immediately, without retrying — upstream has no equivalent case because
 * a thrown error there is not an AssistantMessage). */
typedef int (*pi_retry_produce_fn)(void *user, pi_message_t **out);

/* ≙ isRetryableAssistantError (retry.ts:222-227): true only for a
 * stop_reason == PI_STOP_ERROR message with a non-empty error_message that matches
 * the transient-failure table and NOT the account/quota-limit table. The two pattern
 * tables are ported verbatim from retry.ts:7-88 (case-insensitive, matched anywhere
 * in the text). Cheap and side-effect free — callers may use it standalone to decide
 * whether to restart the last assistant turn. */
bool pi_is_retryable_assistant_error(const pi_message_t *m);

/* ≙ retryAssistantCall (retry.ts:162-210). Runs produce() and, while the response is
 * a retryable error and the budget allows, sleeps base_delay_ms * 2^(attempt-1) and
 * runs it again.
 *
 *  - a non-error, non-aborted response is returned immediately;
 *  - PI_STOP_ABORTED is terminal and never retried;
 *  - a non-retryable error (quota/billing exhaustion included) is returned as-is, so
 *    deterministic failures fail fast;
 *  - an abort DURING the backoff sleep is normalized: the last response is returned
 *    with stop_reason PI_STOP_ABORTED and error_message cleared, so callers need not
 *    care when cancellation happened.
 *
 * `a` frees the discarded intermediate messages and must be the same allocator the
 * messages were created with. `sys` supplies the abort-interruptible sleep; without
 * sys->sleep_ms the backoff cannot be performed and NO retry is attempted (the first
 * response is returned, matching the transport layer's single-shot fallback).
 * abort_flag may be NULL. Returns PI_OK with *out set, or produce()'s error code. */
int pi_retry_assistant_call(const pi_alloc_t *a, const pi_sys_t *sys,
                            pi_retry_produce_fn produce, void *produce_user,
                            const pi_retry_policy_t *policy, volatile bool *abort_flag,
                            const pi_retry_callbacks_t *cb, pi_message_t **out);

#ifdef __cplusplus
}
#endif
#endif /* PI_RETRY_H */
