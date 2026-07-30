/* pi-c — harness tools: read / write / edit / bash (≙ pi agent/src/harness/tools/).
 *
 * Four agent tools over a pi_exec_env_t (pi_exec.h). Each maker mirrors the skills
 * convention (pi_skills_make_read_tool): it returns a by-value pi_agent_tool_t whose
 * `.user` is an env-allocated context; release it with pi_harness_tool_dispose once
 * the agent that holds the tool is gone. `env` and `xenv` must both outlive the tool.
 *
 * ---------------------------------------------------------------------------
 * THESE TOOLS ARE NOT SANDBOXED. THIS IS DELIBERATE — DO NOT "FIX" IT.
 * ---------------------------------------------------------------------------
 * `path` is accepted exactly as upstream accepts it (read.ts:17, "relative or
 * absolute"), including "..", "~", and "file://". read/write/edit will therefore
 * touch anything the process can touch, and bash will run anything bash can run.
 * Confinement is the host's decision and belongs one layer up:
 *   - pi_agent_hooks.before_tool_call to inspect/veto (or rewrite) arguments, or
 *   - a wrapper pi_exec_env_t vtable that rejects paths outside a subtree.
 * The narrow canonicalize-based sandbox in pi_skills.c is specific to
 * read_skill_file's "relative to the skill directory" contract and is intentionally
 * NOT reused here — adding it would silently diverge from upstream behaviour.
 * Within this unit canonical_path has exactly one use: the per-path mutation-queue
 * key that serializes concurrent write/edit calls.
 *
 * bash is provided as a maker only and is never registered automatically: exposing
 * shell execution is the host's call. Gate it behind before_tool_call approval.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_HARNESS_H
#define PI_HARNESS_H

#include "pi_agent.h"
#include "pi_exec.h"
#include "pi_features.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PI_FEATURE_HARNESS_TOOLS

/* All three file tools: `.user` is NULL when the maker hit OOM, and execute() then
 * returns an is_error result instead of crashing (≙ read_skill_exec, pi_skills.c:573). */
pi_agent_tool_t pi_harness_make_read_tool(pi_env_t *env, pi_exec_env_t *xenv);
pi_agent_tool_t pi_harness_make_write_tool(pi_env_t *env, pi_exec_env_t *xenv);
pi_agent_tool_t pi_harness_make_edit_tool(pi_env_t *env, pi_exec_env_t *xenv);

#if PI_FEATURE_HARNESS_EXEC
typedef struct pi_bash_tool_opts {
    /* ≙ BashToolOptions.commandPrefix (bash.ts:37): prepended to every command as
     * "<prefix>\n<command>". Borrowed, not copied — it must outlive the tool.
     *
     * BashToolOptions.prepare (bash.ts:30-34) is deliberately absent: its purpose
     * (per-turn cwd/env adjustment) is served in pi-c by before_tool_call rewriting
     * the arguments, or by the host keeping one pi_exec_env_t per working directory.
     * Porting it would mean a two-way mutable callback protocol in C for no gain. */
    const char *command_prefix;
} pi_bash_tool_opts_t;

/* `opts` may be NULL (no prefix). */
pi_agent_tool_t pi_harness_make_bash_tool(pi_env_t *env, pi_exec_env_t *xenv,
                                          const pi_bash_tool_opts_t *opts);
#endif

/* Frees the maker's context. Safe on a zeroed or already-disposed tool. */
void pi_harness_tool_dispose(pi_env_t *env, pi_agent_tool_t *tool);

#endif /* PI_FEATURE_HARNESS_TOOLS */

#ifdef __cplusplus
}
#endif
#endif /* PI_HARNESS_H */
