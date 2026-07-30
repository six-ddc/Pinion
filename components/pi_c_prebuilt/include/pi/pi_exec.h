/* pi-c — ExecutionEnv: the filesystem + shell capability the harness tools run on
 * (≙ pi agent/src/harness/types.ts ExecutionEnv, implemented for POSIX in
 * port/posix/exec_posix.c ≙ env/nodejs.ts NodeExecutionEnv).
 *
 * Held in its own header rather than pi_harness.h: the contract is a platform
 * capability (like pi_fs_t / pi_sys_t in pi_port.h), not a property of the four
 * tools, and non-tool consumers can depend on it without pulling in pi_agent.h.
 *
 * IRON RULE (≙ types.ts "operations never throw"): no operation ever aborts the
 * process or longjmps. Every failure is an `int` return — 0 (PI_XFS_OK/PI_XEXEC_OK)
 * or a negative code from the enums below — with optional detail in an out-param.
 *
 * NOT A SANDBOX. See the header comment in pi_harness.h: paths are accepted as
 * given (absolute, relative, "~", "file://") and resolved against `cwd`. Confining
 * an agent to a subtree is the host's job (pi_agent_hooks.before_tool_call, or a
 * wrapper vtable around this one) — not this layer's.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_EXEC_H
#define PI_EXEC_H

#include "pi_features.h"
#include "pi_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PI_FEATURE_HARNESS_TOOLS

typedef struct pi_exec_env pi_exec_env_t;

/* ---------- filesystem error model (≙ FileError.code, types.ts:150-158) ----------
 * errno mapping (≙ toFileError, nodejs.ts:96-118): ENOENT→NOT_FOUND,
 * EACCES/EPERM→PERMISSION, ENOTDIR→NOT_DIRECTORY, EISDIR→IS_DIRECTORY,
 * EINVAL→INVALID, anything else→UNKNOWN (the raw errno survives in sys_errno). */
typedef enum {
    PI_XFS_OK = 0,
    PI_XFS_ABORTED = -1,
    PI_XFS_NOT_FOUND = -2,
    PI_XFS_PERMISSION = -3,
    PI_XFS_NOT_DIRECTORY = -4,
    PI_XFS_IS_DIRECTORY = -5,
    PI_XFS_INVALID = -6, /* also: a directory entry that is neither file/dir/symlink
                          * (≙ fileInfoFromStats' undefined kind, nodejs.ts:82) */
    PI_XFS_NOT_SUPPORTED = -7,
    PI_XFS_NOMEM = -8, /* pi-c addition: allocation failure (no JS counterpart) */
    PI_XFS_UNKNOWN = -9
} pi_xfs_ec_t;

typedef struct pi_xfs_err {
    pi_xfs_ec_t code;
    int sys_errno; /* the errno behind `code`, before mapping; 0 = none */
} pi_xfs_err_t;

/* Stable spelling of a code, for tool error text (never NULL). */
const char *pi_xfs_ec_str(pi_xfs_ec_t code);

/* ---------- exec error model (≙ ExecutionError.code, types.ts:176-182) ----------
 * "callback_error" is absent by construction: a C output callback cannot throw
 * (DEVIATION, ≙ nodejs.ts:457-476). */
typedef enum {
    PI_XEXEC_OK = 0,
    PI_XEXEC_ABORTED = -1,
    PI_XEXEC_TIMEOUT = -2,
    PI_XEXEC_SHELL_UNAVAILABLE = -3,
    PI_XEXEC_SPAWN_ERROR = -4,
    PI_XEXEC_NOMEM = -8,
    PI_XEXEC_UNKNOWN = -9
} pi_xexec_ec_t;

/* ---------- file metadata (≙ FileInfo, types.ts:269-280) ---------- */
typedef enum { PI_XFILE_FILE, PI_XFILE_DIR, PI_XFILE_SYMLINK } pi_xfile_kind_t;

typedef struct pi_xfile_info {
    char *name; /* basename; owned by the env allocator */
    char *path; /* the absolute path as addressed — symlinks NOT followed */
    pi_xfile_kind_t kind; /* lstat semantics (≙ nodejs.ts:585) */
    uint64_t size;
    uint64_t mtime_ms;
} pi_xfile_info_t;

#if PI_FEATURE_HARNESS_EXEC
/* ---------- shell options (≙ ShellExecOptions, types.ts:344-359) ---------- */
typedef struct pi_exec_opts {
    const char *cwd;              /* NULL ⇒ env->cwd; relative ⇒ resolved against it */
    const char *const *extra_env; /* "KEY=VALUE" entries */
    size_t extra_env_count;
    bool replace_env;    /* true ≙ inheritEnv:false; default (false) inherits environ */
    uint32_t timeout_ms; /* 0 ⇒ no timeout; >2147483647 is rejected (≙ nodejs.ts:37-48) */
    const volatile bool *abort_flag; /* ≙ abortSignal, polled ≤50ms; NULL allowed */
    /* ≙ onStdout + onStderr merged (types.ts:356-358). `chunk` is NOT NUL-terminated
     * — `len` is authoritative. Called on the calling thread, inside exec(). */
    void (*on_output)(const char *chunk, size_t len, bool is_stderr, void *user);
    void *user;
} pi_exec_opts_t;
#endif

/* ---------- the vtable ----------
 * Every op returns 0 or a pi_xfs_ec_t; `err` may be NULL. Paths may be relative
 * (resolved against `cwd`). Returned heap objects belong to the allocator the env
 * was constructed with; free strings with pi_free and infos with file_info_free.
 * Ops taking `abort_flag` check it on entry and, for the ones that loop, inside
 * the loop (≙ abortResult, nodejs.ts:120-122). */
struct pi_exec_env {
    char *cwd; /* absolute; owned by the env */
    /* The allocator every out-param below is allocated from — callers need it to
     * release those strings and buffers (pi_free / file_info_free). Never NULL. */
    const pi_alloc_t *alloc;

    /* Lexical resolution only: "~"/"~/x" → $HOME, "file://…" → path, then "."/".."
     * and duplicate slashes folded. Does NOT touch the disk or resolve symlinks
     * (≙ resolvePath + Node resolve(), nodejs.ts:50-64). */
    int (*absolute_path)(pi_exec_env_t *e, const char *path, char **out, pi_xfs_err_t *err);
    /* Whole-file binary read. `*out` gets a NUL-terminated buffer of `*out_len`
     * bytes (the NUL is one past the end, so text callers can use it directly).
     * A text read is this plus the UTF-8 repair in pi_sanitize_surrogates
     * (src/util/pi_unicode.h). */
    int (*read_file)(pi_exec_env_t *e, const char *path, uint8_t **out, size_t *out_len,
                     const volatile bool *abort_flag, pi_xfs_err_t *err);
    /* Creates missing parent directories (≙ nodejs.ts:561). */
    int (*write_file)(pi_exec_env_t *e, const char *path, const void *data, size_t len,
                      const volatile bool *abort_flag, pi_xfs_err_t *err);
    /* Same, appending (≙ nodejs.ts:574). No abort hook upstream either. */
    int (*append_file)(pi_exec_env_t *e, const char *path, const void *data, size_t len,
                       pi_xfs_err_t *err);
    int (*file_info)(pi_exec_env_t *e, const char *path, pi_xfile_info_t *out,
                     pi_xfs_err_t *err);
    void (*file_info_free)(pi_exec_env_t *e, pi_xfile_info_t *info);
    /* Entries of a directory, "." and ".." excluded, each lstat'ed. Entries whose
     * kind is unsupported are skipped, not failed (≙ nodejs.ts:604). */
    int (*list_dir)(pi_exec_env_t *e, const char *path, pi_xfile_info_t **out, size_t *out_n,
                    const volatile bool *abort_flag, pi_xfs_err_t *err);
    /* realpath(3): follows symlinks, requires every component to exist
     * (≙ nodejs.ts:618). Contrast absolute_path, which is purely lexical. */
    int (*canonical_path)(pi_exec_env_t *e, const char *path, char **out, pi_xfs_err_t *err);
    /* NOT_FOUND ⇒ ok with *out=false; every other error propagates
     * (≙ nodejs.ts:624-629). */
    int (*exists)(pi_exec_env_t *e, const char *path, bool *out, pi_xfs_err_t *err);
    int (*create_dir)(pi_exec_env_t *e, const char *path, bool recursive, pi_xfs_err_t *err);
    int (*remove)(pi_exec_env_t *e, const char *path, bool recursive, bool force,
                  pi_xfs_err_t *err);
    /* `prefix` NULL ⇒ "tmp-". *out is the new directory's absolute path. */
    int (*create_temp_dir)(pi_exec_env_t *e, const char *prefix, char **out, pi_xfs_err_t *err);
    /* A fresh empty file inside a fresh private temp directory (≙ nodejs.ts:659-669):
     * "<tempdir>/<prefix><random><suffix>". Either affix may be NULL. */
    int (*create_temp_file)(pi_exec_env_t *e, const char *prefix, const char *suffix, char **out,
                            pi_xfs_err_t *err);
#if PI_FEATURE_HARNESS_EXEC
    /* Run `command` through bash (or sh) and block until it finishes. Returns 0 or a
     * pi_xexec_ec_t; on 0, *exit_code holds the status (write it only then).
     *
     * DEVIATION: unlike upstream (types.ts:367) this does NOT return accumulated
     * stdout/stderr strings — the four tools consume opts->on_output and the exit
     * code only, so accumulating is left to the caller (pi_shell_capture), saving a
     * second full copy of the output. */
    int (*exec)(pi_exec_env_t *e, const char *command, const pi_exec_opts_t *opts,
                int *exit_code);
#endif
    /* Best-effort, cannot fail, idempotent: kills any still-running children
     * (≙ activeChildPids sweep, nodejs.ts:671-674). */
    void (*cleanup)(pi_exec_env_t *e);
    void *ctx;
    /* The per-path mutation queue that serializes write/edit against THIS env
     * (≙ the WeakMap<ExecutionEnv> keying in file-mutation-queue.ts:9). Created and
     * owned by the implementation; opaque here, see src/harness/pi_mutation_queue.h.
     * NULL is legal and means "no serialization" (single-threaded hosts). */
    struct pi_mutation_queue *mutation_queue;
};

/* ---------- POSIX implementation (port/posix/exec_posix.c) ----------
 * `cwd` NULL ⇒ getcwd(). `a` NULL ⇒ pi_alloc_default(). `sys` NULL ⇒ single-threaded:
 * the per-path mutation queue degrades to a pass-through and temp names fall back
 * from sys->random_bytes to a clock+counter seed. Returns NULL on OOM or when `cwd`
 * cannot be made absolute. */
pi_exec_env_t *pi_posix_exec_env_create(const char *cwd, const pi_alloc_t *a,
                                        const pi_sys_t *sys);
void pi_posix_exec_env_destroy(pi_exec_env_t *e); /* cleanup() + free; NULL-safe */

#endif /* PI_FEATURE_HARNESS_TOOLS */

#ifdef __cplusplus
}
#endif
#endif /* PI_EXEC_H */
