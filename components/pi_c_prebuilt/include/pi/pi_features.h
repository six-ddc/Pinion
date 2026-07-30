/* pi-c — B-tier compile-time feature switches (DESIGN §12.1). Pure macros, no
 * types: safe to include from anywhere (including other public headers).
 *
 * The build injects PI_FEATURE_X=1 (CMake option on host, Kconfig on ESP-IDF);
 * absent means off. Same flag => identical behaviour on every platform (no
 * semantic forking). The library and its users MUST be compiled with the same
 * set of PI_FEATURE_* values — see the ABI policy note in pi.h.
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_FEATURES_H
#define PI_FEATURES_H

#ifndef PI_FEATURE_IMAGES
#define PI_FEATURE_IMAGES 0
#endif
#ifndef PI_FEATURE_SESSION
#define PI_FEATURE_SESSION 0
#endif
#ifndef PI_FEATURE_PARTIAL_JSON
#define PI_FEATURE_PARTIAL_JSON 0
#endif
#ifndef PI_FEATURE_MODELS_JSON
#define PI_FEATURE_MODELS_JSON 0
#endif
#ifndef PI_FEATURE_COMPAT
#define PI_FEATURE_COMPAT 0
#endif
#ifndef PI_FEATURE_PARALLEL_TOOLS
#define PI_FEATURE_PARALLEL_TOOLS 0
#endif
#ifndef PI_FEATURE_COMPACTION
#define PI_FEATURE_COMPACTION 0
#endif
#ifndef PI_FEATURE_SKILLS_IGNORE
#define PI_FEATURE_SKILLS_IGNORE 0
#endif
/* harness read/write/edit tools + the ExecutionEnv filesystem half (pi_exec.h) */
#ifndef PI_FEATURE_HARNESS_TOOLS
#define PI_FEATURE_HARNESS_TOOLS 0
#endif
/* the bash tool + shell output capture + the ExecutionEnv exec slot; POSIX only,
 * and requires HARNESS_TOOLS (its capture path uses truncate + create_temp_file) */
#ifndef PI_FEATURE_HARNESS_EXEC
#define PI_FEATURE_HARNESS_EXEC 0
#endif
/* in-process multi-threaded subagent pool + LLM tool bindings (pi_subagent.h) */
#ifndef PI_FEATURE_SUBAGENT
#define PI_FEATURE_SUBAGENT 0
#endif
/* Anthropic subscription OAuth: the PKCE/token protocol layer (pi_oauth.h) plus the
 * Claude Code wire identity in the anthropic-messages provider. Credential storage
 * and the login flow itself stay with the caller (DESIGN §12 #8). */
#ifndef PI_FEATURE_OAUTH
#define PI_FEATURE_OAUTH 0
#endif

#endif /* PI_FEATURES_H */
