/* pi-c — umbrella header.
 * A pure-C agent library faithful to pi (@earendil-works/pi-ai + pi-agent-core).
 *
 * Versioning & ABI policy
 * -----------------------
 * - Pre-1.0: PI_VERSION tracks idf_component.yml. Source compatibility is the
 *   promise; there is NO binary-compatibility promise across versions — always
 *   rebuild the library and its users together.
 * - Public structs/vtables evolve by tail-append only (see pi_transport_t /
 *   pi_sys_t / pi_fs_t): existing positional or zero-filled initializers stay
 *   valid, a NULL tail field reads as "absent".
 * - Approved source-breaking exceptions (rebuild consumers against the new headers):
 *   0.2.0: pi_sys_t.now_ms/random_bytes gained a leading `void *ctx` parameter.
 * - PI_FEATURE_* flags must match between the library build and every consumer
 *   TU (the flags are compiled into struct behaviour; fields that used to be
 *   feature-gated are now unconditional precisely to avoid layout forks — see
 *   pi_ai_event_t.partial_args). Query what was built in via pi_features().
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_H
#define PI_H

#define PI_VERSION_MAJOR 0
#define PI_VERSION_MINOR 2
#define PI_VERSION_PATCH 0
#define PI_VERSION "0.2.0" /* keep in sync with idf_component.yml */

#include "pi_features.h"
#include "pi_port.h"
#include "pi_types.h"
#include "pi_compat.h"
#include "pi_ai.h"
#include "pi_agent.h"
#include "pi_subagent.h"
#include "pi_skills.h"
#include "pi_session.h"
#include "pi_models_json.h"
#include "pi_partial_json.h"
#include "pi_compaction.h"
#include "pi_retry.h"
#include "pi_exec.h"
#include "pi_harness.h"

#endif /* PI_H */
