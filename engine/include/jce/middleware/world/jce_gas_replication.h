/*
 * jce_gas_replication.h -- server-authoritative GAS attribute replication.
 *
 * Bridges the headless Gameplay Ability System (jce_gas.h) onto the generic
 * component snapshot substrate (jce_replication.h) so a server's live
 * attribute *current* values travel to every peer on the SAME wire path as
 * every other replicated component (delta stream, acked baseline,
 * late-joiner FULL burst, interest filtering — all for free).
 *
 * Design (one packed component per entity)
 * ----------------------------------------
 * flecs allows exactly one instance of a given component type per entity, so
 * N independent per-attribute scalars are impossible.  Instead a single
 * PACKED component — JceGasAttribRepl { count; values[32] } — carries the
 * whole attribute snapshot.  It is a GENERIC float-array payload: the net
 * layer stays GAS-agnostic (its registered write/read serializers touch only
 * the struct bytes), and the GAS<->component marshalling lives entirely on
 * the application side (the runtime, or a test).
 *
 * Authority model (mirrors jce_net_var_*)
 * ---------------------------------------
 *   - SERVER (or the owning client) is authoritative: it FILLS the component
 *     from its live GAS each tick; the substrate then replicates the bytes.
 *   - A remote (no-authority) peer's component is populated by the substrate
 *     read callback on snapshot apply; the application then pushes those
 *     server-authoritative values back into its local GAS via
 *     jce_gas_attribute_set_base().  The client does not fight the server.
 *
 * Gate
 * ----
 * Only an entity carrying a NetworkObject ever gets the component or
 * replicates.  An entity with no NetworkObject is byte-identical to today.
 *
 * Single registration path
 * ------------------------
 * jce_gas_replication_register(world) is called by BOTH the runtime net
 * bridge and headless tests, so the backing flecs component + its serializer
 * pair are registered exactly once on a given world (re-registration is
 * idempotent, matching jce_net_var_register_all()).
 *
 * Layer: middleware/world (Layer 4) -- public.  Depends on jce_gas.h (same
 * layer) and the public net replication registry (jce_replication.h).
 */
#ifndef JCE_GAS_REPLICATION_H
#define JCE_GAS_REPLICATION_H

#include <jce/middleware/world/jce_gas.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable wire name for the packed GAS attribute replica component.  Both
 * peers register it under this name so the substrate's u16 component
 * interning is identical on each side. */
#define JCE_GAS_ATTRIB_REPL_NAME "JceGasAttribRepl"

/* The packed, replicated GAS attribute payload.  `values[i]` holds the
 * server-authoritative *current* value of the i-th GAS attribute (serialized
 * in attribute-set index order so it is stable across peers).  `count` is the
 * number of valid entries (0..JCE_GAS_MAX_ATTRIBUTES). */
typedef struct JceGasAttribRepl {
    uint32_t count;
    float    values[JCE_GAS_MAX_ATTRIBUTES];
} JceGasAttribRepl;

/* Register the packed GAS attribute replica component with the replication
 * substrate.  Creates the backing flecs component (by stable name) on
 * `ecs_world` and wires its net-generic serializer pair.  `ecs_world` is the
 * SAME pointer passed to jce_net_replication_set_world() (the flecs world,
 * cast to void*).  Safe to call more than once (idempotent).  No-op for a
 * NULL world.  Call AFTER jce_net_var_register_all() so it does not disturb
 * the NetworkVariable registration order. */
/* Returns false when the GAS attribute replica component was NOT wired:
 * NULL world, or the flecs component could not be created.  CHECK IT — a
 * silent miss means attributes (health, stamina, ...) never replicate, and
 * the symptom is a client whose HP simply never changes. */
JCE_API bool JCE_CALL jce_gas_replication_register(void *ecs_world);

/* Authority-side fill: copy `gas`'s live attribute *current* values into the
 * entity's JceGasAttribRepl component (creating it if absent), in
 * attribute-set index order.  The substrate's registered write serializer
 * then ships the bytes on the next snapshot.  No-op (returns false) for a
 * NULL gas / unregistered component / no bound world.  The CALLER is
 * responsible for the authority gate (jce_net_object_has_authority) and the
 * NetworkObject presence gate — this function performs no net checks so the
 * world layer stays free of net-object policy.  `entity` is the flecs entity
 * cast to u64. */
JCE_API bool JCE_CALL
jce_gas_replication_fill_from_gas(uint64_t entity,
                                  const JceGameplayAbilitySystem *gas);

/* Remote-side apply: read the entity's JceGasAttribRepl component (populated
 * by the substrate read serializer on snapshot apply) and push each value
 * into `gas` via jce_gas_attribute_set_base() by matching the i-th attribute
 * name (index order is stable across peers, so this is a positional apply).
 * Returns true if a component was present and applied, false otherwise (no
 * component yet / NULL gas / unregistered).  The CALLER is responsible for
 * only invoking this for remote (no-authority) entities. */
JCE_API bool JCE_CALL
jce_gas_replication_apply_to_gas(uint64_t entity,
                                 JceGameplayAbilitySystem *gas);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GAS_REPLICATION_H */
