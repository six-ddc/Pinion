/* pi-c — in-process multi-threaded subagent pool + LLM tool bindings.
 *
 * This module is a pi-c native superset: upstream pi ships subagents only as a
 * child-process example extension (coding-agent examples/extensions/subagent),
 * not as part of pi-agent-core. pi-c, being an embeddable C library, provides
 * an in-process engine instead; the spawn/send/wait/close lifecycle protocol is
 * cross-referenced against openai/codex's multi_agents tool surface (Apache-2.0;
 * no source was copied). Registered in DESIGN.md §12 (#85–#88).
 *
 * Two layers:
 *   1. Engine (pi_subagent_pool_t): a thread-safe registry of child agents,
 *      each running on its own worker thread. The embedder controls everything
 *      through pool-config callbacks: `configure` builds each child's
 *      pi_agent_config_t (model, tools, system prompt, hooks — full control),
 *      `approve` gates who may spawn what, `on_child_event` taps child events.
 *   2. Tool bindings: pi_subagent_make_*_tool() wrap the engine as four
 *      pi_agent_tool_t values (spawn_agent / send_input / wait_agent /
 *      close_agent) to hand to a parent agent. Embedders may use either layer
 *      alone, or write their own tools over the engine.
 *
 * Threading account: every live child costs one worker thread plus, while a
 * request is in flight, the transport's per-connection thread (curl port) —
 * budget ≈ 2 threads per concurrently-running child.
 *
 * SPDX-License-Identifier: MIT */
#ifndef PI_SUBAGENT_H
#define PI_SUBAGENT_H

#include "pi_agent.h"
#include "pi_features.h"

#if PI_FEATURE_SUBAGENT

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pi_subagent_pool pi_subagent_pool_t;

typedef enum {
    PI_SUBAGENT_NOT_FOUND = 0, /* unknown or already-closed id (final for wait) */
    PI_SUBAGENT_RUNNING,       /* a task is queued or executing (not final) */
    PI_SUBAGENT_CLOSING,       /* close in progress on another thread (not final;
                                * becomes NOT_FOUND when the close returns) */
    PI_SUBAGENT_COMPLETED,     /* last run finished normally (final; the agent
                                * stays alive — send starts a new task on the
                                * same transcript) */
    PI_SUBAGENT_ERRORED,       /* last run failed: the library call returned
                                * non-PI_OK, or the assistant message carries
                                * stop_reason == PI_STOP_ERROR (final; agent
                                * alive, send may retry) */
    PI_SUBAGENT_ABORTED        /* last run ended via the abort latch (final) */
} pi_subagent_status_t;

/* Spawn request as seen by the approve/configure callbacks. All pointers are
 * borrowed and valid only for the duration of the callback. */
typedef struct pi_subagent_spawn_req {
    const char *task;       /* initial user prompt (never NULL/empty) */
    const char *agent_type; /* role selector; NULL = default */
    unsigned depth;         /* the CHILD's depth (= pool depth + 1); read-only */
} pi_subagent_spawn_req_t;

typedef struct pi_subagent_pool_config {
    /* Required. Supplies the pool's allocator and pi_sys_t; must outlive the
     * pool. The sys vtable must provide mutex_create/lock/unlock/destroy,
     * thread_spawn, thread_join, sem_create/post/wait/destroy, now_ms and
     * sleep_ms — pi_subagent_pool_create returns PI_ERR_STATE when any is
     * missing. (Deliberately NOT a silent sequential fallback like parallel
     * tool execution: a handle-based spawn/wait API has no meaningful
     * single-threaded degradation.) thread_spawn_ex is used when present so
     * child_stack_bytes can take effect; without it the hint is dropped —
     * see the slot contract in pi_port.h. The allocator MUST be thread-safe:
     * children allocate concurrently on their worker threads. */
    pi_env_t *env;

    size_t max_children;  /* live-slot cap (finished children count until
                           * closed); 0 ⇒ 4 */
    unsigned depth;       /* nesting level of the agent that owns this pool;
                           * root = 0 */
    unsigned max_depth;   /* spawn is refused when depth+1 > max_depth;
                           * 0 ⇒ 1 (children may not spawn grandchildren) */
    size_t result_cap_bytes; /* cap on the stored per-run final text (the copy
                              * handed to wait/result_copy); truncation is
                              * UTF-8-boundary safe and appends "\n[truncated]".
                              * 0 ⇒ 50*1024 (mirrors the upstream subagent
                              * extension's per-task output cap). */
    uint32_t wait_slice_ms;  /* wait() poll slice; 0 ⇒ 100 */
    size_t child_stack_bytes; /* worker-thread stack hint, forwarded through
                               * pi_sys_t.thread_spawn_ex. 0 ⇒ port default.
                               * A whole agent loop needs ~32 KiB (see
                               * tests/test_stack_budget.c); FreeRTOS ports must
                               * receive at least that (their port clamps up to
                               * PI_WORKER_STACK otherwise). */

    /* ---- embedder control callbacks. All of them run on the thread that
     * calls pi_subagent_spawn/close/pool_destroy (when driven by the LLM tools
     * that is the parent agent's loop thread), never under the pool lock.
     * None of them may re-enter this pool (deadlock/undefined order). ---- */

    /* Required. Build the child's agent config: *cfg arrives zeroed; fill it
     * per the pi_agent_create contract. Every pointer stored in *cfg is
     * borrowed and must stay valid until this child's cleanup() runs.
     * cfg->on_event/event_user may be set: the pool chains its own bridge in
     * front and both fire (on the child's worker thread — see on_child_event).
     * *out_child_user is an opaque per-child cookie handed back to cleanup()
     * (tool contexts, a dedicated env, ...); may be left NULL. Recommended
     * memory pattern: build the child a dedicated arena-backed env here (the
     * cookie owns it) and destroy it wholesale in cleanup() — see
     * examples/host_coder/sub_arena.c and DESIGN §12 #85. Return PI_OK,
     * or any error to fail the spawn with that code (anything you allocated
     * must already be released — cleanup() is NOT called on a configure
     * failure). */
    int (*configure)(pi_subagent_pool_t *pool, const pi_subagent_spawn_req_t *req,
                     pi_agent_config_t *cfg, void **out_child_user, void *user);

    /* Optional. Runs after pi_agent_destroy(child), before the slot is
     * released — free whatever configure() attached. */
    void (*cleanup)(pi_subagent_pool_t *pool, uint32_t child_id, void *child_user,
                    void *user);

    /* Optional spawn gate. Return false to refuse; *out_reason may be set to
     * a string allocated with the POOL env allocator (ownership passes to the
     * pool/caller, see pi_subagent_spawn). NULL callback ⇒ every spawn is
     * allowed (capacity/depth checks still apply). */
    bool (*approve)(pi_subagent_pool_t *pool, const pi_subagent_spawn_req_t *req,
                    char **out_reason, void *user);

    /* Optional child-event tap. THREAD CONTEXT: fires on the CHILD's worker
     * thread — serialized per child, concurrent across children. Pointers
     * inside *ev are borrowed and valid only for the call (same contract as
     * pi_agent_config_t.on_event). Return promptly: blocking here stalls that
     * child's loop (parent and siblings are unaffected). Marshalling to a UI
     * thread is the embedder's job (see the self-pipe pattern in
     * examples/host_coder). NULL ⇒ no forwarding. */
    void (*on_child_event)(pi_subagent_pool_t *pool, uint32_t child_id,
                           const pi_agent_event_t *ev, void *user);

    void *user; /* passed to every callback */
} pi_subagent_pool_config_t;

/* Create a pool. On success *out is set and PI_OK returned. PI_ERR_ARG when
 * cfg/env/configure is missing, PI_ERR_STATE when the sys vtable lacks a
 * required primitive, PI_ERR_NOMEM on allocation failure. On any failure *out
 * is untouched and no partial state exists. */
int pi_subagent_pool_create(const pi_subagent_pool_config_t *cfg,
                            pi_subagent_pool_t **out);

/* Destroy the pool: aborts every live child, joins every worker, destroys
 * every child agent, runs cleanup() for each, then frees the pool. BLOCKS
 * until all workers exit (abort latency is bounded by the transport's abort
 * poll, ~1s on the curl port). Precondition — same discipline as
 * pi_agent_destroy: no other thread may be inside any API of this pool, and a
 * parent run driving the bound tools must have returned. NULL-safe. */
void pi_subagent_pool_destroy(pi_subagent_pool_t *pool);

/* Spawn a child. Thread-safe. On success *out_id receives a non-zero id that
 * is unique for the pool's lifetime (monotonic, never reused).
 * Errors: PI_ERR_LIMIT   capacity full or depth exceeded (distinguish with
 *                        pi_subagent_available());
 *         PI_ERR_STATE   refused by approve() — *out_reason (when non-NULL)
 *                        may carry a reason string owned by the caller, free
 *                        it with the pool env allocator — or the worker
 *                        thread could not be started (no reason string;
 *                        deliberately NOT degraded to inline execution);
 *         PI_ERR_NOMEM / configure()'s code: fully rolled back, no slot
 *                        consumed (cleanup() ran iff configure() succeeded).
 * agent_type and out_reason may be NULL. */
int pi_subagent_spawn(pi_subagent_pool_t *pool, const char *task,
                      const char *agent_type, uint32_t *out_id, char **out_reason);

/* Send text to a child. Thread-safe.
 * - RUNNING child: interrupt=true steers (pi_agent_steer — injected before the
 *   next turn), false queues a follow-up (pi_agent_follow_up).
 * - COMPLETED/ERRORED/ABORTED child: the text becomes a NEW task on the same
 *   transcript; the child returns to RUNNING.
 * Delivery near the end of a run is at-least-once but may slide to the next
 * run (the run being steered can finish before draining the injection — the
 * text then stays queued and is drained by the child's next run; same
 * best-effort family as the abort latch, pi_agent.h). Errors: PI_ERR_NOT_FOUND
 * unknown/closing/closed id; PI_ERR_ARG empty text; PI_ERR_NOMEM queue
 * unchanged. */
int pi_subagent_send(pi_subagent_pool_t *pool, uint32_t id, const char *text,
                     bool interrupt);

typedef struct pi_subagent_wait_item {
    uint32_t id;
    pi_subagent_status_t status;
} pi_subagent_wait_item_t;

typedef struct pi_subagent_wait_result {
    pi_subagent_wait_item_t *items; /* allocated with the pool env allocator;
                                     * release via pi_subagent_wait_result_free.
                                     * Contains every listed id whose status is
                                     * final at return time. On item-array OOM
                                     * the wait still succeeds with count == 0
                                     * and the flags below valid. */
    size_t count;
    bool timed_out; /* deadline hit before any listed child turned final */
    bool aborted;   /* *abort turned true first */
} pi_subagent_wait_result_t;

/* Block until ANY of ids[0..n) reaches a final status (COMPLETED / ERRORED /
 * ABORTED / NOT_FOUND), then report every listed id that is final at that
 * moment. Implementation is a poll: pool-lock scan + sys->sleep_ms slice
 * (wait_slice_ms); no dedicated wakeup primitive, so the detection latency
 * upper bound is one slice. timeout_ms 0 ⇒ 30000, clamped to
 * [wait_slice_ms, 600000]. `abort` may be NULL; the LLM wait tool passes the
 * parent run's abort flag so a parent abort interrupts the wait (children keep
 * running — cancelling them is a policy call, see pi_subagent_abort_all).
 * Returns PI_ERR_ARG for ids==NULL/n==0/out==NULL, else PI_OK. */
int pi_subagent_wait(pi_subagent_pool_t *pool, const uint32_t *ids, size_t n,
                     uint32_t timeout_ms, const volatile bool *abort,
                     pi_subagent_wait_result_t *out);
void pi_subagent_wait_result_free(pi_subagent_pool_t *pool,
                                  pi_subagent_wait_result_t *r);

/* Thread-safe status snapshot (may be stale the instant it returns). */
pi_subagent_status_t pi_subagent_status(pi_subagent_pool_t *pool, uint32_t id);

/* Copy of the child's stored final text (captured at the end of each run from
 * the last assistant message's first text block, capped at result_cap_bytes).
 * alloc NULL ⇒ pool env allocator; free the copy with the SAME allocator.
 * NULL = no result yet / unknown id / OOM (not distinguished). */
char *pi_subagent_result_copy(pi_subagent_pool_t *pool, uint32_t id,
                              const pi_alloc_t *alloc);
/* Copy of the last run's error detail (provider error_message or a library
 * error description); NULL when the last run did not error. */
char *pi_subagent_error_copy(pi_subagent_pool_t *pool, uint32_t id,
                             const pi_alloc_t *alloc);

/* Close a child: abort + join + destroy + cleanup; the slot is released and
 * the id reads NOT_FOUND afterwards. BLOCKS like pool_destroy (one child):
 * the latency bound is one wait_slice_ms plus the transport's abort poll —
 * the pool re-asserts the abort latch each slice until the worker exits,
 * because a run that starts concurrently with the close resets the latch at
 * its entry and would otherwise swallow a single abort.
 * *out_prev (may be NULL) receives the status observed before shutdown was
 * requested. Errors: PI_ERR_NOT_FOUND unknown id; PI_ERR_STATE another close
 * of the same id is in flight. */
int pi_subagent_close(pi_subagent_pool_t *pool, uint32_t id,
                      pi_subagent_status_t *out_prev);

/* Set every live child's abort latch (single boolean store each; non-blocking,
 * signal-safe on POSIX). Policy helper for "drop everything": a parent abort
 * does NOT propagate to children automatically. */
void pi_subagent_abort_all(pi_subagent_pool_t *pool);

size_t pi_subagent_count(pi_subagent_pool_t *pool);     /* live children */
size_t pi_subagent_available(pi_subagent_pool_t *pool); /* free slots */
/* Fill ids[0..cap) with live child ids; returns the TOTAL live count (which
 * may exceed cap). */
size_t pi_subagent_list(pi_subagent_pool_t *pool, uint32_t *ids, size_t cap);

/* ---------------- LLM tool bindings ----------------
 * Makers follow the harness maker convention (pi_harness_make_bash_tool):
 * the tool is returned by value, `.user` points at an env-allocated context;
 * `.user == NULL` is the legal OOM-degraded state where execute reports an
 * is_error result instead of acting. `env` and `pool` must outlive the tool;
 * release with pi_subagent_tool_dispose (safe on degraded tools). Each
 * execute is a blocking call on the parent loop's executing thread; the wait
 * tool polls the parent run's abort flag every slice. */

typedef struct pi_subagent_tool_opts {
    const char *agent_type_guide; /* embedder's role catalogue text, spliced
                                   * verbatim into the spawn tool's agent_type
                                   * description; borrowed, must outlive the
                                   * tool. NULL ⇒ the spawn schema exposes no
                                   * agent_type property at all. */
    uint32_t default_wait_ms;     /* wait tool default timeout; 0 ⇒ 30000 */
    uint32_t max_wait_ms;         /* wait tool timeout cap; 0 ⇒ 600000 */
} pi_subagent_tool_opts_t;

pi_agent_tool_t pi_subagent_make_spawn_tool(pi_env_t *env, pi_subagent_pool_t *pool,
                                            const pi_subagent_tool_opts_t *opts);
pi_agent_tool_t pi_subagent_make_send_tool(pi_env_t *env, pi_subagent_pool_t *pool);
pi_agent_tool_t pi_subagent_make_wait_tool(pi_env_t *env, pi_subagent_pool_t *pool,
                                           const pi_subagent_tool_opts_t *opts);
pi_agent_tool_t pi_subagent_make_close_tool(pi_env_t *env, pi_subagent_pool_t *pool);
void pi_subagent_tool_dispose(pi_env_t *env, pi_agent_tool_t *tool);

#ifdef __cplusplus
}
#endif

#endif /* PI_FEATURE_SUBAGENT */
#endif /* PI_SUBAGENT_H */
