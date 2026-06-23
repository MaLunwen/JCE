/*
 * jce_net_transform.h — Snapshot interpolation + client prediction
 *                       foundation for replicated transforms (P3-D.4).
 *
 * Two-layer transform replication, modelled on Unity NGO's
 * NetworkTransform:
 *
 *   1) Authoritative snapshot stream.
 *      The authority (server for SERVER mode, owning client for OWNER
 *      mode) samples the scene transform of each registered
 *      NetworkObject every `snapshot_hz` and broadcasts a dedicated
 *      replication packet (type 5, JCE_REPL_PKT_NET_TRANSFORM).
 *
 *   2) Smooth interpolation on receivers.
 *      Snapshots land in a per-object 16-slot ring buffer keyed by the
 *      sender's server_tick.  Each visual frame, the client renders at
 *          render_tick = last_known_server_tick - interp_delay_ticks
 *      and lerps position / slerps rotation between the two snapshots
 *      straddling that tick.  Default interp_delay_ms = 100 (~ 2
 *      snapshot intervals at 20 Hz).
 *
 *   3) Client prediction (foundation only).
 *      For NetworkObjects the local client owns, gameplay writes the
 *      scene transform directly each fixed step (no interpolation,
 *      no wait for server roundtrip).  On snapshot arrival, this layer
 *      compares the authoritative pose to the client's current pose;
 *      if it diverges beyond `divergence_snap_distance` /
 *      `divergence_snap_angle_deg`, the transform is snapped to the
 *      server state and the snap counter is bumped.  Full input
 *      replay / rollback is intentionally out of scope (P3-D follow-up).
 *
 * Determinism: the interpolation timeline is tick-based, NOT wall-clock,
 * so it stays consistent with the B.2 fixed clock.  `interp_delay_ms`
 * is converted to ticks using the current fixed_hz at fixed-step time.
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 *
 * Scheduling:
 *   - jce_net_transform_fixed_step()  → call from JCE_PHASE_FIXED_UPDATE
 *     after physics + scene sim (so transforms reflect this tick).
 *   - jce_net_transform_render_step() → call from JCE_PHASE_UPDATE or
 *     JCE_PHASE_PRE_RENDER (after FixedUpdate, before render submit).
 *   The L4 substrate does not include the L5 player-loop header — the
 *   application is responsible for wiring these in (matches replication).
 *
 * Wire format (little-endian, channel JCE_NET_REPL_CHANNEL, UNRELIABLE):
 *   [ packet_type : u8  = 5 (JCE_REPL_PKT_NET_TRANSFORM) ]
 *   [ server_tick : u32 ]
 *   [ count       : u16 ]
 *   count times:
 *     [ net_id    : u32  ]
 *     [ pos_x/y/z : f32 × 3 ]
 *     [ rot_x/y/z/w : f32 × 4 ]
 *     [ vel_x/y/z : f32 × 3 ]
 *   ⇒ 7-byte header + 44 bytes per entry.  For 50 entities @ 20 Hz that
 *   is ~44 KB/s of server-out traffic; quantization is a follow-up.
 */

#ifndef JCE_NET_TRANSFORM_H
#define JCE_NET_TRANSFORM_H

#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_NET_TRANSFORM_SNAPSHOT_HISTORY 16u

typedef struct JceNetTransformSnapshot {
    uint32_t  server_tick;
    jce_vec3  position;
    jce_quat  rotation;
    jce_vec3  velocity;          /* optional — for extrapolation hold */
} JceNetTransformSnapshot;

typedef enum JceNetTransformAuthorityMode {
    JCE_NET_AUTH_SERVER = 0,     /* server owns transform; clients interp */
    JCE_NET_AUTH_OWNER  = 1      /* owner client sends up; others interp  */
} JceNetTransformAuthorityMode;

typedef struct JceNetTransformConfig {
    uint32_t snapshot_hz;                  /* default 20                  */
    uint32_t interp_delay_ms;              /* default 100                 */
    float    divergence_snap_distance;     /* metres; default 0.5         */
    float    divergence_snap_angle_deg;    /* degrees; default 30         */
    JceNetTransformAuthorityMode authority;
} JceNetTransformConfig;

/* Process-wide default config used when register() is called with NULL. */
JCE_API void JCE_CALL
jce_net_transform_set_default_config(const JceNetTransformConfig *cfg);

JCE_API void JCE_CALL
jce_net_transform_get_default_config(JceNetTransformConfig *out);

/* Bind the JceScene whose transforms this subsystem reads / writes.
 * May be NULL to detach (e.g. between scene loads).  Idempotent. */
JCE_API void JCE_CALL
jce_net_transform_set_scene(JceScene *scene);

/* Register a NetworkObject's transform for replication.  Idempotent —
 * a second call with the same id refreshes the config in place.  The
 * caller is responsible for first spawning the object via
 * jce_net_object_spawn() so the id resolves to a backing entity.
 *
 * Passing NULL for `cfg_or_null` uses the current default config. */
JCE_API bool JCE_CALL
jce_net_transform_register(JceNetObjectId id,
                           const JceNetTransformConfig *cfg_or_null);

/* Stop replicating this object's transform.  Silent no-op for unknown
 * ids.  Does NOT despawn the underlying NetworkObject. */
JCE_API void JCE_CALL
jce_net_transform_unregister(JceNetObjectId id);

/* Fixed-step hook.  Call once per FIXED_UPDATE tick after physics /
 * scene simulation has settled this tick's authoritative state.
 *   - SERVER role: samples local transforms, broadcasts a packet
 *     whenever (tick % (fixed_hz / snapshot_hz) == 0).
 *   - CLIENT role with OWNER authority: sends owned-object transforms
 *     to the server at the same cadence (TODO: real upstream channel;
 *     v1 broadcasts on JCE_NET_REPL_CHANNEL — server is the only
 *     legitimate receiver and other clients ignore packets whose
 *     net_id they don't own and aren't authoritative for).
 */
JCE_API void JCE_CALL
jce_net_transform_fixed_step(uint32_t tick);

/* Render-step hook.  Call each visual frame (UPDATE / PRE_RENDER).
 *   - Non-owned, non-authoritative objects: write the interpolated
 *     pose (computed at render_tick = last_known_server_tick -
 *     interp_delay_ticks) to the scene transform.
 *   - Owned objects: leave the scene transform alone (gameplay /
 *     prediction wrote it this tick); pending snap-corrections from
 *     the most recent inbound snapshot are applied here.
 *
 * `interp_alpha` is the fixed-clock alpha [0,1] — accepted for future
 * sub-tick visual interpolation but unused in v1 (interpolation is
 * already snapshot-tick based and naturally smooth).
 */
JCE_API void JCE_CALL
jce_net_transform_render_step(double interp_alpha);

/* ── Runtime-prediction coexistence (client-prediction wiring) ────────
 *
 * These two seams let the APPLICATION-layer runtime own client-side
 * prediction (rollback/replay via jce_net_prediction) for an entity it is
 * predicting, WITHOUT this net layer gaining any knowledge of prediction.
 * The net layer only learns "this owned object's transform is driven by the
 * runtime now (skip my snap-correction)" and "hand me the latest unconsumed
 * authoritative snapshot for it".  All prediction composition stays above L4.
 */

/* Mark / unmark an owned object as runtime-predicted.  While `predicted` is
 * true, jce_net_transform_render_step SKIPS its built-in owned-object snap-
 * correction for that object (the runtime applies the reconciled pose itself).
 * Non-predicted owned objects keep today's snap behaviour exactly.  Silent
 * no-op for an unregistered id (register the transform first). */
JCE_API void JCE_CALL
jce_net_transform_set_predicted(uint64_t entity, bool predicted);

/* Pop the latest UNCONSUMED authoritative snapshot for `entity` (the most
 * recent inbound server snapshot stashed for an owned/predicted object).  On
 * success copies its server_tick / position / rotation into the out params,
 * marks it consumed, and returns true.  Returns false when there is no pending
 * snapshot (or unknown id / NULL out-tick).  out_pos / out_rot may be NULL if
 * only one component is wanted.  This exposes exactly what the runtime needs to
 * call jce_prediction_reconcile, without the net layer knowing about it. */
JCE_API bool JCE_CALL
jce_net_transform_get_pending_auth(uint64_t  entity,
                                   uint32_t *out_tick,
                                   float     out_pos[3],
                                   float     out_rot[4]);

/* ── Stats / diagnostics ─────────────────────────────────────────── */

JCE_API uint32_t JCE_CALL jce_net_transform_registered_count(void);
JCE_API uint32_t JCE_CALL jce_net_transform_snap_corrections_count(void);

/* Reset stats counters (test helper). */
JCE_API void JCE_CALL jce_net_transform_reset_stats(void);

/* Tear-down — drops every registration and the scene binding.  Safe to
 * call more than once. */
JCE_API void JCE_CALL jce_net_transform_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_NET_TRANSFORM_H */
