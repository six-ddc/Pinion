/* pi-c — skills (≙ pi harness/skills.ts + system-prompt.ts): SKILL.md progressive
 * disclosure. File format is byte-compatible with pi / Claude skills.
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_SKILLS_H
#define PI_SKILLS_H

#include "pi_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pi_skill {
    char *name;
    char *description;
    char *location; /* absolute path of the SKILL.md / *.md file */
    bool disable_model_invocation;
} pi_skill_t;

typedef struct pi_skill_set pi_skill_set_t;

/* Recursively scan directories for SKILL.md (plus direct *.md files in each root dir,
 * matching pi's loadSkills). Only frontmatter is read — bodies are lazy-loaded.
 * Later dirs win on duplicate names (pass firmware dir first, SD/user dir last). */
int pi_skills_scan(pi_env_t *env, const char *const *dirs, size_t dir_count,
                   pi_skill_set_t **out);

/* Diagnostic emitted for a non-fatal metadata problem during a scan (≙ pi
 * SkillDiagnostic warnings, harness/skills.ts:255-263): `path` is the SKILL.md /
 * *.md file, `message` is the human-readable reason. */
typedef void (*pi_skill_warning_cb)(const char *path, const char *message, void *user);

/* Like pi_skills_scan, but reports name/description validation issues through
 * `warn` (≙ validateName/validateDescription, skills.ts:281-301). Name problems are
 * warnings only — the skill is still kept; a missing/blank description drops the
 * skill (and warns "description is required"). `warn` may be NULL (silent). */
int pi_skills_scan_ex(pi_env_t *env, const char *const *dirs, size_t dir_count,
                      pi_skill_warning_cb warn, void *warn_user, pi_skill_set_t **out);
void pi_skills_free(pi_skill_set_t *set);

size_t pi_skills_count(const pi_skill_set_t *set);
const pi_skill_t *pi_skills_get(const pi_skill_set_t *set, size_t index);
const pi_skill_t *pi_skills_find(const pi_skill_set_t *set, const char *name);

/* ≙ formatSkillsForSystemPrompt (ts: harness/system-prompt.ts:3):
 * "<available_skills>…" block listing name/description/location of every skill
 * without disable-model-invocation. Caller appends to system prompt, then releases
 * with pi_free(pi_env_alloc(env), s) — NOT libc free(): the buffer belongs to the
 * env allocator (PSRAM/arena on device), and the two heaps differ there.
 * Returns NULL on OOM — never a silently truncated catalog. */
char *pi_skills_render_catalog(pi_env_t *env, const pi_skill_set_t *set);

/* ≙ formatSkillInvocation (ts: harness/skills.ts:38): lazy-load the body and wrap in
 * <skill name=... location=...>…</skill>, plus optional extra instructions.
 * Returns allocated string — release with pi_free(pi_env_alloc(env), s), same
 * env-allocator rule as pi_skills_render_catalog. NULL if unknown name / read
 * failure. */
char *pi_skill_load_invocation(pi_env_t *env, const pi_skill_set_t *set, const char *name,
                               const char *additional_instructions);

/* Built-in retrieval tool "read_skill" (DESIGN.md §9): the model fetches a skill body
 * listed in the catalog. `set` and `env` must outlive the agent using the tool.
 * The returned tool's `.user` context is allocated with the env allocator — release
 * it with pi_skills_tool_dispose() once no agent uses the tool anymore. A .user of
 * NULL means the context allocation failed (by-value return can't signal an error);
 * the tool stays safe to register — every call then returns an is_error result. */
pi_agent_tool_t pi_skills_make_read_tool(pi_env_t *env, pi_skill_set_t *set);

/* Sandboxed companion-file reader (DESIGN §12 #10, §16.5): read_skill_file(skill,
 * path) confines `path` to the named skill's directory. Two gates: (1) a lexical
 * check always rejects absolute paths, backslashes and ".." components; (2) when
 * the fs port supplies pi_fs_t.canonicalize, the resolved target must sit strictly
 * under the resolved skill directory — this closes symlink escapes (a link inside
 * the skill dir pointing outside is resolved and rejected). Ports without
 * canonicalize (NULL) keep gate (1) only, which suffices on filesystems that
 * cannot hold symlinks (FAT/SPIFFS on esp32). Lets skill bodies reference relative
 * assets the way pi skills do. Same `.user` lifecycle as pi_skills_make_read_tool(). */
pi_agent_tool_t pi_skills_make_read_file_tool(pi_env_t *env, pi_skill_set_t *set);

/* Free the `.user` context of a tool returned by either maker above (safe on a
 * zeroed/already-disposed tool). The tool must not be executed afterwards. */
void pi_skills_tool_dispose(pi_env_t *env, pi_agent_tool_t *tool);

#ifdef __cplusplus
}
#endif
#endif /* PI_SKILLS_H */
