/* pi-c — ai layer (≙ @earendil-works/pi-ai): unified streaming LLM API.
 * Event names mirror pi's AssistantMessageEvent (ts: packages/ai/src/types.ts:453).
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_AI_H
#define PI_AI_H

#include "pi_features.h"
#include "pi_types.h"

#if PI_FEATURE_PARTIAL_JSON
#include "cJSON.h" /* convenience for consumers of partial_args (cJSON_* accessors) */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration so pi_ai_event_t has ONE layout regardless of feature
 * flags (a field gated on PI_FEATURE_PARTIAL_JSON would make the struct size
 * depend on the macro — an ABI trap between differently-configured TUs). */
struct cJSON;

/* ---------- assistant message events (≙ AssistantMessageEvent, full 12-kind set,
 * ts: packages/ai/src/types.ts:453) ---------- */
typedef enum {
    PI_AI_EV_START,           /* stream accepted; partial has 0 blocks */
    PI_AI_EV_TEXT_START,      /* blocks[block_index] opened */
    PI_AI_EV_TEXT_DELTA,      /* text fragment appended to blocks[block_index] */
    PI_AI_EV_TEXT_END,        /* blocks[block_index] complete */
    PI_AI_EV_THINKING_START,
    PI_AI_EV_THINKING_DELTA,
    PI_AI_EV_THINKING_END,
    PI_AI_EV_TOOLCALL_START,  /* id/name available on the block */
    PI_AI_EV_TOOLCALL_DELTA,  /* raw JSON fragment appended to tool-call arguments */
    PI_AI_EV_TOOLCALL_END,    /* arguments complete: safe to parse/dispatch */
    PI_AI_EV_DONE,            /* final message ready (also delivered via out param) */
    PI_AI_EV_ERROR            /* final message with stop_reason == PI_STOP_ERROR */
} pi_ai_event_kind_t;

typedef struct pi_ai_event {
    pi_ai_event_kind_t kind;
    const pi_message_t *partial; /* current accumulating assistant message; never NULL */
    size_t block_index;          /* valid for all block-scoped events */
    const char *delta;           /* valid for *_DELTA; NUL-terminated fragment */
    /* Parsed accumulated tool-call arguments (TOOLCALL_DELTA/TOOLCALL_END only;
     * NULL otherwise, and always NULL unless built with PI_FEATURE_PARTIAL_JSON).
     * Owned by the emitter and valid only for the duration of the callback —
     * deep-copy anything you keep (≙ parseStreamingJson per delta). The field
     * exists unconditionally so the struct layout never depends on the flag. */
    const struct cJSON *partial_args;
} pi_ai_event_t;

/* Delivered synchronously on the calling thread, in wire order.
 * Invariants (same as pi's EventStream): START first; exactly one DONE or ERROR, last. */
typedef void (*pi_ai_event_cb)(const pi_ai_event_t *ev, void *user);

/* ---------- environment (≙ createModels(): providers + injected platform) ---------- */
#define PI_MAX_PROVIDERS 8

typedef struct pi_env pi_env_t;

/* ---------- provider interface (≙ ProviderStreams, ts: types.ts:222) ---------- */
typedef struct pi_provider {
    const char *id; /* "openai-completions", "anthropic-messages", or custom */
    /* Blocking. Emits events via cb (may be NULL), returns final assistant message
     * in *out (caller frees with pi_message_free). Network/LLM failures produce a
     * message with stop_reason PI_STOP_ERROR/PI_STOP_ABORTED and return PI_OK.
     * Contract: check opts->abort_flag BEFORE opening a connection and poll it
     * between reads (the built-in providers do, via pi_run_sse_post) — the agent
     * loop may call stream() with the flag already set (abort during a tool batch)
     * and relies on an immediate PI_STOP_ABORTED without network traffic. */
    int (*stream)(pi_env_t *env, const pi_model_t *model, const pi_context_t *ctx,
                  const pi_stream_options_t *opts, pi_ai_event_cb cb, void *user,
                  pi_message_t **out);
} pi_provider_t;

struct pi_env {
    const pi_alloc_t *alloc;    /* NULL ⇒ pi_alloc_default() */
    pi_transport_t *transport;  /* required for streaming */
    const pi_sys_t *sys;        /* NULL ⇒ single-threaded, no logging */
    const pi_fs_t *fs;          /* required for skills only */
    /* internal — filled by pi_env_init */
    const pi_provider_t *providers[PI_MAX_PROVIDERS];
    size_t provider_count;
};

/* Fill defaults and register built-in providers (openai-completions, anthropic-messages).
 * Call after setting alloc/transport/sys/fs fields. */
int pi_env_init(pi_env_t *env);
/* Register a custom provider (models with api == PI_API_CUSTOM select it by provider_id). */
int pi_env_register_provider(pi_env_t *env, const pi_provider_t *p);

/* ---------- entry points (≙ models.stream / models.complete) ---------- */
int pi_ai_stream(pi_env_t *env, const pi_model_t *model, const pi_context_t *ctx,
                 const pi_stream_options_t *opts, pi_ai_event_cb cb, void *user,
                 pi_message_t **out);
int pi_ai_complete(pi_env_t *env, const pi_model_t *model, const pi_context_t *ctx,
                   const pi_stream_options_t *opts, pi_message_t **out);

const pi_alloc_t *pi_env_alloc(const pi_env_t *env); /* resolved allocator */

/* ---------- compile-time capability self-description (DESIGN §12.1) ----------
 * B-tier features are compile-time options; query instead of guessing what was
 * built in. pi_features() reports exactly the PI_FEATURE_* flags this library
 * was compiled with. */
#define PI_FEATURE_BIT_IMAGES (1u << 0)       /* PI_BLOCK_IMAGE passthrough */
#define PI_FEATURE_BIT_SESSION (1u << 1)      /* pi_session jsonl persistence */
#define PI_FEATURE_BIT_PARTIAL_JSON (1u << 2) /* incremental tool-arg parsing */
#define PI_FEATURE_BIT_MODELS_JSON (1u << 3)  /* models.json catalog loader */
#define PI_FEATURE_BIT_COMPAT (1u << 4)       /* provider compat detection + request shaping */
#define PI_FEATURE_BIT_PARALLEL_TOOLS (1u << 5) /* concurrent tool-call batch execution */
#define PI_FEATURE_BIT_COMPACTION (1u << 6)     /* context summarization / compaction */
#define PI_FEATURE_BIT_SKILLS_IGNORE (1u << 7)  /* .gitignore/.ignore/.fdignore skill filtering */
#define PI_FEATURE_BIT_HARNESS_TOOLS (1u << 8)  /* read/write/edit tools + ExecutionEnv (host only) */
#define PI_FEATURE_BIT_HARNESS_EXEC (1u << 9)   /* bash tool + shell capture (implies HARNESS_TOOLS) */
#define PI_FEATURE_BIT_SUBAGENT (1u << 10)      /* in-process subagent pool + LLM tools */
#define PI_FEATURE_BIT_OAUTH (1u << 11)         /* subscription OAuth protocol + Claude Code wire identity */
uint32_t pi_features(void);

#ifdef __cplusplus
}
#endif
#endif /* PI_AI_H */
