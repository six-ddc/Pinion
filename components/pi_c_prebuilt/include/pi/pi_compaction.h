/* pi-c — COMPACTION (PI_FEATURE_COMPACTION, depends on PI_FEATURE_SESSION).
 *
 * Context summarization: when a conversation approaches the model's context
 * window, the history before a cut point is replaced by an LLM-generated
 * structured summary, and a `compaction` entry recording the summary + the id of
 * the first retained entry is appended to the v3 session tree. Mirrors pi's
 * harness compaction (upstream: packages/agent/src/harness/compaction/
 * compaction.ts + coding-agent agent-session.ts compact()).
 *
 * The library never compacts on its own: pi_compaction_should_run() is the
 * threshold predicate the application polls (upstream checks it after agent_end),
 * and pi_compact() is the explicit action. token estimation and cut-point
 * selection are exported for testing and reuse.
 * SPDX-License-Identifier: MIT */
#ifndef PI_COMPACTION_H
#define PI_COMPACTION_H

#include "pi_retry.h"   /* pi_retry_policy_t, pi_retry_callbacks_t */
#include "pi_session.h" /* pi_session_t, pi_agent_t, pi_model_t, pi_message_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- settings */
/* ≙ CompactionSettings / DEFAULT_COMPACTION_SETTINGS (compaction.ts:100-115). */
typedef struct pi_compaction_settings {
    uint32_t reserve_tokens;     /* tokens reserved for summary prompt + output (16384) */
    uint32_t keep_recent_tokens; /* approx recent-context tokens to retain (20000) */
    bool enabled;                /* gate automatic compaction decisions (true) */
} pi_compaction_settings_t;

pi_compaction_settings_t pi_compaction_settings_default(void);

/* -------------------------------------------------------- token estimation */
/* Estimated total context tokens for a message list (≙ estimateContextTokens,
 * compaction.ts:169): the most recent valid assistant usage.totalTokens plus a
 * conservative char/4 estimate of the messages after it; a pure char/4 estimate
 * when no usage is present. Images count as ~4800 chars (≙ ESTIMATED_IMAGE_CHARS). */
uint32_t pi_compaction_estimate_tokens(const pi_message_t *const *msgs, size_t n);

/* Whether context usage crosses the compaction threshold (≙ shouldCompact,
 * compaction.ts:200): enabled && estimate > context_window - reserve_tokens.
 * Always false when settings->enabled is false.
 * REGISTERED DEVIATION: also false whenever context_window - reserve_tokens is <= 0
 * (an unknown window of 0, or a small-window model against a large reserve). Upstream's
 * bare inequality is unconditionally TRUE there, so its harness compacts every turn
 * without ever dropping under the threshold; pi-c is polled by the application, which
 * has no such loop to absorb, so it reports "no compaction needed" instead. Explicit
 * pi_compact() calls are unaffected. */
bool pi_compaction_should_run(const pi_message_t *const *msgs, size_t n,
                              uint32_t context_window,
                              const pi_compaction_settings_t *settings);

/* -------------------------------------------------------- cut-point search */
/* Session-entry projection for cut-point selection. Only the fields the
 * boundary rules depend on (≙ SessionTreeEntry, compaction.ts:265-303). */
typedef enum {
    PI_COMPACTION_KIND_USER,           /* message/user       — cut point + turn start, counts tokens */
    PI_COMPACTION_KIND_ASSISTANT,      /* message/assistant  — valid cut point, counts tokens */
    PI_COMPACTION_KIND_TOOL_RESULT,    /* message/toolResult — NOT a cut point, counts tokens */
    PI_COMPACTION_KIND_COMPACTION,     /* compaction entry   — meta-backoff stop */
    PI_COMPACTION_KIND_BRANCH_SUMMARY, /* branch_summary     — cut point + turn start, no token accum */
    PI_COMPACTION_KIND_CUSTOM_MESSAGE, /* custom_message     — cut point + turn start, no token accum */
    PI_COMPACTION_KIND_OTHER           /* model_change/label/leaf/... — no tokens, no cut point */
} pi_compaction_kind_t;

typedef struct pi_compaction_cut_entry {
    pi_compaction_kind_t kind;
    uint32_t tokens; /* per-entry estimate; 0 for non-message entries */
} pi_compaction_cut_entry_t;

/* Cut point selected for compaction (≙ CutPointResult, compaction.ts:322). */
typedef struct pi_cut_point {
    size_t first_kept_index; /* index of the first entry retained after compaction */
    size_t turn_start_index; /* turn-start entry when the cut splits a turn (else == first_kept_index) */
    bool is_split_turn;      /* whether the cut splits an in-progress turn */
} pi_cut_point_t;

/* Select the compaction cut point (≙ findCutPoint, compaction.ts:333): walk back
 * from `end` accumulating message tokens until the keep-recent budget is met, snap
 * forward to the nearest valid cut point (never a toolResult), then back off left
 * over non-message meta entries. first_kept_index is `start` when there is no valid
 * cut point. When the cut lands mid-turn (on a non-user cut point whose turn began
 * at an earlier user/branch_summary/custom_message entry), is_split_turn is set and
 * turn_start_index marks that turn's first entry. Operates on entries[start, end). */
pi_cut_point_t pi_compaction_find_cut_point(const pi_compaction_cut_entry_t *entries, size_t start,
                                            size_t end, uint32_t keep_recent_tokens);

/* ------------------------------------------------------------------ compact */
/* Result of a compaction run (≙ CompactionResult, compaction.ts:88). Strings are
 * owned; release with pi_compaction_result_free. */
typedef struct pi_compaction_result {
    char *summary;             /* generated summary text (with file-op tags) */
    char *first_kept_entry_id; /* entry id where retained history starts */
    uint32_t tokens_before;    /* estimated context tokens before compaction */
    /* Token/cost usage of the summarization call(s) that produced `summary`
     * (≙ CompactionResult.usage). A split turn issues TWO calls and this is their
     * field-wise sum (≙ combineUsage). The same value is persisted on the compaction
     * entry, so pi_session_stats bills summarization like upstream does. */
    pi_usage_t usage;
} pi_compaction_result_t;

void pi_compaction_result_free(const pi_alloc_t *a, pi_compaction_result_t *r);

/* Run compaction against `session` (must be attached to `agent`), summarizing the
 * history before the cut with `model` and the SUMMARIZATION prompts. On PI_OK a
 * `compaction` entry is appended to the session and the agent's in-memory
 * transcript is rebuilt to {summary, retained messages}; *out (may be NULL)
 * receives the owned result — ownership transfers ONLY on PI_OK (on any failure
 * *out is left untouched and internal allocations are released). A previous
 * compaction summary is fed back through the UPDATE_SUMMARIZATION prompt for
 * iterative updates, and the previous compaction's file-op details seed this
 * round's read/modified file lists (≙ extractFileOperations inheritance; skipped
 * when that entry carries fromHook:true, as upstream does).
 *
 * Returns PI_ERR_STATE when there is nothing to compact (empty branch or the
 * leaf is already a compaction) or while the agent is running; PI_ERR_TRANSPORT
 * when the summarization LLM call fails (session and transcript are left
 * untouched). A failure while rebuilding/installing the transcript AFTER the
 * compaction entry persisted is also reported (non-PI_OK, *out unfilled); the
 * entry remains durable and pi_session_restore can rebuild the transcript.
 * `custom_instructions` (optional, NULL/"" ⇒ none) is appended to the summarization
 * prompt as "\n\nAdditional focus: <text>" (≙ generateSummaryWithUsage:567-569 — the
 * landing point for /compact's argument); the turn-prefix prompt takes none, as upstream.
 * When the cut splits a turn (≙ isSplitTurn, compaction.ts:580), the
 * split turn's prefix is summarized separately with the TURN_PREFIX prompt and
 * appended to the main history summary under a "**Turn Context (split turn):**"
 * heading; this issues a second summarization LLM call.
 * `session`, `agent`, `model` and `settings` are all REQUIRED — any NULL is
 * PI_ERR_ARG, checked before anything is read or persisted. */
int pi_compact(pi_session_t *session, pi_agent_t *agent, const pi_model_t *model,
               const pi_compaction_settings_t *settings, const char *custom_instructions,
               pi_compaction_result_t *out);

/* ------------------------------------------------------- summarization retry */
/* pi_compact / pi_branch_summarize with the application-layer retry policy the
 * harness threads into its summarization calls (≙ compact(..., retry, callbacks) at
 * agent-harness.ts:758-767 and generateBranchSummary({... retry, callbacks}) at
 * :823-831, both funnelling into completeSimpleWithRetries → retryAssistantCall,
 * compaction.ts:118-138).
 *
 * `retry` NULL — which is what the plain pi_compact / pi_branch_summarize pass — is
 * upstream's default: `retry?: RetryPolicy` is optional all the way down and
 * retryAssistantCall then computes zero attempts, so the first response is returned
 * unchanged. Applications opt in (pi's own coding-agent defaults to enabled/3/2000 ms
 * at its settings layer, not in the harness). `cb` may be NULL.
 *
 * Retry applies per summarization CALL: a split-turn compaction issues two, each with
 * its own budget, and only the surviving attempt's usage is billed (upstream discards
 * a failed attempt's usage the same way). Classification is
 * pi_is_retryable_assistant_error, so a quota/billing error fails immediately while a
 * transient "overloaded"/"socket hang up"/"terminated" error is retried. Retry needs
 * env->sys->sleep_ms for the backoff; without it the budget collapses to zero.
 * Everything else — including "an LLM failure appends NO session entry" — is unchanged:
 * once the budget is exhausted the final error still yields PI_ERR_TRANSPORT with the
 * session and transcript untouched. */
int pi_compact_with_retry(pi_session_t *session, pi_agent_t *agent, const pi_model_t *model,
                          const pi_compaction_settings_t *settings,
                          const char *custom_instructions, const pi_retry_policy_t *retry,
                          const pi_retry_callbacks_t *cb, pi_compaction_result_t *out);

/* ---------------------------------------------------------- branch summary */
/* Result of a branch summarization + navigation (≙ generateBranchSummary +
 * Session.moveTo). `summary` is owned (release with pi_branch_summary_result_free);
 * NULL when the abandoned branch had no summarizable content. */
typedef struct pi_branch_summary_result {
    char *summary;   /* generated branch summary (preamble + LLM text + file-op tags) */
    bool cancelled;  /* target already the current leaf: navigation was a no-op */
    pi_usage_t usage; /* usage of the summarization call; all-zero when none was made */
} pi_branch_summary_result_t;

void pi_branch_summary_result_free(const pi_alloc_t *a, pi_branch_summary_result_t *r);

/* Summarize the branch being abandoned when navigating to `target_id`, then move
 * the session leaf there (≙ agent-harness navigateTree: collectEntriesForBranchSummary
 * + generateBranchSummary + moveTo). The entries between the current leaf and the
 * deepest common ancestor of leaf and target are projected to messages (toolResults
 * excluded, nested branch/compaction summaries included), serialized, and summarized
 * with model + the BRANCH_SUMMARY prompt; the result is stored as a branch_summary
 * entry parented on `target_id` (pi_session_move_to). The agent's in-memory transcript
 * is rebuilt from the new leaf. `custom_instructions` (optional) appends
 * "\n\nAdditional focus: <text>" to the prompt.
 * When the abandoned span holds entries but NONE of them project to a message (only
 * toolResults/meta), a branch_summary entry is still written carrying the literal
 * placeholder "No content to summarize" and empty file lists, with no LLM call
 * (≙ branch-summarization.ts:222-223 + the raw-count gate in agent-harness.ts:820).
 * Only a genuinely empty span navigates without any branch_summary entry, leaving
 * *out->summary NULL.
 *
 * `settings` is REQUIRED (NULL ⇒ PI_ERR_ARG) even though this call never consults
 * the threshold: it reads reserve_tokens to size the summarization request. Pass
 * pi_compaction_settings_default() if you have no opinion. Same for pi_compact.
 *
 * Returns PI_OK on success (out, may be NULL, receives the owned result;
 * ownership transfers only on PI_OK); PI_ERR_NOT_FOUND when target_id is unknown;
 * PI_ERR_STATE while the agent is running; PI_ERR_TRANSPORT when the summarization
 * LLM call fails (session and transcript left untouched). A transcript rebuild/
 * install failure AFTER the leaf moved is reported too (the navigation and any
 * branch_summary entry remain durable; pi_session_restore can rebuild). When
 * target_id equals the current leaf — including root -> root, both NULL — the
 * call is a no-op and *out->cancelled is set. */
int pi_branch_summarize(pi_session_t *session, pi_agent_t *agent, const pi_model_t *model,
                        const char *target_id, const pi_compaction_settings_t *settings,
                        const char *custom_instructions, pi_branch_summary_result_t *out);

/* pi_branch_summarize with a retry policy; see pi_compact_with_retry for the semantics.
 * The no-LLM placeholder path ("No content to summarize") never retries — there is no
 * call to retry. */
int pi_branch_summarize_with_retry(pi_session_t *session, pi_agent_t *agent,
                                   const pi_model_t *model, const char *target_id,
                                   const pi_compaction_settings_t *settings,
                                   const char *custom_instructions,
                                   const pi_retry_policy_t *retry,
                                   const pi_retry_callbacks_t *cb,
                                   pi_branch_summary_result_t *out);

#ifdef __cplusplus
}
#endif
#endif /* PI_COMPACTION_H */
