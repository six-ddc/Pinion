/* pi-c — models.json catalog loader (PI_FEATURE_MODELS_JSON), pi-compatible shape:
 * {"providers":{name:{baseUrl,api,apiKey,models:[{id,name,contextWindow,maxTokens,
 *   input,reasoning,cost,thinkingLevelMap,headers,compat}]}}}.
 * C translation of pi's Models/Provider catalog (ts: packages/ai/src/models.ts,
 * Model in types.ts:666). Provider-level baseUrl/api/apiKey are the default for
 * every model under it; a model may override any of them with its own key (upstream
 * flat-Model field names are accepted as aliases).
 *
 * NOTE: `apiKey` (provider- or model-level, surfaced via pi_models_api_key) is a
 * pi-c extension to the catalog shape — upstream's Model interface has NO apiKey
 * field; the key travels through StreamOptions/env there. `name` and `headers` do
 * mirror upstream Model.name / Model.headers.
 * SPDX-License-Identifier: MIT */
#ifndef PI_MODELS_JSON_H
#define PI_MODELS_JSON_H
#include "pi_ai.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---------- full catalog (≙ Models collection) ---------- */
/* Opaque handle owning every parsed provider/model plus all backing strings,
 * thinkingLevelMap slots, and compat overrides. All pi_model_t pointers returned
 * from this catalog stay valid until pi_models_free. */
typedef struct pi_models_catalog pi_models_catalog_t;

/* Parse the whole models.json at `path` (read via env->fs) into a new catalog.
 * On PI_OK *out owns the catalog (free with pi_models_free). Returns
 * PI_ERR_NOT_FOUND (unreadable), PI_ERR_ARG (bad JSON / no providers), or
 * PI_ERR_NOMEM. */
int pi_models_load(pi_env_t *env, const char *path, pi_models_catalog_t **out);
void pi_models_free(pi_models_catalog_t *cat);

/* Runtime lookup (≙ Models.getModel; linear scan, models.ts:194-196). Matches on
 * provider_id + id; NULL if absent. Returned pointer valid for catalog lifetime. */
const pi_model_t *pi_models_get(const pi_models_catalog_t *cat, const char *provider_id,
                                const char *model_id);
/* Iteration over every model across all providers (≙ Models.getModels). */
size_t pi_models_count(const pi_models_catalog_t *cat);
const pi_model_t *pi_models_at(const pi_models_catalog_t *cat, size_t i);

/* Provider-level apiKey backing a catalog model (pi_model_t has no api_key field;
 * ≙ Provider.auth apiKey). NULL when the model isn't from `cat` or apiKey unset.
 * Valid for catalog lifetime. */
const char *pi_models_api_key(const pi_models_catalog_t *cat, const pi_model_t *m);

/* ---------- back-compat: first-model convenience ---------- */
typedef struct pi_model_loaded {
    pi_model_t model; /* string fields point into the storage below */
    char provider[64], id[128], base_url[256], api_key[256];
} pi_model_loaded_t;
/* Load the first provider's first model into a self-contained snapshot (no catalog
 * lifetime to manage). Semantically identical to loading the full catalog and
 * copying pi_models_at(cat, 0). Reads via env->fs. */
int pi_models_json_load_first(pi_env_t *env, const char *path, pi_model_loaded_t *out);

#ifdef __cplusplus
}
#endif
#endif
