/* pi-c — SESSION v3 tree persistence (PI_FEATURE_SESSION).
 *
 * Mirrors pi's harness jsonl session storage (upstream:
 * packages/agent/src/harness/session/jsonl-storage.ts, session.ts,
 * harness/types.ts). A session file is one JSON object per line: a version-3
 * SessionHeader on the first line, then a linear/tree of SessionTreeEntry lines.
 * Entries chain by parentId; the active branch tip is the "leaf". Entry ids are
 * the first 8 hex chars of a uuidv7; the session id is a full uuidv7.
 *
 * Interoperable with upstream pi: files written here load in pi and files pi
 * writes load here (entry/message field shapes copied field-for-field from
 * harness/types.ts and packages/ai/src/types.ts Message). Unknown entry types are
 * preserved on load (never rejected). SPDX-License-Identifier: MIT */
#ifndef PI_SESSION_H
#define PI_SESSION_H
#include "pi_agent.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Opaque session handle. */
typedef struct pi_session pi_session_t;

/* ---------------------------------------------------------------- lifecycle */

/* Create a new v3 session file at `path`, writing the SessionHeader first line
 * {type:"session",version:3,id,timestamp,cwd}. The session id (uuidv7) and the
 * ISO-8601 UTC timestamp are minted from env->sys->now_ms + random_bytes; BOTH
 * callbacks are REQUIRED — if either is NULL the call returns PI_ERR_STATE and no
 * file is written (never a weak-random fallback). Needs env->fs->write_file.
 * `cwd` must be a non-empty string (PI_ERR_ARG otherwise): it is a required header
 * field, and an empty one would produce a file pi_session_open rejects.
 * On PI_OK, *out receives an owned handle; free it with pi_session_close(). */
int pi_session_create(pi_env_t *env, const char *path, const char *cwd, pi_session_t **out);

/* pi_session_create plus the two optional header fields (≙ JsonlSessionCreateOptions,
 * types.ts:540-544): `parent_session_path` (≙ SessionHeader.parentSession — the file a
 * fork descends from) and `metadata_json` (≙ SessionHeader.metadata — an application
 * JSON OBJECT; a non-object, malformed or too-deep payload is PI_ERR_ARG, never a
 * silently dropped field). Either may be NULL, in which case the key is omitted and
 * the header stays byte-identical to a plain pi_session_create. */
int pi_session_create_ex(pi_env_t *env, const char *path, const char *cwd,
                         const char *parent_session_path, const char *metadata_json,
                         pi_session_t **out);

/* Which entries a fork copies, relative to its `entry_id` (≙ getEntriesToFork's
 * `position`, repo-utils.ts:32-51). */
typedef enum {
    PI_FORK_BEFORE = 0, /* everything strictly before entry_id; entry_id MUST be a user
                           message (PI_ERR_ARG otherwise, ≙ invalid_fork_target) */
    PI_FORK_AT          /* everything up to and including entry_id */
} pi_fork_position_t;

/* Fork `source` into a NEW session file at `dest_path` (≙ SessionRepo.fork,
 * jsonl-repo.ts:134-161): the forked entries are copied VERBATIM — original id,
 * parentId and timestamp — so the new file has the same tree shape and every
 * cross-reference (parentId / firstKeptEntryId / fromId / targetId) still resolves.
 * This is fundamentally different from pi_session_move_to, which only re-points the
 * leaf inside one existing file.
 *   entry_id == NULL  ⇒ copy every entry of the source file (≙ storage.getEntries()).
 *   entry_id != NULL  ⇒ copy the branch of the effective leaf, truncated at the newest
 *                       compaction's retained window like every other read path.
 * `dest_cwd` NULL inherits the source cwd. The new header records
 * parentSession = source path and inherits the source metadata unless
 * `metadata_json` overrides it. Returns PI_ERR_NOT_FOUND for an unknown entry_id,
 * PI_ERR_ARG for an invalid PI_FORK_BEFORE target. On PI_OK *out is an owned handle
 * for the new session (the source is untouched and stays open). */
int pi_session_fork(pi_env_t *env, pi_session_t *source, const char *dest_path,
                    const char *dest_cwd, const char *entry_id, pi_fork_position_t position,
                    const char *metadata_json, pi_session_t **out);

/* Open an existing v3 session file: read the whole file, reject a header whose
 * version != 3 (PI_ERR_STATE), index every entry by id, and restore the current
 * leaf. Unknown entry types are kept (loaded, not rejected) so parentId chains
 * through them still resolve. Missing file => PI_ERR_NOT_FOUND.
 * Corruption recovery is READ-ONLY: unparseable entry lines (torn tails from
 * power loss, flipped bytes) are skipped — the entries after them still load —
 * and an embedded-NUL tail (flash zero-fill) truncates to the last good entry;
 * in both cases the on-disk cleanup is deferred to the next append, so an open
 * with no append never modifies the file. A corrupt header still refuses the
 * whole file (PI_ERR_STATE), like upstream.
 * The header is validated field by field like upstream parseHeaderLine: id, timestamp
 * and cwd must be non-empty strings, an optional parentSession must be a string and an
 * optional metadata must be a non-array object — every violation is PI_ERR_STATE
 * (malformed), so PI_ERR_NOMEM from this call always means real memory pressure.
 * Entry lines are validated too (type/id/timestamp present, parentId null-or-string,
 * leaf targetId null-or-string); a violation is treated like a corrupt line (skipped,
 * cleanup deferred) rather than loaded, because an entry with no usable id used to
 * silently null the leaf. If the resulting leaf points at an entry that did not load,
 * the leaf falls back to the newest entry that did and pi_session_last_error() reports
 * PI_ERR_STATE — never a silently empty history. */
int pi_session_open(pi_env_t *env, const char *path, pi_session_t **out);

void pi_session_close(pi_session_t *s);

/* ------------------------------------------------------------------- append */
/* Each append writes one entry line, updates the in-memory tree, and advances the
 * leaf; the new entry's parentId is the leaf id at call time (NULL at the root).
 * Persistence uses the port's O(1) append_file when available and the on-disk
 * file matches the in-memory mirror; otherwise (no append_file, or a recovery/
 * normalization flagged a rewrite) the whole file is rewritten from the mirror —
 * the on-disk result is byte-identical either way.
 *
 * Write-timing contract (differs from upstream on purpose): the harness buffers
 * run-time state changes while a turn is in flight (pendingSessionWrites, flushed at
 * prepareNextTurn/turn_end/agent_end), so its files never contain a model_change /
 * thinking_level_change / active_tools_change parented on a mid-turn message. pi-c has
 * no harness: these entries land exactly when you call them. Prefer appending them
 * while the agent is idle; appending during a run parents the entry on the current
 * turn's latest message, which makes the change take effect one turn earlier in the
 * path-based state derivations (pi_session_thinking_level / _model / _active_tools). */
int pi_session_append_message(pi_session_t *s, const pi_message_t *msg);
int pi_session_append_model_change(pi_session_t *s, const char *provider, const char *model_id);
int pi_session_append_thinking_level_change(pi_session_t *s, const char *thinking_level);
/* Record the set of tool names active from this point on the branch (≙ Session
 * .appendActiveToolsChange / ActiveToolsChangeEntry, session.ts:163). The `names`
 * array (may be empty; count 0 records "no tools active") is copied. On transcript
 * rebuild the LAST active_tools_change on the leaf's path wins (see
 * pi_session_active_tools). */
int pi_session_append_active_tools_change(pi_session_t *s, const char *const *names, size_t count);
/* label == NULL clears any label on target_id. target_id must be a known entry. */
int pi_session_append_label(pi_session_t *s, const char *target_id, const char *label);
int pi_session_append_session_name(pi_session_t *s, const char *name);
/* data_json: optional raw JSON payload (NULL => omitted; malformed or too-deep
 * JSON => PI_ERR_ARG, never a silently omitted field). */
int pi_session_append_custom(pi_session_t *s, const char *custom_type, const char *data_json);
/* custom_message entry (≙ Session.appendCustomMessageEntry, session.ts:295-311): a
 * synthetic message the application injects into the transcript. Unlike a `custom`
 * entry (opaque data, no message) this one DOES rebuild into a PI_ROLE_CUSTOM message.
 * Content comes in exactly one of two forms — a bare `text` string, or `content_json`
 * holding a TextContent|ImageContent JSON ARRAY; supplying both or neither is
 * PI_ERR_ARG, as is a non-array/malformed content_json or details_json.
 * Needed because the agent bridge deliberately never persists synthetic-role messages
 * (they are projections of their own entry types): this is the only way a PI_ROLE_CUSTOM
 * message survives a restart. */
int pi_session_append_custom_message(pi_session_t *s, const char *custom_type, const char *text,
                                    const char *content_json, bool display,
                                    const char *details_json);
/* Compaction entry (struct per harness/types.ts:362; generation is P7). details_json
 * optional (NULL => omitted; malformed => PI_ERR_ARG). first_kept_entry_id is
 * `string | undefined` upstream — NULL means "nothing was retained" and omits the key
 * (the read side already tolerates its absence). tokens_before is the
 * pre-compaction token count. `usage` (optional, NULL => key omitted) records the
 * token/cost usage of the LLM call that produced the summary (≙ appendCompaction's
 * usage parameter) — pi_session_stats and upstream's getSessionStats both bill it. */
int pi_session_append_compaction(pi_session_t *s, const char *summary,
                                 const char *first_kept_entry_id, uint32_t tokens_before,
                                 const char *details_json, const pi_usage_t *usage);

/* Write a leaf entry re-pointing the active leaf (branch switch). leaf_id == NULL
 * moves to the root; an unknown leaf_id returns PI_ERR_NOT_FOUND. */
int pi_session_set_leaf(pi_session_t *s, const char *leaf_id);

/* Move the active leaf to `entry_id` and optionally record a branch summary
 * (≙ Session.moveTo, session.ts:247). Writes a leaf entry pointing at entry_id
 * (NULL => root); an unknown entry_id returns PI_ERR_NOT_FOUND with no change. When
 * `summary` is non-NULL a branch_summary entry {fromId: entry_id or "root", summary,
 * details, fromHook} is appended parented on entry_id, becoming the new tip. On
 * transcript rebuild a branch_summary contributes its summary as a user message
 * (folded, no dedicated role — DESIGN deviation, same as compaction summaries).
 * details_json is optional raw JSON (NULL => omitted; malformed => PI_ERR_ARG,
 * checked before anything persists). Branch summary generation is a separate step
 * (pi_branch_summarize, PI_FEATURE_COMPACTION); this call only navigates and
 * persists a caller-provided summary.
 * Half-completion: the leaf entry and the branch_summary entry are two writes; if
 * the second fails (OOM/IO) the navigation is already durable and only the summary
 * is missing — the caller may retry it (the two cannot be atomic without a journal).
 * `usage` (optional) is persisted on the branch_summary entry (≙ moveTo's
 * summary.usage) and is ignored when `summary` is NULL. */
int pi_session_move_to(pi_session_t *s, const char *entry_id, const char *summary,
                       const char *details_json, bool from_hook, const pi_usage_t *usage);

/* --------------------------------------------------------------- inspection */
const char *pi_session_id(const pi_session_t *s);         /* header session id */
const char *pi_session_cwd(const pi_session_t *s);        /* header cwd */
const char *pi_session_created_at(const pi_session_t *s); /* header ISO-8601 timestamp */
const char *pi_session_leaf_id(const pi_session_t *s);    /* current leaf id or NULL */
/* Optional header fields (NULL when absent): the file this session was forked from
 * (≙ JsonlSessionMetadata.parentSessionPath) and the application metadata object as
 * compact JSON text (≙ .metadata). Both borrowed, valid until pi_session_close. */
const char *pi_session_parent_path(const pi_session_t *s);
const char *pi_session_metadata_json(const pi_session_t *s);
size_t pi_session_entry_count(const pi_session_t *s);
const char *pi_session_entry_type_at(const pi_session_t *s, size_t i); /* NULL if out of range */
/* Whether `id` names a loaded entry (≙ storage.getEntry(id) !== undefined). */
bool pi_session_has_entry(const pi_session_t *s, const char *id);

/* Current label of `target_id`, or NULL when unlabeled (≙ storage.getLabel). Replays
 * every label entry in file order: a label whose TRIMMED text is non-empty sets it,
 * while JSON null / "" / all-whitespace CLEARS it (upstream deletes the map key rather
 * than storing an empty string). The answer is a trimmed copy owned by the session,
 * valid until the next pi_session_label call or pi_session_close. */
const char *pi_session_label(pi_session_t *s, const char *target_id);

/* Session name: the LAST session_info entry's name, trimmed, or NULL when there is
 * none or it trims to empty (≙ storage.getSessionName's `?.trim() || undefined`).
 * Owned by the session like pi_session_label. */
const char *pi_session_name(pi_session_t *s);

/* Whole-file usage/cost roll-up (≙ storage.getSessionStats, jsonl-storage.ts:308-348).
 * Scans EVERY entry, not the current branch. usage is taken from assistant messages and
 * from compaction / branch_summary entries; an entry whose usage is missing any of
 * input/output/cacheRead/cacheWrite/cost.total is skipped entirely (no partial sums),
 * matching upstream's guard. total_tokens is recomputed as input+output+cacheRead+
 * cacheWrite — the stored totalTokens field is deliberately not used. */
typedef struct pi_session_stats {
    uint32_t message_count;     /* number of `message` entries */
    uint64_t cached_tokens;     /* sum of cacheRead */
    uint64_t uncached_tokens;   /* sum of input + cacheWrite */
    uint64_t total_tokens;      /* sum of input + output + cacheRead + cacheWrite */
    double cost_total;          /* sum of cost.total */
} pi_session_stats_t;
int pi_session_stats(pi_session_t *s, pi_session_stats_t *out);
/* Last persistence failure recorded by the attached-agent bridge (the notify path
 * has no caller to return an error to). PI_OK when everything persisted; latched
 * until pi_session_clear_last_error. Poll after a run when durability matters.
 *
 * The latch is FAIL-STOP: while it is set, the bridge appends nothing further, so
 * the file on disk always holds a consistent PREFIX of the conversation rather than
 * a chain with holes in it. Without this, an append that failed mid-turn would be
 * skipped while later ones kept landing — producing a toolResult whose toolCall is
 * absent, which providers reject on restore and the orphan-synthesis paths paper
 * over with a toolCall that never happened.
 *
 * So a caller that wants persistence to resume must call
 * pi_session_clear_last_error() after handling the failure; note the messages
 * dropped while the latch was set are NOT written retroactively, and the transcript
 * in memory is therefore ahead of the file. Re-attaching a fresh session is the way
 * to get a complete file again. */
int pi_session_last_error(const pi_session_t *s);
void pi_session_clear_last_error(pi_session_t *s);

/* ------------------------------------------------------- transcript rebuild */
/* Rebuild the pi_message_t transcript from the current leaf's path-to-root,
 * honouring compaction: when a compaction entry is on the path, messages before
 * its firstKeptEntryId are dropped and replaced by the compaction summary (pi-c
 * has no dedicated compaction-summary role, so it is emitted as a user text
 * message — DESIGN.md deviation). On PI_OK, *out_msgs is an owned array of
 * *out_count messages (free each with pi_message_free, then pi_free the array);
 * an empty transcript yields *out_count == 0 and *out_msgs == NULL. */
int pi_session_transcript(pi_session_t *s, pi_message_t ***out_msgs, size_t *out_count);

/* Active tool set for the current leaf: the activeToolNames of the LAST
 * active_tools_change entry on the leaf's path-to-root (≙ buildSessionContext
 * .activeToolNames, session.ts:35). Returns NULL with *count == 0 when no such entry
 * exists on the path (upstream `null`), or a non-NULL array (possibly length 0 for an
 * explicit empty set). The returned array is owned by the session and stays valid
 * until the next pi_session_active_tools call or pi_session_close; it is recomputed
 * per call, so it reflects the current leaf after pi_session_move_to. pi-c has no
 * harness that filters tools per turn — the application is responsible for reading
 * this after a rebuild/navigation and applying it to the tools it sends. */
const char *const *pi_session_active_tools(pi_session_t *s, size_t *count);

/* The other two derived states upstream's buildSessionContext exposes alongside
 * activeToolNames (deriveSessionContextState, session.ts:39-57), each a last-wins scan
 * over the same compaction-truncated leaf path. Without these, a model/thinking-level
 * change could be written but never read back — pi_session_restore would resume a
 * conversation with the wrong model.
 *
 * thinking level: the last thinking_level_change on the path, or "off" when there is
 * none (upstream's default, not an empty string). Never NULL.
 * model: the last model_change on the path OR the last assistant message (whose
 * provider/model fields carry the same information, so a session that only ever ran
 * still reports its model). PI_ERR_NOT_FOUND expresses upstream's `model: null`.
 * All returned strings are borrowed from the loaded entries: valid until
 * pi_session_close (unlike pi_session_active_tools they are not recomputed copies). */
const char *pi_session_thinking_level(pi_session_t *s);
int pi_session_model(pi_session_t *s, const char **out_provider, const char **out_model_id);

/* ------------------------------------------------------------- agent bridge */
/* Persist the ongoing conversation: after attach, every message pushed onto the
 * agent's transcript is appended to the session automatically (parentId = current
 * leaf). While an agent is attached, pi_agent_transcript_splice on it returns
 * PI_ERR_STATE (the v3 tree is append-only; splice is the escape hatch for the
 * no-session case). The bridge is strictly 1:1 and enforced on both ends:
 * attaching returns PI_ERR_STATE when this session already has a different agent
 * OR when the agent is already attached to a different session (detach there
 * first); re-attaching the same pair is an idempotent PI_OK. pi_session_detach /
 * pi_session_close only unhook the agent when its bridge still points at this
 * session, so closing a stale session never severs a newer attachment. */
int pi_session_attach(pi_session_t *s, pi_agent_t *agent);
void pi_session_detach(pi_session_t *s);

/* Load the rebuilt transcript into `agent` (via pi_agent_transcript_append)
 * without re-persisting it — safe whether or not the session is attached. Call
 * before continuing an attached conversation. */
int pi_session_restore(pi_session_t *s, pi_agent_t *agent);

/* Format the conventional session path
 * "<root>/<encodeCwd(cwd)>/<ts>_<sessionId>.jsonl" (upstream jsonl-repo.ts) into
 * `buf`. encodeCwd => "--<cwd, leading slash stripped, /\\: -> ->--"; ts has ':'
 * and '.' replaced by '-'. Returns the written length (excl. NUL) or a negative
 * PI_ERR_* (PI_ERR_ARG / PI_ERR_NOMEM on truncation). Helper only — embedded
 * callers may pass any path to pi_session_create; directory creation is theirs. */
int pi_session_default_path(const char *root, const char *cwd, const char *session_id,
                            const char *iso_timestamp, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
