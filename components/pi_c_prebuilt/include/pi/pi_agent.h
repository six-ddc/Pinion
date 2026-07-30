/* pi-c — agent layer (≙ @earendil-works/pi-agent-core): loop, tools, steering.
 * Control flow mirrors ts: packages/agent/src/agent-loop.ts (runLoop, sequential path);
 * handle API mirrors ts: packages/agent/src/agent.ts (Agent class).
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_AGENT_H
#define PI_AGENT_H

#include "cJSON.h" /* tools receive parsed arguments (public dep, DESIGN.md §16) */
#include "pi_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pi_agent pi_agent_t;

/* Agent-layer error code: the max_turns fuse tripped (the run exceeded
 * pi_agent_config.max_turns before finishing; the transcript keeps everything
 * produced so far). Defined here — not in the core PI_ERR_* block (pi_port.h,
 * -1..-5) — and numbered far away from it so the two ranges can grow
 * independently without collisions. Since R10 the meaning is broadened from
 * "max_turns fuse" to "a configured limit refused the operation": the subagent
 * pool returns it for capacity-full and depth-exceeded spawns (pi_subagent.h). */
#define PI_ERR_LIMIT (-100)

/* ---------- tool execution (≙ AgentTool / AgentToolResult) ---------- */

/* ≙ an ImageContent block inside AgentToolResult.content (types.ts:355-357).
 * Both strings are owned by the result and allocated with the env allocator. */
typedef struct pi_tool_image {
    char *data; /* base64 payload, no data-URI prefix */
    char *mime; /* "image/jpeg" | "image/png" | "image/gif" | "image/webp" */
} pi_tool_image_t;

typedef struct pi_tool_result {
    char *output;   /* allocated with the env allocator; loop takes ownership */
    bool is_error;
    bool terminate; /* pi semantics: batch stops only if ALL results set it */
    /* ≙ the ImageContent entries of AgentToolResult.content: upstream models a tool
     * result as an ordered (TextContent|ImageContent)[]; pi-c keeps `output` as the
     * text part and carries the images here, in order, as a TAIL-APPENDED optional
     * array — a zero-initialized result (every existing tool) has none and behaves
     * exactly as before. Fill it with pi_tool_result_add_image(); the loop takes
     * ownership together with `output` and moves the images onto the tool-result
     * message as PI_BLOCK_IMAGE blocks.
     * Only consumed when the library is built with PI_FEATURE_IMAGES — an IMAGES-off
     * build ignores the array (the tool's text still goes out unchanged), so a tool
     * that wants a graceful degradation must say so in `output` itself. */
    pi_tool_image_t *images;
    size_t image_count;
} pi_tool_result_t;

/* Append one image to a tool result, copying both strings with `alloc` (the same
 * allocator the tool got in execute()). mime == NULL defaults to "image/jpeg", like
 * pi_message_add_image. Returns PI_OK, PI_ERR_ARG (res == NULL / no data) or
 * PI_ERR_NOMEM (the result is left unchanged). */
int pi_tool_result_add_image(const pi_alloc_t *alloc, pi_tool_result_t *res,
                             const char *base64_data, const char *mime);
/* Release the image array. The loop calls this for every result it finalizes, so a
 * tool only needs it to unwind its own error paths. */
void pi_tool_result_free_images(const pi_alloc_t *alloc, pi_tool_result_t *res);

typedef void (*pi_tool_update_cb)(const char *partial_text, void *user);

/* Per-tool execution mode (≙ AgentTool.execution / ToolExecutionMode, types.ts:41).
 * PI_TOOL_EXEC_DEFAULT follows the batch policy (pi_agent_config.tool_execution);
 * PI_TOOL_EXEC_SEQUENTIAL pins the whole batch sequential when this tool is called
 * (≙ hasSequentialToolCall, agent-loop.ts:381). Parallel execution requires the
 * PI_FEATURE_PARALLEL_TOOLS build; without it both values behave sequentially. */
typedef enum { PI_TOOL_EXEC_DEFAULT = 0, PI_TOOL_EXEC_SEQUENTIAL } pi_tool_exec_mode_t;

typedef struct pi_agent_tool {
    /* def.parameters_schema_json is validated by the built-in JSON-Schema checker
     * (src/agent/pi_validate.c, ≙ validation.ts): type/required/properties/items/
     * additionalProperties, allOf/anyOf/oneOf, enum/const, and the constraint
     * keywords minimum/maximum/exclusiveMinimum/exclusiveMaximum/multipleOf,
     * minLength/maxLength, minItems/maxItems/uniqueItems. `pattern` is NOT
     * enforced (no regex engine in the embedded core) — it is silently skipped;
     * do not rely on it as a security boundary. minLength/maxLength count Unicode
     * code points (upstream JS counts UTF-16 units; differs only for astral-plane
     * characters). */
    pi_tool_def_t def;
    const char *label; /* optional display label (≙ AgentTool.label) */
    /* Return PI_OK and fill *out (out->output allocated with `alloc`).
     * Non-PI_OK ⇒ loop records an is_error tool result with a generic message. */
    int (*execute)(const pi_alloc_t *alloc, const char *tool_call_id, const cJSON *args,
                   volatile bool *abort_flag, pi_tool_update_cb on_update, void *update_user,
                   void *tool_user, pi_tool_result_t *out);
    /* Optional: mutate the parsed cJSON args in place before schema validation
     * (≙ AgentTool.prepareArguments, types.ts:378; runs before validation and
     * before_tool_call). Receives the tool's `user`. NULL ⇒ skipped. */
    void (*prepare_arguments)(cJSON *args, void *tool_user);
    pi_tool_exec_mode_t execution_mode; /* PI_TOOL_EXEC_SEQUENTIAL pins any batch
                                         * containing this tool to sequential
                                         * execution even under a PARALLEL batch
                                         * policy (see pi_tool_exec_mode_t above) */
    void *user;
} pi_agent_tool_t;

/* ---------- agent events (names ≙ AgentEvent, ts: agent/src/types.ts:413) ---------- */
typedef enum {
    PI_AG_EV_AGENT_START,
    PI_AG_EV_TURN_START,
    PI_AG_EV_MESSAGE_START,
    PI_AG_EV_MESSAGE_UPDATE, /* wraps an ai-layer CONTENT event (field `ai`): only the
                              * 9 text/thinking/toolcall × start/delta/end kinds.
                              * START maps to MESSAGE_START, DONE/ERROR to MESSAGE_END
                              * — none of them produce a MESSAGE_UPDATE (≙ ts:314-355) */
    PI_AG_EV_MESSAGE_END,
    PI_AG_EV_TOOL_EXECUTION_START,
    PI_AG_EV_TOOL_EXECUTION_UPDATE,
    PI_AG_EV_TOOL_EXECUTION_END,
    PI_AG_EV_TURN_END,
    PI_AG_EV_AGENT_END
} pi_agent_event_kind_t;

typedef struct pi_agent_event {
    pi_agent_event_kind_t kind;
    size_t turn;                  /* 0-based turn counter */
    const pi_message_t *message;  /* MESSAGE_* / TOOL_EXECUTION_END (tool result msg)
                                   * / TURN_END (the turn's assistant message).
                                   * May be NULL on OOM for TOOL_EXECUTION_END (the
                                   * tool-result message could not be allocated or
                                   * pushed; tool_result still carries the outcome)
                                   * and for TURN_END (the assistant message could
                                   * not be synthesized after a stream failure). */
    const pi_ai_event_t *ai;      /* MESSAGE_UPDATE only */
    const char *tool_name;        /* TOOL_EXECUTION_* */
    const char *tool_call_id;     /* TOOL_EXECUTION_* */
    const cJSON *tool_args;       /* TOOL_EXECUTION_START/UPDATE/END (parsed args) */
    const char *update_text;      /* TOOL_EXECUTION_UPDATE only */
    const pi_tool_result_t *tool_result; /* TOOL_EXECUTION_END only */
    /* TURN_END payload (≙ pi turn_end carrying message + toolResults[]):
     * transcript slice of the tool-result messages produced this turn. */
    const pi_message_t *const *turn_tool_results;
    size_t turn_tool_result_count;
} pi_agent_event_t;

typedef void (*pi_agent_event_cb)(const pi_agent_event_t *ev, void *user);

/* ---------- hooks (≙ AgentLoopConfig callbacks, trimmed to the used set) ----------
 * CONTRACT for every hook below (≙ upstream's per-hook "must not throw or reject"):
 *  - must RETURN NORMALLY: no longjmp/abort/exception out of the loop. Escaping leaks
 *    this turn's transform view, the parsed argument tree and the tool result, and
 *    breaks the event sequence (no TURN_END/AGENT_END).
 *  - must not re-enter the agent: pi_agent_prompt/pi_agent_continue refuse with
 *    PI_ERR_STATE while running, and pi_agent_destroy on a running agent is UB.
 *  - should return promptly: a hook is called on the loop thread, so blocking in it
 *    delays the abort latch from taking effect (poll pi_agent_aborted() to bail out). */
typedef struct pi_agent_hooks {
    /* May mutate args in place; the mutated args are NOT re-validated (validation runs
     * before this hook). Return false to veto: the loop records an is_error tool result
     * without executing (≙ beforeToolCall).
     * *out_reason is an optional block reason (≙ BeforeToolCallResult.reason): assign a
     * string allocated with the env allocator (pi_env_alloc) and the loop forwards it to
     * the model as the tool result, then frees it. Leaving it NULL (or assigning "")
     * keeps the default wording "Tool execution was blocked" (≙ upstream `reason ||
     * ...`); when the abort latch is already set the result reads "Operation aborted"
     * regardless. A reason assigned while returning true is freed unused. */
    bool (*before_tool_call)(pi_agent_t *ag, const char *tool_name, cJSON *args,
                             char **out_reason, void *user);
    /* May mutate the result, incl. setting terminate (≙ afterToolCall). */
    void (*after_tool_call)(pi_agent_t *ag, const char *tool_name, pi_tool_result_t *result,
                            void *user);
    /* Checked after each turn (≙ shouldStopAfterTurn). */
    bool (*should_stop_after_turn)(pi_agent_t *ag, void *user);

    /* Non-destructive context transform before each LLM request (≙ transformContext,
     * agent-loop.ts:284). On entry *view is a temporary, loop-owned array of *count
     * pointers into the transcript (or into a prepare_next_turn messages override).
     * The hook may:
     *  - prune / reorder in place and SHRINK *count (pruning; *count must not grow
     *    while *view is the loop's array — the loop clamps it back to the input length,
     *    since a larger value would read past the allocation);
     *  - replace individual slots with messages of its own;
     *  - replace *view wholesale with its OWN array of any length and set *count to
     *    match (this is how context is INJECTED from external sources, ≙ upstream
     *    returning a brand-new array). Such an array (and the messages in it) is
     *    borrowed: it must stay valid until the request completes and is never freed
     *    by the loop.
     * The hook must NOT free the transcript's messages. The loop builds the request
     * from the resulting view, then discards the view; the transcript is untouched.
     * DEGRADATION: if the loop cannot allocate the view it skips the hook and sends the
     * FULL message list — a pruning hook must therefore not be relied on as a hard
     * context-window guarantee. NULL ⇒ disabled. */
    void (*transform_context)(pi_agent_t *ag, pi_message_t ***view, size_t *count, void *user);

    /* Called after each turn_end, before the NEXT request of this run — never before
     * the first request (≙ prepareNextTurn, agent-loop.ts:226). May overwrite *model
     * and/or *thinking to hot-swap them for subsequent requests; leave them untouched
     * to keep the current values. NULL ⇒ disabled.
     *
     * *context is the request-context replacement slot (≙ AgentLoopTurnUpdate.context,
     * types.ts:133): point it at a pi_context_t to swap system_prompt / messages /
     * tools for every subsequent request of this run. Per-field opt-in — a NULL field
     * inside the snapshot keeps the agent's own value (system_prompt from the config,
     * tools from the tool table, messages from the live transcript). A messages
     * override is a FIXED snapshot: the loop appends new messages to its transcript,
     * not to the override, so a hook that wants to keep growing the request must set a
     * fresh snapshot each turn. transform_context still runs on top of the result.
     * LIFETIME: both a swapped-in *model and *context (plus everything they point at)
     * are only borrowed — they must stay valid until the run returns or the hook
     * replaces them; the loop never copies them. */
    void (*prepare_next_turn)(pi_agent_t *ag, const pi_model_t **model, pi_thinking_t *thinking,
                              const pi_context_t **context, void *user);

    /* Resolve an API key per request (≙ getApiKey, agent-loop.ts:301). `provider_id`
     * identifies the endpoint. A non-NULL, NON-EMPTY return overrides
     * stream_opts.api_key for that request (useful for short-lived tokens); NULL or ""
     * keeps the configured key (≙ upstream `|| config.apiKey`, agent-loop.ts:305).
     * LIFETIME: the returned string is borrowed, never freed by the loop, and must
     * stay valid until the current request completes — return a stable/static buffer,
     * not a temporary. NOTE the plural: when the same hook serves several agents
     * (e.g. a subagent pool), it is called concurrently from every loop's thread, and
     * "the current request" means EVERY in-flight request. "Free the previous token on
     * the next call" is only sound for a single loop; a shared hook must be internally
     * synchronized and keep every pointer it ever returned alive until all loops are
     * torn down (see host_coder's coder_token_cache_t for the reference pattern). */
    const char *(*get_api_key)(pi_agent_t *ag, const char *provider_id, void *user);

    void *user;
} pi_agent_hooks_t;

/* Queue drain policy (≙ QueueMode, agent/src/types.ts:49). Default (0) is
 * one-at-a-time: each drain point injects the single oldest queued message. */
typedef enum { PI_QUEUE_ONE_AT_A_TIME = 0, PI_QUEUE_ALL } pi_queue_mode_t;

/* Tool-batch execution policy (≙ ToolExecutionMode, agent/src/types.ts:41).
 * Default (0) is sequential — pi-c keeps the embedded-friendly single-thread
 * default even though upstream defaults to "parallel" (DESIGN.md deviation #4).
 * PI_TOOL_EXECUTION_PARALLEL runs a batch's tools concurrently, but only when the
 * library was built with PI_FEATURE_PARALLEL_TOOLS and env->sys supplies the
 * threading primitives; otherwise it silently degrades to sequential. */
typedef enum {
    PI_TOOL_EXECUTION_SEQUENTIAL = 0,
    PI_TOOL_EXECUTION_PARALLEL
} pi_tool_execution_t;

/* ---------- config / lifecycle ---------- */
typedef struct pi_agent_config {
    pi_env_t *env; /* required; must outlive the agent */
    pi_model_t model;
    const char *system_prompt; /* copied */
    const pi_agent_tool_t *tools; /* array copied (shallow); defs must outlive agent */
    size_t tool_count;
    /* api_key etc.; abort_flag is managed internally. The struct itself is copied,
     * but every pointer inside it (api_key, extra_headers, ...) is borrowed and must
     * outlive the agent — the loop reuses them verbatim on every request. */
    pi_stream_options_t stream_opts;
    pi_agent_event_cb on_event;
    void *event_user;
    pi_agent_hooks_t hooks;
    int max_turns; /* fuse; 0 ⇒ unlimited (≙ pi's uncapped loop). MIGRATION: 0 once
                    * meant "default 16"; it now means no limit. Set an explicit value
                    * (e.g. PI_AGENT_DEFAULT_MAX_TURNS) to keep a cap. A tripped fuse
                    * makes the run return PI_ERR_LIMIT (was PI_ERR_STATE). */
    /* Queue drain policy (≙ steeringMode / followUpMode, agent.ts:222-223). Default 0
     * (PI_QUEUE_ONE_AT_A_TIME) preserves pi's default behavior. PI_QUEUE_ALL drains
     * the whole queue at each drain point, injecting every message. */
    pi_queue_mode_t steering_mode;
    pi_queue_mode_t follow_up_mode;
    /* Tool-batch execution policy (≙ toolExecution, agent-loop.ts:384). Default 0
     * (PI_TOOL_EXECUTION_SEQUENTIAL). See pi_tool_execution_t. */
    pi_tool_execution_t tool_execution;
} pi_agent_config_t;

/* NULL on invalid cfg (missing env) or allocation failure — the two are not
 * distinguished (constructor returns a pointer, not a pi error code). */
pi_agent_t *pi_agent_create(const pi_agent_config_t *cfg);
/* Destroy an IDLE agent. Calling this while a run (prompt/continue) is still in
 * progress on another thread is forbidden/undefined behavior — the loop thread
 * keeps dereferencing the handle. As a cheap defense the call refuses (returns
 * without freeing, leaking the agent) when it can see the run flag set; do not
 * rely on that: abort() + wait for the run call to return, then destroy. */
void pi_agent_destroy(pi_agent_t *ag);

/* Blocking: append user message and run the loop to completion (≙ agent.prompt()). */
int pi_agent_prompt(pi_agent_t *ag, const char *user_text);
/* Blocking: resume from current transcript state (≙ agent.continue()). */
int pi_agent_continue(pi_agent_t *ag);

/* Thread-safe (require env->sys when called cross-thread): */
int pi_agent_steer(pi_agent_t *ag, const char *text);     /* injected before next turn */
int pi_agent_follow_up(pi_agent_t *ag, const char *text); /* queued after inner loop ends */
/* Set the abort latch of the CURRENT run (≙ Agent.abort() aborting activeRun).
 * Semantics/guarantees:
 *  - only affects a run in progress; calling while idle is a no-op — the latch is
 *    cleared at pi_agent_prompt()/pi_agent_continue() entry, so an abort issued
 *    before the next run does not carry over into it. An abort racing that entry
 *    (called from another thread in the same instants prompt()/continue() begins)
 *    may land on either side of the reset; callers who need a hard guarantee must
 *    order abort vs. start themselves.
 *  - monotonic within a run: once set it stays set until the run ends.
 *  - best-effort latency: the loop and tools poll the flag, so cancellation can
 *    take effect up to one event/turn late. No locks/fences are used; correctness
 *    relies only on single-byte load/store atomicity (holds on all supported
 *    targets, including SMP FreeRTOS). */
void pi_agent_abort(pi_agent_t *ag);
/* Read the abort latch of the current run. This is the read side the four hooks
 * (before_tool_call / after_tool_call / transform_context / should_stop_after_turn) use
 * to honor cancellation — upstream passes them the AbortSignal itself and documents
 * that the hook "is responsible for honoring it" (types.ts:257-259); pi-c keeps the
 * signatures narrow and exposes the latch here instead (tools get it as the
 * `volatile bool *abort_flag` execute argument). Same best-effort/monotonic semantics
 * as pi_agent_abort(); safe to call from any thread. */
bool pi_agent_aborted(const pi_agent_t *ag);

/* Queue introspection/control (≙ clearSteeringQueue/clearFollowUpQueue/
 * clearAllQueues/hasQueuedMessages, ts: agent.ts:284-302). Thread-safe.
 * NOTE: pi-c models two queues; upstream's third, harness-level nextTurn queue
 * ("inject before the NEXT prompt") is out of scope — inject with
 * pi_agent_transcript_append() while idle, or keep an application-side queue and
 * flush it before the next pi_agent_prompt(). */
size_t pi_agent_queued_steering(pi_agent_t *ag);
size_t pi_agent_queued_follow_ups(pi_agent_t *ag);
/* Peek the oldest queued message without dequeuing (≙ PendingMessageQueue head).
 * NOT thread-safe, despite sitting next to the calls above: the returned pointer is
 * borrowed from the queue node, so the loop thread's own drain can free it at any
 * moment — a cross-thread peek may dangle before the caller even dereferences it.
 * Only call these from the thread that owns the agent (while idle, or from inside a
 * hook); use the _copy variants below from any other thread. NULL if empty. */
const char *pi_agent_peek_steering(pi_agent_t *ag);
const char *pi_agent_peek_follow_up(pi_agent_t *ag);
/* Thread-safe peek: the head string is copied while the queue lock is held. `alloc`
 * may be NULL for the agent's env allocator; the caller frees the result with
 * pi_free() using the SAME allocator. NULL when the queue is empty. */
char *pi_agent_peek_steering_copy(pi_agent_t *ag, const pi_alloc_t *alloc);
char *pi_agent_peek_follow_up_copy(pi_agent_t *ag, const pi_alloc_t *alloc);
/* Drain policy, readable/writable at runtime (≙ Agent.steeringMode/followUpMode,
 * agent.ts:257-273; harness set/getSteeringMode + set/getFollowUpMode). Overrides the
 * pi_agent_config_t value; a change takes effect at the next drain point of the run.
 * Visibility: the mode is a plain enum-sized store read at the next drain point, with
 * no lock or fence — same best-effort, eventually-visible contract as the abort latch
 * in pi_agent_abort(). Correctness relies only on the store not tearing, which holds
 * for a naturally-aligned int on every supported target. A change is therefore never
 * lost, but the exact drain point it first affects is not pinned down. */
void pi_agent_set_steering_mode(pi_agent_t *ag, pi_queue_mode_t mode);
void pi_agent_set_follow_up_mode(pi_agent_t *ag, pi_queue_mode_t mode);
pi_queue_mode_t pi_agent_steering_mode(const pi_agent_t *ag);
pi_queue_mode_t pi_agent_follow_up_mode(const pi_agent_t *ag);
void pi_agent_clear_steering(pi_agent_t *ag);
void pi_agent_clear_follow_ups(pi_agent_t *ag);
void pi_agent_clear_all_queues(pi_agent_t *ag);
bool pi_agent_has_queued(pi_agent_t *ag);

/* Hook context accessors (richer context without widening hook signatures):
 * valid only while a tool batch is executing — i.e. from inside
 * before_tool_call/after_tool_call — NULL otherwise. */
const pi_message_t *pi_agent_current_assistant(const pi_agent_t *ag);
const char *pi_agent_current_tool_call_id(const pi_agent_t *ag);

/* Transcript of finalized messages; streaming partials are visible via events.
 * This matches pi's Agent layer exactly: state.messages is only pushed on
 * message_end, partials travel through state.streamingMessage (ts: agent.ts:537). */
const pi_message_t *const *pi_agent_transcript(const pi_agent_t *ag, size_t *count);
const pi_message_t *pi_agent_last_assistant(const pi_agent_t *ag);

/* ---------- message observer (persistence/session seam, DESIGN §16.5) ----------
 * A single downstream consumer notified of every message as it enters the
 * transcript (the write-side hook the SESSION layer uses to persist, but not
 * tied to SESSION — available in every build). The observer fires from inside
 * pi_agent_push_message, i.e. for prompt/continue user messages, streamed
 * assistant messages, tool-result messages, and pi_agent_transcript_append.
 * The message pointer is borrowed (owned by the transcript); do not free it or
 * hold it past the next transcript mutation. */

/* Install (or, with cb == NULL, uninstall) the observer. The seam is 1:1:
 * installing while a DIFFERENT observer — different callback OR different user —
 * already holds the slot returns PI_ERR_STATE; re-installing the identical
 * (cb, user) pair is an idempotent PI_OK; uninstalling is always PI_OK. While an
 * observer is installed pi_agent_transcript_splice refuses (PI_ERR_STATE), since
 * the transcript is being consumed append-only. Returns PI_ERR_ARG if ag is NULL. */
int pi_agent_set_message_observer(pi_agent_t *ag,
                                  void (*cb)(const pi_message_t *m, void *user), void *user);
/* Read back the currently installed observer (either out param may be NULL).
 * Used by the session layer to make detach/close only unhook its own adapter. */
void pi_agent_message_observer(const pi_agent_t *ag,
                               void (**cb)(const pi_message_t *m, void *user), void **user);

/* Application-side context pruning (DESIGN §11: overflow handling is the app's
 * decision — this is the write-side hook that makes it possible). Removes
 * messages [start, start+count) and frees them. PI_ERR_STATE while running.
 * Caller owns keeping the transcript coherent (e.g. do not orphan a tool_use
 * from its tool_result — providers synthesize placeholders, but pruning pairs
 * together is cleaner). */
int pi_agent_transcript_splice(pi_agent_t *ag, size_t start, size_t count);
/* Append a message to the transcript, taking ownership (session restore / custom
 * message injection). PI_ERR_STATE while running. Not announced via events. */
int pi_agent_transcript_append(pi_agent_t *ag, pi_message_t *msg);

#ifdef __cplusplus
}
#endif
#endif /* PI_AGENT_H */
