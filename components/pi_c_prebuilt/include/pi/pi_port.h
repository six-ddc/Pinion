/* pi-c — platform port interfaces (all DI vtables; core contains no platform code)
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_PORT_H
#define PI_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- error codes (library-call failures only; LLM/network failures are
 * reported inside the assistant message per pi semantics, see DESIGN.md §11) */
#define PI_OK 0
#define PI_ERR_ARG (-1)
#define PI_ERR_NOMEM (-2)
#define PI_ERR_TRANSPORT (-3)
#define PI_ERR_STATE (-4)
#define PI_ERR_NOT_FOUND (-5)

/* ---------- allocator ---------- */
typedef struct pi_alloc {
    void *(*malloc_fn)(size_t size, void *ctx);
    void *(*realloc_fn)(void *ptr, size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} pi_alloc_t;

/* Default allocator backed by libc malloc/realloc/free. */
const pi_alloc_t *pi_alloc_default(void);

void *pi_malloc(const pi_alloc_t *a, size_t size);
void *pi_realloc(const pi_alloc_t *a, void *ptr, size_t size);
void pi_free(const pi_alloc_t *a, void *ptr);
char *pi_strdup(const pi_alloc_t *a, const char *s);
char *pi_strndup(const pi_alloc_t *a, const char *s, size_t n);

/* ---------- streaming HTTP transport ----------
 * One blocking POST with incremental body reads. TLS, proxies, cert bundles,
 * and network-interface routing are entirely the port's business. */
typedef struct pi_transport {
    /* Open connection, send request (headers are "Key: Value" strings).
     * Returns opaque connection handle, or NULL on connect/send failure. */
    void *(*open_post)(void *ctx, const char *url, const char *const *headers,
                       size_t header_count, const char *body, size_t body_len);
    /* Read response body bytes. >0 = bytes read, 0 = EOF, <0 = error. */
    int (*read)(void *ctx, void *conn, char *buf, size_t cap);
    /* HTTP status code (valid after open_post returns non-NULL). */
    int (*status)(void *ctx, void *conn);
    void (*close)(void *ctx, void *conn);
    void *ctx;
    /* ---- appended at the tail so existing positional/zero-filled pi_transport
     * initializers stay valid (same pattern as pi_sys_t / pi_fs_t; NULL reads as
     * absent). ----
     * Optional abort injection: register `flag` on an open connection; while it
     * is non-NULL the transport polls it and makes any blocked or future read()
     * fail promptly (return <0) once *flag turns true, instead of waiting out
     * the idle timeout (a blocking read can otherwise stall abort for the full
     * idle window). `flag` must stay valid until close(); NULL clears it. The
     * core wires the stream's abort_flag here right after open_post succeeds. */
    void (*set_abort_flag)(void *ctx, void *conn, const volatile bool *flag);
    /* ---- appended at the tail (same NULL-tolerant contract as set_abort_flag). ----
     * Optional response-header getter used by the provider retry loop
     * (pi_run_sse_post) to read `retry-after`, `retry-after-ms`, and
     * `x-should-retry` off a non-2xx response before deciding whether/when to
     * retry. Returns the header value for `name` (matched case-insensitively) or
     * NULL when absent. Valid only while `conn` is open (call before close()).
     * NULL vtable slot ⇒ header inspection unsupported: retry then falls back to
     * exponential backoff only (no retry-after, no x-should-retry override). */
    const char *(*response_header)(void *ctx, void *conn, const char *name);
} pi_transport_t;

/* ---------- sys: mutex + log (optional; NULL sys ⇒ single-threaded, no logs) */
typedef struct pi_mutex pi_mutex_t; /* opaque, defined by the port */
typedef struct pi_sem pi_sem_t;     /* opaque counting semaphore, defined by the port */

typedef enum { PI_LOG_ERROR = 0, PI_LOG_WARN, PI_LOG_INFO, PI_LOG_DEBUG } pi_log_level_t;

typedef struct pi_sys {
    pi_mutex_t *(*mutex_create)(void *ctx);
    void (*mutex_lock)(pi_mutex_t *m, void *ctx);
    void (*mutex_unlock)(pi_mutex_t *m, void *ctx);
    void (*mutex_destroy)(pi_mutex_t *m, void *ctx);
    void (*log)(pi_log_level_t level, const char *tag, const char *msg, void *ctx);
    void *ctx;
    /* ---- threading + counting semaphore (appended at the tail so existing
     * positional/zero-filled pi_sys initializers stay valid; NULL fields read as
     * absent). Used only by parallel tool execution (PI_FEATURE_PARALLEL_TOOLS):
     * the agent runs a batch in parallel only when ALL of thread_spawn/thread_join/
     * sem_create/sem_post/sem_wait/sem_destroy (and mutex_create) are non-NULL;
     * otherwise it silently falls back to sequential execution. Because tool
     * callbacks and their result allocations run on the spawned worker threads,
     * the env allocator MUST be thread-safe when this set is provided. */
    void *(*thread_spawn)(void (*fn)(void *arg), void *arg, void *ctx); /* opaque handle; NULL=fail */
    void (*thread_join)(void *handle, void *ctx);
    pi_sem_t *(*sem_create)(unsigned initial, void *ctx);
    void (*sem_post)(pi_sem_t *s, void *ctx);
    void (*sem_wait)(pi_sem_t *s, void *ctx);
    void (*sem_destroy)(pi_sem_t *s, void *ctx);
    /* ---- clock + entropy (appended at the tail; NULL fields read as absent).
     * Used by SESSION v3 (PI_FEATURE_SESSION) to mint uuidv7 entry/session ids and
     * ISO-8601 header/entry timestamps. pi_session_create requires BOTH to be
     * non-NULL and never falls back to weak randomness (returns PI_ERR_STATE). */
    uint64_t (*now_ms)(void *ctx);                        /* epoch milliseconds (UTC) */
    int (*random_bytes)(void *ctx, void *buf, size_t n);  /* fill buf with n CSPRNG bytes; 0 = ok */
    /* ---- abort-interruptible sleep (appended at the tail; NULL reads as absent).
     * Used by the provider retry backoff (pi_run_sse_post): sleep up to `ms`,
     * returning early once *abort turns true (poll ~50ms; `abort` may be NULL ⇒
     * sleep the full duration). NULL slot ⇒ backoff cannot sleep, so a request is
     * NOT retried even when max_retries>0 (single-shot fallback); retry therefore
     * requires this slot only when max_retries>0. */
    void (*sleep_ms)(void *ctx, uint32_t ms, const volatile bool *abort);
    /* ---- thread spawn with a stack-size hint (appended at the tail; NULL reads
     * as absent). Used by PI_FEATURE_SUBAGENT: a subagent worker runs a whole
     * agent loop (provider streaming + JSON assembly), which does not fit the
     * small stacks ports size for parallel-tool workers (the FreeRTOS port
     * defaults to PI_WORKER_STACK = 4096 bytes; a loop needs ~32 KiB, see
     * tests/test_stack_budget.c). stack_bytes==0 ⇒ port default. NULL slot ⇒
     * callers fall back to thread_spawn and the hint is dropped — safe only when
     * the port's default thread stack is already large (POSIX); a port whose
     * default is a small worker stack (FreeRTOS) MUST implement this slot before
     * enabling PI_FEATURE_SUBAGENT, or subagent workers will overflow. */
    void *(*thread_spawn_ex)(void (*fn)(void *arg), void *arg, size_t stack_bytes, void *ctx);
} pi_sys_t;

/* ---------- fs: minimal read-only view (skills only) ---------- */
typedef struct pi_fs_dirent {
    const char *name;
    bool is_dir;
} pi_fs_dirent_t;

typedef struct pi_fs {
    /* Read whole file; returns NUL-terminated buffer allocated with `alloc`
     * (caller frees), NULL on failure. out_len may be NULL. */
    char *(*read_file)(void *ctx, const char *path, size_t *out_len, const pi_alloc_t *alloc);
    /* Enumerate entries of a directory; returns PI_OK or PI_ERR_NOT_FOUND. */
    int (*scan_dir)(void *ctx, const char *path, void (*cb)(const pi_fs_dirent_t *e, void *user),
                    void *user);
    /* Optional (NULL on read-only ports): atomically-enough whole-file write. */
    int (*write_file)(void *ctx, const char *path, const char *data, size_t len);
    void *ctx;
    /* ---- appended at the tail so existing positional/zero-filled pi_fs
     * initializers stay valid (same pattern as pi_sys_t's threading set). ----
     * Optional O(1) tail append (creates the file if missing). SESSION uses it so
     * each JSONL entry costs one small append instead of a whole-file rewrite
     * (flash wear + power-loss blast radius); NULL ⇒ callers fall back to
     * write_file with the full content. */
    int (*append_file)(void *ctx, const char *path, const char *data, size_t len);
    /* Optional. Resolve `path` to a canonical absolute form — symbolic links,
     * "." / ".." and duplicate slashes removed — and write it (NUL-terminated)
     * into `out` (capacity `out_cap`). Returns PI_OK on success; a non-zero
     * PI_ERR_* on failure: PI_ERR_NOT_FOUND when the path cannot be resolved
     * (a component is missing, or a symlink is broken/looping), PI_ERR_ARG for
     * a bad argument or an `out` buffer too small to hold the result.
     * NULL ⇒ absent: callers that use this to close a sandbox (read_skill_file
     * resolving symlink escapes) fall back to their lexical check alone. The
     * skills sandbox needs it precisely because read_file follows symlinks. */
    int (*canonicalize)(void *ctx, const char *path, char *out, size_t out_cap);
} pi_fs_t;

#ifdef __cplusplus
}
#endif
#endif /* PI_PORT_H */
