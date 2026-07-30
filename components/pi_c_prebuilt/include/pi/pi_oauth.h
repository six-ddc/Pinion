/* pi-c — subscription OAuth protocol layer (PI_FEATURE_OAUTH).
 * ≙ packages/ai/src/auth/oauth/{pkce.ts,anthropic.ts}
 *
 * Scope, deliberately narrow (DESIGN §12 #8, #91): this module obtains and refreshes
 * tokens. It does NOT store them and it does NOT drive a UI. Upstream splits the same
 * way — `OAuthAuth` only knows login/refresh, `CredentialStore` only knows storage,
 * and `resolveProviderAuth` stitches them (resolve.ts:102). In pi-c the stitching
 * belongs to the embedder: see examples/host_coder for a worked auth.json store and
 * a `/login` flow, and pi_agent_hooks_t.get_api_key (pi_agent.h) for the per-request
 * seam a refresh hangs off.
 *
 * Layering inside this header:
 *   - provider-agnostic:  pi_oauth_pkce_t / pi_oauth_credential_t / pi_oauth_provider_t
 *   - Anthropic-specific: everything named pi_oauth_anthropic_*
 * A second subscription provider (OpenAI Codex, GitHub Copilot, …) adds a file next to
 * src/extras/pi_oauth_anthropic.c and a pi_oauth_provider_t of its own; the generic
 * half and the embedder's storage/refresh plumbing are reused unchanged.
 *
 * Only the vtables in pi_port.h are used (transport POST, sys->random_bytes,
 * sys->now_ms), so this compiles and runs on every target — no sockets, no browser.
 * The interactive half of login (open a browser, take a pasted code) is the caller's.
 *
 * Billing note worth propagating to your users: a subscription token used from a
 * third-party harness draws on the account's extra usage and is billed per token, it
 * does NOT consume Claude plan limits.
 *
 * SPDX-License-Identifier: MIT */
#ifndef PI_OAUTH_H
#define PI_OAUTH_H

#include "pi_ai.h"
#include "pi_features.h"

#if PI_FEATURE_OAUTH

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- provider-agnostic ---------- */

/* PKCE pair. Both halves are base64url of 32 bytes ⇒ 43 chars + NUL (≙ pkce.ts:21). */
#define PI_OAUTH_PKCE_LEN 43
typedef struct pi_oauth_pkce {
    char verifier[PI_OAUTH_PKCE_LEN + 1];
    char challenge[PI_OAUTH_PKCE_LEN + 1];
} pi_oauth_pkce_t;

/* A token pair. Strings are owned; free with pi_oauth_credential_free.
 * `expires_ms` is epoch ms and ALREADY carries the 5-minute safety margin upstream
 * subtracts at mint time (anthropic.ts:224), so "expired" means expires_ms <= now. */
typedef struct pi_oauth_credential {
    char *access;
    char *refresh;
    uint64_t expires_ms;
} pi_oauth_credential_t;

/* Generate a PKCE verifier/challenge pair (S256). Requires sys->random_bytes;
 * PI_ERR_STATE when it is absent — never falls back to weak randomness, same rule as
 * pi_session_create. Returns PI_OK / PI_ERR_ARG / PI_ERR_STATE. */
int pi_oauth_pkce(const pi_sys_t *sys, pi_oauth_pkce_t *out);

void pi_oauth_credential_free(const pi_alloc_t *a, pi_oauth_credential_t *c);

/* What an embedder needs to keep a stored credential alive, without knowing which
 * subscription provider it came from. `id` doubles as the credential-store key.
 * `refresh` writes a fresh credential into *out (including a possibly-rotated refresh
 * token — persist it, or the next refresh fails) and returns PI_OK, PI_ERR_TRANSPORT,
 * PI_ERR_ARG or PI_ERR_NOMEM, filling `err` with a human-readable reason when non-NULL. */
typedef struct pi_oauth_provider {
    const char *id;
    int (*refresh)(pi_env_t *env, const pi_alloc_t *a, const char *refresh_token,
                   pi_oauth_credential_t *out, char *err, size_t errsz);
} pi_oauth_provider_t;

/* ---------- Anthropic (Claude Pro/Max) ---------- */

/* Buffer size that always fits pi_oauth_anthropic_authorize_url's output. */
#define PI_OAUTH_ANTHROPIC_URL_MAX 768

/* True for an OAuth access token as opposed to an API key: `sk-ant-oat*` vs
 * `sk-ant-api*` (≙ anthropic-messages.ts:840). The whole request-side identity switch
 * keys off this, so an embedder can enable subscription mode simply by handing the
 * agent an oat token as its api_key. */
bool pi_oauth_is_anthropic_token(const char *api_key);

/* Build the browser authorization URL for `pkce` into `buf`.
 * Two non-standard details are load-bearing and deliberate: `code=true` asks Anthropic
 * to render the `code#state` on the page rather than only redirecting, and `state` is
 * the verifier itself rather than an independent nonce — Anthropic's token endpoint
 * expects that (so state validation degenerates to `state == verifier`).
 * ≙ anthropic.ts:240-249. The redirect_uri is the console paste endpoint, not the
 * loopback one — see the note in pi_oauth_anthropic.c.
 * Returns PI_OK or PI_ERR_ARG (NULL args, or cap too small). */
int pi_oauth_anthropic_authorize_url(const pi_oauth_pkce_t *pkce, char *buf, size_t cap);

/* Parse whatever the user pasted back. Four accepted shapes (≙ anthropic.ts:52-78):
 *   1. a full redirect URL     https://console.anthropic.com/oauth/code/callback?code=X&state=Y
 *   2. code#state              X#Y        (what the console page shows)
 *   3. a bare query string     code=X&state=Y
 *   4. a bare code             X          (state is then filled in from `verifier`)
 * Leading/trailing whitespace is ignored. A state that is present but differs from
 * `verifier` is rejected with PI_ERR_ARG ("OAuth state mismatch"); a missing state is
 * backfilled with `verifier`. Returns PI_OK, or PI_ERR_ARG when no code could be found
 * or a buffer is too small. */
int pi_oauth_anthropic_parse_input(const char *input, const char *verifier, char *code,
                                   size_t code_cap, char *state, size_t state_cap);

/* Exchange an authorization code for a credential (POST console.anthropic.com — note
 * this is a DIFFERENT host from the authorize page; a 404 or an HTML body means the
 * request went to claude.ai by mistake). ≙ anthropic.ts:189-224.
 * On PI_OK *out owns two strings. On failure *out is untouched and `err` (when
 * non-NULL) holds the reason. Requires env->transport and env->sys->now_ms. */
int pi_oauth_anthropic_exchange(pi_env_t *env, const pi_alloc_t *a, const char *code,
                                const char *state, const char *verifier,
                                pi_oauth_credential_t *out, char *err, size_t errsz);

/* Refresh. The response's refresh token ROTATES — persist the new one or the session
 * dies a few hours later (≙ anthropic.ts:299-330). Same return contract as exchange. */
int pi_oauth_anthropic_refresh(pi_env_t *env, const pi_alloc_t *a, const char *refresh_token,
                               pi_oauth_credential_t *out, char *err, size_t errsz);

/* Registry entry for the generic half above. */
extern const pi_oauth_provider_t pi_oauth_anthropic;

#ifdef __cplusplus
}
#endif

#endif /* PI_FEATURE_OAUTH */
#endif /* PI_OAUTH_H */
