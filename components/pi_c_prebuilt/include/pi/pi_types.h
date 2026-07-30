/* pi-c — core data model. C translation of pi's types
 * (ts: packages/ai/src/types.ts — Context/Message/Content/Tool/Model).
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_TYPES_H
#define PI_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pi_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- content blocks (ts: TextContent/ThinkingContent/ToolCall) ---------- */
typedef enum { PI_BLOCK_TEXT, PI_BLOCK_THINKING, PI_BLOCK_TOOL_CALL, PI_BLOCK_IMAGE } pi_block_kind_t;

typedef struct pi_block {
    pi_block_kind_t kind;
    union {
        struct {
            char *text;
            /* ≙ TextContent.textSignature (openai-responses-shared.ts:48-72): an
             * opaque provider hint that lets a replayed assistant text block keep the
             * item id the endpoint minted for it. The openai-responses provider stores
             * the encoded v1 form `{"v":1,"id":"msg_…"[,"phase":"…"]}` here and, on the
             * next request, replays the message item under that id (and phase) instead
             * of a synthesized `msg_pi_<n>` fallback — which is what keeps OpenAI's
             * prompt-cache affinity across turns. A legacy bare id string is also
             * accepted on read. Owned copy; NULL = absent (tail-appended after `text`,
             * so a zero-initialized block keeps the current no-signature behavior; the
             * union's size is set by tool_call, which is wider). Providers other than
             * openai-responses ignore it. */
            char *signature;
        } text;
        struct {
            char *thinking;
            char *signature;     /* anthropic: must be echoed back */
            char *redacted_data; /* anthropic redacted_thinking opaque payload */
        } thinking;
        struct {
            char *data; /* base64 payload (PI_FEATURE_IMAGES) */
            char *mime; /* e.g. "image/jpeg" */
        } image;
        struct {
            char *id;
            char *name;
            char *arguments_json; /* full JSON text; during streaming = accumulated fragment */
            /* ≙ ToolCall.thoughtSignature (ts: types.ts:356): opaque serialized
             * reasoning-detail JSON that arrives on delta.reasoning_details (encrypted
             * reasoning blocks from OpenRouter/reasoning models) and MUST be replayed
             * verbatim under the assistant message's top-level reasoning_details array
             * on the next request, keyed to this tool call by its id — some providers
             * 400 otherwise (openai-completions.ts:547-560,1150-1162). Owned copy;
             * NULL = absent (tail-appended, so a zero-initialized block keeps the
             * current no-signature behavior). Dropped cross-model like Google's
             * thoughtSignature (transform-messages.ts:131-134). */
            char *thought_signature;
        } tool_call;
    } u;
} pi_block_t;

/* ---------- messages (ts: UserMessage | AssistantMessage | ToolResultMessage) ---------- */
/* The first three are the on-the-wire roles (ai/types.ts:377-406). The trailing
 * three are synthetic agent-message roles (≙ harness messages.ts CustomMessage /
 * BranchSummaryMessage / CompactionSummaryMessage): they only ever exist inside a
 * rebuilt transcript and are lowered to `user` by the providers before a request is
 * sent. Ordinals are appended so existing values are unchanged.
 * (messages.ts also defines a bashExecutionMessage role — that one is specific to the
 * coding-agent harness and is intentionally NOT modelled here; pi-c has no bash
 * harness, so an application that wants it can carry it as a PI_ROLE_CUSTOM message.) */
typedef enum {
    PI_ROLE_USER,
    PI_ROLE_ASSISTANT,
    PI_ROLE_TOOL_RESULT,
    PI_ROLE_CUSTOM,             /* ≙ CustomMessage: content passthrough as user */
    PI_ROLE_BRANCH_SUMMARY,     /* ≙ BranchSummaryMessage: wrapped as user */
    PI_ROLE_COMPACTION_SUMMARY  /* ≙ CompactionSummaryMessage: wrapped as user */
} pi_role_t;

typedef enum {
    PI_STOP_STOP,     /* "stop" */
    PI_STOP_LENGTH,   /* "length" */
    PI_STOP_TOOL_USE, /* "toolUse" */
    PI_STOP_ERROR,    /* "error" */
    PI_STOP_ABORTED   /* "aborted" */
} pi_stop_reason_t;

typedef struct pi_usage {
    uint32_t input, output, cache_read, cache_write;
    /* reasoning: subset of `output` (already counted there); 0 when the provider
     * reports no breakdown. cache_write_1h: subset of `cache_write` written with 1h
     * retention (only Anthropic splits it). ≙ Usage (ts: types.ts:352). */
    uint32_t reasoning;
    uint32_t cache_write_1h;
    uint32_t total_tokens;
    /* $ amounts, filled by pi_usage_finalize from the model's pricing. */
    struct {
        double input, output, cache_read, cache_write, total;
    } cost;
} pi_usage_t;

typedef struct pi_message {
    pi_role_t role;

    /* user / assistant content */
    pi_block_t *blocks;
    size_t block_count;

    /* role == PI_ROLE_TOOL_RESULT */
    char *tool_call_id;
    char *tool_name;
    char *tool_output;
    bool tool_is_error;

    /* role == PI_ROLE_ASSISTANT */
    pi_usage_t usage;
    pi_stop_reason_t stop_reason;
    char *error_message; /* set when stop_reason == PI_STOP_ERROR; may be NULL if
                          * even the message copy OOMed (fail path allocates it) */
    /* origin (≙ pi stamping api/provider/model on AssistantMessage): which endpoint
     * produced this message; basis for cross-provider compat decisions (was
     * deviation #14). Owned copies; NULL on non-assistant messages.
     * origin_provider ≙ AssistantMessage.provider = the SERVICE id (e.g. "fireworks",
     * "openrouter"); == model->provider_id, NULL for hand-built models. The protocol
     * family lives in origin_api below. */
    char *origin_provider; /* e.g. "fireworks" (service id); NULL when unknown */
    char *origin_model;    /* e.g. "deepseek-v4-pro" */
    /* provider-reported identifiers (≙ AssistantMessage.responseId/responseModel):
     * response_id = upstream response/message id; response_model = concrete model
     * echoed by the endpoint when it differs from the requested model->id (gateways
     * like OpenRouter "auto"). Owned copies; NULL when the API doesn't expose them. */
    char *response_id;
    char *response_model;
    /* ≙ AssistantMessage.api (types.ts:386): the protocol family the message was
     * produced with ("openai-completions"/"anthropic-messages"), independent of
     * origin_provider (the service id). Together with origin_provider + origin_model
     * this forms the isSameModel triple (transform-messages.ts:92-95). Owned; NULL on
     * non-assistant messages or hand-built history. */
    char *origin_api;

    /* ---- synthetic-role payload (PI_ROLE_CUSTOM/BRANCH_SUMMARY/COMPACTION_SUMMARY).
     * The role's text/content lives in `blocks`: summary roles store the bare summary
     * as a single TEXT block (the PREFIX/SUFFIX wrap is added only at request time by
     * the providers); custom stores its content as TEXT/IMAGE blocks. These fields
     * carry the remaining metadata. All NULL/0 on the on-the-wire roles. ---- */
    char *custom_type;      /* ≙ CustomMessage.customType (owned) */
    bool display;           /* ≙ CustomMessage.display */
    char *from_id;          /* ≙ BranchSummaryMessage.fromId (owned) */
    uint32_t tokens_before; /* ≙ CompactionSummaryMessage.tokensBefore */

    /* ≙ Message.timestamp (ai/types.ts): epoch-ms the entry was produced, carried
     * so the token estimator can drop a stale assistant `usage` when a LATER message
     * (e.g. a compaction summary) sits BEFORE it in the array (getLastAssistantUsageInfo
     * usageAppliesToPrefix). Scalar, not owned. 0 = unknown (live/hand-built messages
     * with no session origin): the forward-scan treats 0 as the earliest possible time,
     * so an all-zero array reproduces the old scan-from-end behaviour exactly. Stamped
     * from the session entry's timestamp on transcript reconstruction. */
    uint64_t timestamp_ms;

    /* ≙ ToolResultMessage.addedToolNames (ai/types.ts; deferred-tools.ts): names of
     * tools that this tool-result dynamically introduced into the transcript (a coding-
     * agent harness concern; pi-c has a fixed tool set so the PRODUCING side is an app
     * concern — the app sets these). On the Anthropic wire side (anthropic-messages.ts
     * convertToolResult/splitDeferredTools), when the model's compat declares
     * supportsToolReferences, each still-deferred, not-yet-loaded name here is emitted as
     * a `{type:"tool_reference",tool_name}` block on this tool_result and the definition is
     * marked `defer_loading:true` in the tools array. Owned array of owned strings; NULL/0
     * = none = current behavior. Only set on PI_ROLE_TOOL_RESULT messages. */
    char **added_tool_names;
    size_t added_tool_names_count;
} pi_message_t;

/* True for a synthetic (non-wire) role — custom / branchSummary / compactionSummary.
 * These only exist inside a rebuilt transcript and are lowered to `user` before a
 * request is sent. */
static inline bool pi_role_is_synthetic(pi_role_t role) {
    return role == PI_ROLE_CUSTOM || role == PI_ROLE_BRANCH_SUMMARY ||
           role == PI_ROLE_COMPACTION_SUMMARY;
}

/* ---------- tool definition (ts: Tool; parameters = JSON-Schema) ---------- */
typedef struct pi_tool_def {
    const char *name;
    const char *description;
    const char *parameters_schema_json; /* hand-written JSON-Schema (esp-claw style) */
    /* ≙ Tool.constrainedSampling with type "json_schema" (constrained-sampling.ts:84
     * resolveJsonSchemaStrictSampling): when TRUE and the model's compat declares
     * supportsStrictTools, the anthropic-messages provider emits `strict:true` plus the
     * FULL input_schema (verbatim parameters_schema_json, keeping additionalProperties/
     * title) for this tool. Every OTHER tool gets the legacy stripped schema
     * {type,properties,required} — unconditionally, compat or not, since R8 aligned the
     * trim with upstream's convertTools:1291-1305 (DESIGN §12.2 #32). Tail-appended, so
     * a zero-initialised tool sends the trimmed schema with no strict field. */
    bool constrained_json_schema;
    /* ≙ ConstrainedSamplingConfig {type:"json_schema", strict:"require"} vs "prefer"
     * (constrained-sampling.ts:84 resolveJsonSchemaStrictSampling). Only meaningful
     * when constrained_json_schema is set. On the openai-completions wire, when the
     * model's compat clears supports_strict_mode, a "require" tool makes the request
     * fail (`Tool "X" requires JSON-schema constrained sampling, but strict tools are
     * unsupported.`); a "prefer" tool just drops strict silently. Tail-appended, so a
     * zero-initialised json-schema tool behaves as "prefer". */
    bool constrained_strict_require;
    /* ≙ ConstrainedSamplingConfig {type:"grammar", variants} (constrained-sampling.ts:
     * 101 resolveGrammarConstrainedSampling; openai-completions.ts convertTools:1283).
     * When TRUE this tool is a grammar-constrained tool: if the model's compat sets
     * supports_openai_grammar_tools it serializes on the openai-completions wire as a
     * `custom` tool ({type:"custom",custom:{name,description,format:{type:"grammar",
     * grammar:{syntax,definition}}}}) and its calls stream via
     * delta.tool_calls[].custom.input (a raw grammar string) rather than
     * function.arguments; the parameters schema MUST have exactly one required string
     * property (the grammar input property). openai_lark is preferred over
     * openai_regex. When supports_openai_grammar_tools is off it falls back to a plain
     * function tool. Both variant strings are borrowed (caller-owned); NULL/blank ⇒
     * that variant is absent (an empty-variant grammar tool fails the request, ≙ "no
     * supported grammar variant was provided").
     * The openai-responses provider implements the same feature with a DIFFERENT wire
     * shape — {type:"custom",name,description,format:{type:"grammar",syntax,definition}},
     * i.e. no `custom:{}` wrapper and no extra `grammar:{}` level — and streams its calls
     * through custom_tool_call_input.delta / replays them as custom_tool_call items.
     * Tail-appended. */
    bool constrained_grammar;
    const char *constrained_grammar_lark;  /* ≙ variants.openai_lark */
    const char *constrained_grammar_regex; /* ≙ variants.openai_regex */
} pi_tool_def_t;

/* ---------- request context (ts: Context) ---------- */
typedef struct pi_context {
    const char *system_prompt; /* may be NULL */
    pi_message_t **messages;
    size_t message_count;
    const pi_tool_def_t *tools; /* may be NULL */
    size_t tool_count;
} pi_context_t;

/* ---------- model (ts: Model<TApi>; no generated catalogs — caller provides) ---------- */
/* Endpoint / protocol family. PI_API_OPENAI_RESPONSES is TAIL-APPENDED after
 * PI_API_CUSTOM on purpose: the project's ABI discipline forbids shifting an
 * existing ordinal, so "custom last" loses to ordinal stability. Switches over this
 * enum must stay exhaustive (project rule: -Werror=switch, no default label). */
typedef enum {
    PI_API_OPENAI_COMPLETIONS,
    PI_API_ANTHROPIC_MESSAGES,
    PI_API_CUSTOM,
    PI_API_OPENAI_RESPONSES /* ≙ client.responses.create (POST <base_url>/responses) */
} pi_api_t;

/* Input modalities the model accepts (≙ Model.input: ("text"|"image")[]). A model
 * with input_caps == 0 is treated as text-only, so existing callers stay valid. */
enum { PI_INPUT_TEXT = 1u << 0, PI_INPUT_IMAGE = 1u << 1 };

/* Compat override descriptor (≙ Model.compat, the partial-override form). Fully
 * defined in pi_compat.h; consumed only by the openai-completions provider when
 * built with PI_FEATURE_COMPAT. NULL ⇒ pure provider/URL auto-detection. */
struct pi_compat_override;

/* ≙ ModelCostRates: $/million tokens. */
typedef struct pi_model_cost_rates {
    double input, output, cache_read, cache_write;
} pi_model_cost_rates_t;

/* ≙ ModelCostTier: rates that replace the model's base rates for a whole request
 * once its input usage exceeds `input_tokens_above` (a long-context surcharge, say).
 * Kept as a double to mirror the JSON number exactly, negatives included. */
typedef struct pi_model_cost_tier {
    pi_model_cost_rates_t rates;
    double input_tokens_above;
} pi_model_cost_tier_t;

typedef struct pi_model {
    const char *id;       /* e.g. "gpt-4o-mini", "claude-sonnet-5" */
    pi_api_t api;
    const char *base_url; /* e.g. "https://api.openai.com/v1" (no trailing slash) */
    const char *provider_id; /* only for PI_API_CUSTOM: id of a registered provider */
    int max_tokens;          /* default output cap; 0 ⇒ provider default */
    int context_window;      /* informational */
    bool reasoning;          /* supports thinking / reasoning_content */
    /* compat (≙ pi Model.compat, e.g. DeepSeek V4's
     * requiresReasoningContentOnAssistantMessages): echo reasoning_content back
     * on assistant history messages. */
    bool send_reasoning_content;
    /* pricing in $/million tokens (≙ Model.cost); all-zero ⇒ pi_usage_finalize
     * leaves cost at 0. */
    pi_model_cost_rates_t cost;
    uint32_t input_caps;                    /* PI_INPUT_* bitmask; 0 ⇒ text-only */
    /* ≙ Model.thinkingLevelMap: per-level reasoning-effort string overrides,
     * indexed by pi_thinking_t (NULL entry ⇒ inherit the level's default string).
     * May be NULL.
     * EMPTY-STRING SENTINEL (openai-responses only, PI_THINK_OFF slot): upstream's
     * `thinkingLevelMap.off` is tri-valued — undefined ⇒ send effort "none", a string
     * ⇒ send that string, and explicit `null` ⇒ send NO reasoning field at all (the
     * gpt-5-pro family). C cannot distinguish "absent" from "null" with one pointer,
     * so thinking_level_map[PI_THINK_OFF] == "" encodes upstream's `null`: the
     * openai-responses provider then omits the whole `reasoning` object for
     * thinking == OFF. NULL keeps the "none" default (DESIGN §12 deviation 49b). */
    const char *const *thinking_level_map;
    const struct pi_compat_override *compat; /* ≙ Model.compat overrides; NULL ⇒ auto-detect */
    /* ≙ Model.name: human display name / capability-matching label; NOT sent in the
     * request body. NULL ⇒ absent. */
    const char *name;
    /* ≙ Model.headers: per-model custom HTTP headers as "Key: Value" strings (same
     * representation as pi_stream_options_t.extra_headers). Merged with override
     * (last-wins, case-insensitive) semantics by the providers. NULL ⇒ none. */
    const char *const *headers;
    size_t header_count;
    /* ≙ ModelCost.tiers: request-wide pricing tiers, tail-appended so existing
     * designated initializers stay valid. NULL/0 ⇒ the base rates always apply. */
    const pi_model_cost_tier_t *cost_tiers;
    size_t cost_tier_count;
} pi_model_t;

/* ---------- stream options (ts: StreamOptions/SimpleStreamOptions, trimmed) ---------- */
/* ≙ ModelThinkingLevel = "off"|ThinkingLevel (ts: types.ts:79). Ordinals track
 * EXTENDED_THINKING_LEVELS ["off","minimal","low","medium","high","xhigh","max"]
 * (ts: models.ts:661) so thinking_level_map is indexed by this enum.
 * ABI note: PI_THINK_MAX is tail-appended (like earlier levels); adding a level
 * shifts no existing ordinal, so this is source-compatible pre-1.0 (no ABI
 * promise; see DESIGN §12 deviation 49). "max" is the ceiling above "xhigh". */
typedef enum {
    PI_THINK_OFF = 0, PI_THINK_MINIMAL, PI_THINK_LOW,
    PI_THINK_MEDIUM, PI_THINK_HIGH, PI_THINK_XHIGH, PI_THINK_MAX
} pi_thinking_t;

/* Prompt-cache retention window (≙ CacheRetention, ts: types.ts:96). SHORT is 0
 * so a bare {0} stream-options resolves to "short", matching pi's default. */
typedef enum { PI_CACHE_SHORT = 0, PI_CACHE_NONE, PI_CACHE_LONG } pi_cache_retention_t;

/* ≙ OpenAICompletionsOptions.toolChoice / ChatCompletionToolChoiceOption
 * (ts: openai-completions.ts:142, wire at :734-736). UNSET is 0 so a
 * zero-initialized options block omits `tool_choice` entirely and keeps the
 * previous request bytes; upstream likewise only sends the field when the
 * caller set it (`if (options?.toolChoice)`). The other values map verbatim:
 *   AUTO     ⇒ "auto"      (model decides; the API default)
 *   NONE     ⇒ "none"      (tools visible, calling them forbidden)
 *   REQUIRED ⇒ "required"  (the model must call some tool)
 *   FUNCTION ⇒ {"type":"function","function":{"name":<tool_choice_function>}}
 * FUNCTION with a NULL/empty tool_choice_function is unexpressible upstream and
 * omits the field. The value is sent independently of whether the request
 * carries tools (upstream applies it after the tools block, ts:734). */
typedef enum {
    PI_TOOL_CHOICE_UNSET = 0,
    PI_TOOL_CHOICE_AUTO,
    PI_TOOL_CHOICE_NONE,
    PI_TOOL_CHOICE_REQUIRED,
    PI_TOOL_CHOICE_FUNCTION
} pi_tool_choice_t;

typedef struct pi_stream_options {
    const char *api_key;
    float temperature;
    bool has_temperature;
    int max_tokens; /* 0 ⇒ model->max_tokens ⇒ provider default */
    pi_thinking_t thinking;
    volatile bool *abort_flag; /* ≙ AbortSignal; polled between reads */
    const char *const *extra_headers; /* "Key: Value" strings */
    size_t extra_header_count;
    /* prompt caching (≙ StreamOptions.cacheRetention/sessionId); read only when
     * built with PI_FEATURE_COMPAT. session_id feeds prompt_cache_key; NULL ⇒
     * omitted. cache_retention defaults to PI_CACHE_SHORT. */
    const char *session_id;
    pi_cache_retention_t cache_retention;
    /* provider request retry (≙ retryProviderRequest, provider-retry.ts). All three
     * are tail-appended so a zero-initialized options block keeps the current
     * single-shot behavior byte-identical. max_retries == 0 ⇒ one attempt, no
     * retry (default). When > 0 the built-in providers wrap the POST in an
     * abort-interruptible retry loop that honors retryable statuses
     * (408/409/429/>=500) and the `retry-after`/`retry-after-ms`/`x-should-retry`
     * response headers, else exponential backoff (0.5*2^n s, capped at 8 s, with
     * ≤25% negative jitter; when sys->random_bytes is absent or fails the jitter is
     * 0 and the backoff takes the interval's upper bound — still inside upstream's
     * [0.75x, 1x] range). A server-requested delay above the resolved cap fails
     * immediately (≙ validateServerRetryDelayMs).
     *
     * Cap resolution mirrors upstream's `maxRetryDelayMs ?? 60_000` plus its
     * "set it to zero to disable the limit": has_max_retry_delay_ms == false (the
     * zero-initialized default) ⇒ 60000 ms, exactly like upstream's `undefined`;
     * has_max_retry_delay_ms == true ⇒ max_retry_delay_ms verbatim, and 0 then
     * means "no limit" (any server-requested delay is honored). The extra flag
     * exists because 0 cannot serve as both the unset default and "disabled"
     * (same idiom as has_temperature above).
     *
     * Retry needs both the transport's response_header slot (for retry-after;
     * absent ⇒ backoff-only) and sys->sleep_ms (absent ⇒ single-shot fallback). */
    int max_retries;
    uint32_t max_retry_delay_ms;
    /* ≙ OpenAICompletionsOptions.toolChoice; UNSET ⇒ the field is omitted. */
    pi_tool_choice_t tool_choice;
    const char *tool_choice_function; /* only read for PI_TOOL_CHOICE_FUNCTION */
    bool has_max_retry_delay_ms;      /* see the cap-resolution note above */
    /* Distinguishes an explicit PI_CACHE_SHORT from the zero-initialised default, so
     * PI_CACHE_RETENTION=long can fill in only the latter (≙ `cacheRetention ?? env`).
     * false (the default) keeps the previous behavior for every existing caller. */
    bool has_cache_retention;
} pi_stream_options_t;

/* ---------- message lifecycle (all strings copied; owned by the message) ---------- */
pi_message_t *pi_message_new(const pi_alloc_t *a, pi_role_t role);
pi_message_t *pi_message_user_text(const pi_alloc_t *a, const char *text);
pi_message_t *pi_message_tool_result(const pi_alloc_t *a, const char *tool_call_id,
                                     const char *tool_name, const char *output, bool is_error);
/* Append a block; returns its index or <0 on OOM. Strings are copied (NULL ok). */
int pi_message_add_text(const pi_alloc_t *a, pi_message_t *m, const char *text);
int pi_message_add_thinking(const pi_alloc_t *a, pi_message_t *m, const char *thinking,
                            const char *signature);
int pi_message_add_image(const pi_alloc_t *a, pi_message_t *m, const char *base64_data,
                         const char *mime);
int pi_message_add_tool_call(const pi_alloc_t *a, pi_message_t *m, const char *id,
                             const char *name, const char *arguments_json);
pi_message_t *pi_message_clone(const pi_alloc_t *a, const pi_message_t *m);
void pi_message_free(const pi_alloc_t *a, pi_message_t *m);

/* Set (replace) a TEXT block's signature (≙ TextContent.textSignature). `idx` must
 * address an existing PI_BLOCK_TEXT block; sig == NULL clears it. Returns PI_OK,
 * PI_ERR_ARG (bad index / not a text block) or PI_ERR_NOMEM (previous value kept). */
int pi_message_set_text_signature(const pi_alloc_t *a, pi_message_t *m, size_t idx,
                                  const char *sig);

/* Set a tool-result message's added_tool_names (≙ ToolResultMessage.addedToolNames):
 * replaces any existing list with owned copies of `names[0..count)`. Returns PI_OK, or
 * PI_ERR_NOMEM (message left with the old list intact). count==0 / names==NULL clears it. */
int pi_message_set_added_tool_names(const pi_alloc_t *a, pi_message_t *m,
                                    const char *const *names, size_t count);

/* Convenience: concatenated text of all TEXT blocks (allocated; caller frees). */
char *pi_message_text(const pi_alloc_t *a, const pi_message_t *m);

/* Fill usage->total_tokens (input+output+cache_read+cache_write when still 0;
 * reasoning is excluded — it is a subset of output) and usage->cost from the
 * model's per-token pricing (≙ calculateCost, ts: models.ts:385). Anthropic bills
 * 1h cache writes at 2x base input, hence the cache_write_1h split. A NULL model or
 * all-zero pricing leaves cost at 0. Both providers route their usage through this
 * one entry point, so tier selection needs no per-provider wiring. */
void pi_usage_finalize(pi_usage_t *usage, const pi_model_t *model);

/* The rates that apply to a request whose total input usage (input + cache_read +
 * cache_write) is `input_tokens` — the highest tier whose threshold it exceeds, or
 * the model's base rates when none does (≙ the tier loop in calculateCost,
 * models.ts:639-647). NULL model ⇒ all-zero rates. Exposed so callers that price a
 * request outside pi_usage_finalize resolve tiers the same way. */
pi_model_cost_rates_t pi_model_cost_rates(const pi_model_t *model, double input_tokens);

#ifdef __cplusplus
}
#endif
#endif /* PI_TYPES_H */
