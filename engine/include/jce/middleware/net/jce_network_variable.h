/*
 * jce_network_variable.h — typed property replication (FEATURE 7.2).
 *
 * A NetworkVariable is a typed, server-authoritative field whose value is
 * automatically replicated to every peer over the existing component
 * snapshot substrate (jce_replication.h).  It is the "last mile" that turns
 * the generic delta-capable per-component registry into an ergonomic,
 * per-field replicated property with:
 *
 *   - change-detection      (the substrate already diffs against a baseline
 *                            via memcmp; we skip unchanged values on encode),
 *   - authority / write     (only the authority — server, or the owning
 *     permission             client — may legally mutate the value; a
 *                            non-authority write is rejected),
 *   - an OnValueChanged      callback that fires exactly once when the value
 *     hook                   actually changes (on the authority when it
 *                            writes, and on a remote when a delta applies)
 *                            and NOT when an identical value re-arrives.
 *
 * Storage model
 * -------------
 * A NetworkVariable is backed by a real flecs component that lives on the
 * NetworkObject's entity.  The replication substrate reads/writes that
 * component through its registered (write/read) serializer pair, so the
 * value travels on the SAME wire path as every other replicated component
 * — delta stream, acked baseline, late-joiner FULL burst, and interest
 * filtering all apply for free.
 *
 * Production registration
 * -----------------------
 * jce_net_var_register_all() registers the built-in NetworkVariable
 * component types with the replication substrate.  It is called from the
 * runtime's net bridge bring-up so a SHIPPED build has
 * jce_net_replication_component_count() > 0 (the substrate previously
 * carried only spawn/despawn/ownership in production).
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 */

#ifndef JCE_NETWORK_VARIABLE_H
#define JCE_NETWORK_VARIABLE_H

#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Built-in NetworkVariable component types                            */
/* ================================================================== */

/* Stable wire names for the built-in replicated scalar components.  Both
 * peers register them in the same order so the substrate's u16 component
 * interning is identical on each side. */
#define JCE_NETVAR_F32_NAME  "JceNetVarF32"
#define JCE_NETVAR_I32_NAME  "JceNetVarI32"

/* A single replicated 32-bit float field.  This is the minimal "real"
 * replicated scalar component used to bring the substrate online in
 * production (comp_count > 0). */
typedef struct JceNetVarF32 {
    float value;
} JceNetVarF32;

/* A single replicated 32-bit signed integer field. */
typedef struct JceNetVarI32 {
    int32_t value;
} JceNetVarI32;

/* ================================================================== */
/* OnValueChanged callback                                             */
/* ================================================================== */

/* Fired when a NetworkVariable's value actually changes.  `entity` is the
 * backing flecs entity (cast to u64); `old_value` / `new_value` are the
 * previous and current values.  Never fires for an identical write. */
typedef void (*JceNetVarF32ChangedFn)(uint64_t entity,
                                      float    old_value,
                                      float    new_value,
                                      void    *user);
typedef void (*JceNetVarI32ChangedFn)(uint64_t entity,
                                      int32_t  old_value,
                                      int32_t  new_value,
                                      void    *user);

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

/* Register the built-in NetworkVariable component types with the
 * replication substrate.  Creates the backing flecs components on the
 * bound world (by stable name) and wires their serializer pairs.  Safe to
 * call more than once (re-registration is idempotent).  REQUIRES a world
 * bound via jce_net_replication_set_world() first.  Call from production
 * init so a shipped build carries replicated components. */
/* Returns false when the NetworkVariable components were NOT registered
 * (no world bound — call jce_net_replication_set_world() first).  CHECK IT:
 * on a miss every replicated float/int silently stops moving, and because
 * the registration order also fixes the wire component ids, a peer that
 * skipped it disagrees with one that did not. */
JCE_API bool JCE_CALL jce_net_var_register_all(void);

/* ================================================================== */
/* Typed access (float)                                                */
/* ================================================================== */

/* Read the current replicated float on `entity`.  Returns `fallback` if
 * the entity has no JceNetVarF32 component (or no world is bound). */
JCE_API float JCE_CALL
jce_net_var_f32_get(uint64_t entity, float fallback);

/* Authority-gated write.  Sets the replicated float on `entity` ONLY if
 * the caller has authority over the owning NetworkObject
 * (jce_net_object_has_authority): server always, client iff it owns the
 * object.  Returns true if the write was applied (value stored; if it
 * changed, dirty for the next snapshot and the OnValueChanged hook fires
 * locally exactly once), false if rejected (no authority / no world /
 * unknown entity).  Writing an identical value is a no-op success: it
 * does NOT fire the hook and does NOT re-send. */
JCE_API bool JCE_CALL
jce_net_var_f32_set(uint64_t entity, float value);

/* Register an OnValueChanged hook for a specific entity's float
 * NetworkVariable.  At most one hook per entity (a second call replaces
 * it).  Pass fn = NULL to clear.  The hook fires once whenever the value
 * changes — locally on an authoritative set and remotely when a delta
 * applies. */
JCE_API void JCE_CALL
jce_net_var_f32_on_changed(uint64_t entity, JceNetVarF32ChangedFn fn,
                           void *user);

/* ================================================================== */
/* Typed access (int32)                                                */
/* ================================================================== */

JCE_API int32_t JCE_CALL
jce_net_var_i32_get(uint64_t entity, int32_t fallback);

JCE_API bool JCE_CALL
jce_net_var_i32_set(uint64_t entity, int32_t value);

JCE_API void JCE_CALL
jce_net_var_i32_on_changed(uint64_t entity, JceNetVarI32ChangedFn fn,
                           void *user);

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Drop all registered OnValueChanged hooks (called on net teardown).
 * Does not touch the replication component registry. */
JCE_API void JCE_CALL jce_net_var_reset_hooks(void);

JCE_EXTERN_C_END

#endif /* JCE_NETWORK_VARIABLE_H */
