/* pi-c — OpenAI-completions compatibility descriptor.
 * C translation of pi's OpenAICompletionsCompat / ResolvedOpenAICompletionsCompat
 * and detectCompat/getCompat (ts: packages/ai/src/types.ts:509-566,
 * packages/ai/src/api/openai-completions.ts:1384-1504, pin 5bc1c2c0).
 *
 * The struct/enum definitions here are always available (no feature gate) so a
 * caller can populate model->compat overrides unconditionally; only the
 * detect/resolve *implementations* (src/extras/pi_compat.c) are compiled under
 * PI_FEATURE_COMPAT, and only the openai-completions provider consumes them.
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_COMPAT_H
#define PI_COMPAT_H

#include "pi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reasoning/thinking request-parameter dialect (≙ OpenAICompletionsCompat.thinkingFormat).
 * The provider dispatches request-body assembly on this. Order is arbitrary but
 * fixed; switches over it must be exhaustive (project rule: -Werror=switch, no
 * default). */
typedef enum {
    PI_THINKFMT_OPENAI = 0,      /* reasoning_effort (top-level) */
    PI_THINKFMT_OPENROUTER,      /* reasoning: { effort } */
    PI_THINKFMT_DEEPSEEK,        /* thinking: { type } + reasoning_effort */
    PI_THINKFMT_TOGETHER,        /* reasoning: { enabled } + reasoning_effort */
    PI_THINKFMT_ZAI,             /* thinking: { type, clear_thinking } */
    PI_THINKFMT_QWEN,            /* top-level enable_thinking: bool */
    PI_THINKFMT_QWEN_CHAT_TEMPLATE, /* chat_template_kwargs.enable_thinking/preserve_thinking */
    PI_THINKFMT_CHAT_TEMPLATE,   /* configurable chat_template_kwargs */
    PI_THINKFMT_STRING_THINKING, /* top-level thinking: string */
    PI_THINKFMT_ANT_LING         /* reasoning: { effort } only when mapped effort non-null */
} pi_thinking_format_t;

/* Which field carries the output-token cap (≙ maxTokensField). */
typedef enum {
    PI_MAXTOK_COMPLETION = 0, /* max_completion_tokens (OpenAI standard) */
    PI_MAXTOK_MAX_TOKENS      /* max_tokens (legacy / non-standard providers) */
} pi_max_tokens_field_t;

/* Prompt-cache marker convention (≙ cacheControlFormat). */
typedef enum {
    PI_CACHECTL_NONE = 0,  /* provider-native / OpenAI prompt_cache_key path */
    PI_CACHECTL_ANTHROPIC  /* Anthropic-style cache_control blocks (openrouter anthropic/ models) */
} pi_cache_control_format_t;

/* Which session-affinity request headers to emit (≙ SessionAffinityFormat,
 * types.ts:108). Consumed by createClient (openai-completions.ts:646-655) only
 * when send_session_affinity_headers is set. The resolved value is never "auto":
 * detect() picks openrouter for OpenRouter endpoints, else openai. */
typedef enum {
    PI_SAF_OPENAI = 0,       /* session_id + x-client-request-id + x-session-affinity */
    PI_SAF_OPENAI_NOSESSION, /* x-client-request-id + x-session-affinity (no session_id) */
    PI_SAF_OPENROUTER        /* x-session-id only */
} pi_session_affinity_format_t;

/* Deferred-tools serialization mode (≙ deferredToolsMode, types.ts:557). Only "kimi"
 * exists upstream; NONE is the default. Under KIMI the openai-completions provider
 * filters tools already loaded by a tool-result's added_tool_names out of the request
 * `tools` array and re-injects them as a `{role:"system",tools:[...]}` message after
 * the tool results that named them. */
typedef enum {
    PI_DEFERRED_TOOLS_NONE = 0,
    PI_DEFERRED_TOOLS_KIMI
} pi_deferred_tools_mode_t;

/* Resolved compat: every field is a concrete value (≙ ResolvedOpenAICompletionsCompat).
 * Produced by pi_compat_detect (+ pi_compat_resolve overrides). */
typedef struct pi_compat {
    bool supports_store;
    bool supports_developer_role;
    bool supports_reasoning_effort;
    bool supports_usage_in_streaming;
    pi_max_tokens_field_t max_tokens_field;
    bool requires_tool_result_name;
    bool requires_assistant_after_tool_result;
    bool requires_thinking_as_text;
    bool requires_reasoning_content_on_assistant_messages;
    pi_thinking_format_t thinking_format;
    bool supports_strict_mode;
    pi_cache_control_format_t cache_control_format;
    bool supports_long_cache_retention;
    /* ≙ zaiToolStream: z.ai top-level `tool_stream: true` when the request carries
     * tools. Detect default false. */
    bool zai_tool_stream;
    /* ≙ sendSessionAffinityHeaders: emit session-affinity request headers from
     * options.session_id. openai sets session_id/x-client-request-id/x-session-affinity;
     * anthropic sets only x-session-affinity. Detect default false. */
    bool send_session_affinity_headers;
    /* ≙ supportsOpenAIGrammarTools (types.ts:549; detectCompat default false). When
     * TRUE a tool marked constrained_grammar serializes as a `custom` grammar tool on
     * the completions wire and its calls stream via custom.input. Enabled only via a
     * model.compat override — pi-c carries no built-in grammar-capable catalog. */
    bool supports_openai_grammar_tools;
    /* ≙ deferredToolsMode (detectCompat default undefined ⇒ NONE). See the enum. */
    pi_deferred_tools_mode_t deferred_tools_mode;
    /* --- tail-append region (ABI discipline: new fields go here, never mid-struct) --- */
    /* ≙ sessionAffinityFormat: which header set send_session_affinity_headers emits.
     * Detect default openrouter for OpenRouter endpoints, else openai. */
    pi_session_affinity_format_t session_affinity_format;
    /* ≙ supportsCacheControlOnTools (types.ts:609; resolved with `?? true` at
     * anthropic-messages.ts:180) — whether the provider accepts Anthropic-style
     * `cache_control` markers on TOOL definitions. Detect default TRUE; only an
     * explicit model.compat override turns it off (some Anthropic-compatible
     * providers, e.g. Fireworks, reject the field on tools). The anthropic-messages
     * provider is the consumer (anthropic-messages.ts:1005). */
    bool supports_cache_control_on_tools;
} pi_compat_t;

/* Tri-state for boolean overrides. UNSET is 0 so a `{0}`-initialised override
 * struct inherits every auto-detected value (differs from pi's `-1/0/1` in the
 * task note, chosen for C zero-init safety; see DESIGN §12 deviations). */
typedef enum { PI_TRI_UNSET = 0, PI_TRI_FALSE = 1, PI_TRI_TRUE = 2 } pi_tristate_t;

/* Enum overrides carry their own UNSET=0 sentinel for the same zero-init reason. */
typedef enum {
    PI_MAXTOK_OVR_UNSET = 0,
    PI_MAXTOK_OVR_COMPLETION,
    PI_MAXTOK_OVR_MAX_TOKENS
} pi_max_tokens_field_ovr_t;

typedef enum {
    PI_THINKFMT_OVR_UNSET = 0,
    PI_THINKFMT_OVR_OPENAI,
    PI_THINKFMT_OVR_OPENROUTER,
    PI_THINKFMT_OVR_DEEPSEEK,
    PI_THINKFMT_OVR_TOGETHER,
    PI_THINKFMT_OVR_ZAI,
    PI_THINKFMT_OVR_QWEN,
    PI_THINKFMT_OVR_QWEN_CHAT_TEMPLATE,
    PI_THINKFMT_OVR_CHAT_TEMPLATE,
    PI_THINKFMT_OVR_STRING_THINKING,
    PI_THINKFMT_OVR_ANT_LING
} pi_thinking_format_ovr_t;

typedef enum {
    PI_CACHECTL_OVR_UNSET = 0,
    PI_CACHECTL_OVR_NONE,
    PI_CACHECTL_OVR_ANTHROPIC
} pi_cache_control_format_ovr_t;

typedef enum {
    PI_SAF_OVR_UNSET = 0,
    PI_SAF_OVR_OPENAI,
    PI_SAF_OVR_OPENAI_NOSESSION,
    PI_SAF_OVR_OPENROUTER
} pi_session_affinity_format_ovr_t;

typedef enum {
    PI_DEFERRED_OVR_UNSET = 0,
    PI_DEFERRED_OVR_NONE,
    PI_DEFERRED_OVR_KIMI
} pi_deferred_tools_mode_ovr_t;

/* Override form (≙ Model.compat, the partial-object shape). Any UNSET field
 * inherits the value pi_compat_detect derived from provider_id/base_url. The
 * routing/kwargs object fields carry the upstream JSON verbatim (parsed only when
 * the provider assembles the request body); resolve() passes them through untouched
 * (≙ buildParams reading model.compat.* raw, not the resolved value). */
typedef struct pi_compat_override {
    pi_tristate_t supports_store;
    pi_tristate_t supports_developer_role;
    pi_tristate_t supports_reasoning_effort;
    pi_tristate_t supports_usage_in_streaming;
    pi_max_tokens_field_ovr_t max_tokens_field;
    pi_tristate_t requires_tool_result_name;
    pi_tristate_t requires_assistant_after_tool_result;
    pi_tristate_t requires_thinking_as_text;
    pi_tristate_t requires_reasoning_content_on_assistant_messages;
    pi_thinking_format_ovr_t thinking_format;
    pi_tristate_t supports_strict_mode;
    pi_cache_control_format_ovr_t cache_control_format;
    pi_tristate_t supports_long_cache_retention;
    /* ≙ zaiToolStream / sendSessionAffinityHeaders (both default false). */
    pi_tristate_t zai_tool_stream;
    pi_tristate_t send_session_affinity_headers;
    /* Raw JSON text of the upstream routing/kwargs object fields; NULL ⇒ the field
     * was not declared. The provider parses these at body-assembly time.
     *   open_router_routing_json   ≙ OpenRouterRouting → request `provider` field.
     *   vercel_gateway_routing_json ≙ VercelGatewayRouting → `providerOptions.gateway`.
     *   chat_template_kwargs_json  ≙ chatTemplateKwargs (thinkingFormat "chat-template"). */
    const char *open_router_routing_json;
    const char *vercel_gateway_routing_json;
    const char *chat_template_kwargs_json;
    /* ≙ forceAdaptiveThinking (anthropic-messages.ts:811,1024; default false via
     * `=== true`): when TRUE the anthropic-messages provider emits the adaptive
     * thinking payload — `thinking:{type:"adaptive",display:"summarized"}` +
     * `output_config:{effort}` (native xhigh/max effort) — and skips the legacy
     * budget path. UNSET/FALSE ⇒ legacy `thinking:{type:"enabled",budget_tokens}`
     * (pi-c has no built-in adaptive model catalog, so a custom model is never
     * adaptive-by-default; the built-in opt-out case is out of scope — DESIGN §12). */
    pi_tristate_t force_adaptive_thinking;
    /* ≙ supportsToolReferences (anthropic-messages.ts:184). Upstream defaults per
     * first-party model family; pi-c carries no built-in Anthropic catalog, so the
     * default is OFF (UNSET/FALSE ⇒ the normal tool list, byte-identical to today).
     * When TRUE the anthropic-messages provider partitions tools into immediate vs
     * deferred (splitDeferredTools predicate: a tool named by a tool-result's
     * addedToolNames and NOT yet used in the transcript is deferred), emits
     * `defer_loading:true` on deferred tools, and writes `tool_reference` blocks for
     * their markers (keeping ≥1 immediate tool when every tool is marked). */
    pi_tristate_t supports_tool_references;
    /* ≙ supportsStrictTools (anthropic-messages.ts:183, default false). When TRUE, a
     * tool whose def sets constrained_json_schema gets `strict:true` + the FULL
     * input_schema; UNSET/FALSE ⇒ no strict field.
     * NOTE: the TRIMMED input_schema {type,properties,required} is what a non-strict
     * tool gets ALWAYS — with or without compat, and in a COMPAT-OFF build — because
     * that trim is unconditional upstream (convertTools:1291-1305). This knob only
     * decides who gets the full schema back. DESIGN §12.2 #32 (R8 correction). */
    pi_tristate_t supports_strict_tools;
    /* ≙ supportsEagerToolInputStreaming (anthropic-messages.ts:177, upstream default
     * true — and pi-c follows it since R8): UNSET/TRUE ⇒ emit per-tool
     * `eager_input_streaming:true`; only an explicit FALSE omits it AND sends the
     * legacy `anthropic-beta: fine-grained-tool-streaming-2025-05-14` header when the
     * request carries tools. So this knob only ever DISABLES eager streaming.
     * DESIGN §12.2 #32 (R8 correction). */
    pi_tristate_t eager_input_streaming;
    /* ≙ supportsOpenAIGrammarTools (types.ts:575; detect default false). UNSET/FALSE ⇒
     * grammar tools fall back to plain function tools (byte-identical to today). */
    pi_tristate_t supports_openai_grammar_tools;
    /* ≙ deferredToolsMode (types.ts:557). UNSET inherits the detected default (NONE);
     * KIMI enables the Kimi system-tools split. */
    pi_deferred_tools_mode_ovr_t deferred_tools_mode;
    /* --- tail-append region (ABI discipline: new fields go here, never mid-struct) --- */
    /* ≙ sessionAffinityFormat: UNSET inherits the detected default. */
    pi_session_affinity_format_ovr_t session_affinity_format;
    /* ≙ supportsTemperature (anthropic-messages.ts:181, default true): when
     * FALSE, omit the `temperature` field for temperature-incompatible models.
     * UNSET keeps the current behavior of sending it (upstream default true). */
    pi_tristate_t supports_temperature;
    /* ≙ allowEmptySignature (anthropic-messages.ts:182, default false): when
     * TRUE, keep a thinking block with an empty signature (signature:"") instead
     * of converting it to plain text. UNSET/FALSE ⇒ convert to text (default). */
    pi_tristate_t allow_empty_signature;
    /* ≙ supportsCacheControlOnTools (types.ts:609, resolved `?? true`): UNSET keeps
     * the detected default TRUE (cache_control markers may ride on tool definitions);
     * FALSE omits them from tool params. Resolved into
     * pi_compat_t.supports_cache_control_on_tools. */
    pi_tristate_t supports_cache_control_on_tools;
    /* ---- openai-responses-only overrides (≙ OpenAIResponsesCompat, types.ts:565-580).
     * The openai-responses provider resolves its OWN 7-field compat from this struct
     * (its resolved defaults differ from the completions ones), so these two are read
     * by that provider alone; pi_compat_resolve ignores them. ---- */
    /* ≙ supportsToolSearch (types.ts:577; default false). TRUE ⇒ tools that a
     * tool-result introduced and that the transcript has not called yet are held back
     * out of `tools` and replayed as synthetic tool_search_call/tool_search_output
     * items at their load point. UNSET/FALSE ⇒ every tool is sent immediately. */
    pi_tristate_t supports_tool_search;
    /* ≙ supportsExplicitPromptCacheMode (types.ts:579; default false). TRUE ⇒ a
     * cache_retention of NONE additionally sends `prompt_cache_options:{mode:
     * "explicit"}` to switch the endpoint's implicit prompt cache off. */
    pi_tristate_t supports_explicit_prompt_cache_mode;
} pi_compat_override_t;

/* Auto-detect compat from provider id + base URL (≙ detectCompat). base_url NULL
 * is treated as "". Pure function; fills every field of *out. */
void pi_compat_detect(const char *provider_id, const char *base_url, pi_compat_t *out);

/* Resolve a model's effective compat (≙ getCompat): detect, then apply
 * model->compat overrides field-by-field, then OR the legacy
 * model->send_reasoning_content convenience bit into
 * requires_reasoning_content_on_assistant_messages. */
void pi_compat_resolve(const pi_model_t *model, pi_compat_t *out);

/* Cross-provider message transform (≙ transformMessages,
 * packages/ai/src/api/transform-messages.ts). Produces an owned array of
 * transformed message copies in *out_msgs / *out_count (the caller frees with
 * pi_transform_free); the source transcript is never mutated. Handles unsupported-
 * image downgrade, cross-model thinking demotion, Anthropic tool-call-id
 * normalisation (with matching tool_result remap), orphan tool-call backfill, and
 * dropping errored/aborted turns.
 *   target_api            = destination endpoint / protocol family name
 *                           ("anthropic-messages" / "openai-completions"), matched
 *                           against a message's origin_api.
 *   target_provider       = destination service id (model->provider_id, may be NULL),
 *                           matched against origin_provider.
 *   target_model_id       = model->id, matched against origin_model.
 * These three form the isSameModel triple (transform-messages.ts:92-95).
 *   has_image_input       = model accepts image input (else images are downgraded).
 *   normalize_anthropic_ids = normalise tool-call ids to Anthropic's charset/length.
 * Compiled only under PI_FEATURE_COMPAT. Returns PI_OK or PI_ERR_NOMEM. */
int pi_transform_messages(const pi_alloc_t *a, const pi_context_t *ctx, const char *target_api,
                          const char *target_provider, const char *target_model_id,
                          bool has_image_input, bool normalize_anthropic_ids,
                          pi_message_t ***out_msgs, size_t *out_count);
void pi_transform_free(const pi_alloc_t *a, pi_message_t **msgs, size_t count);

#ifdef __cplusplus
}
#endif
#endif /* PI_COMPAT_H */
