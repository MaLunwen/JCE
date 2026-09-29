/*
 * jce_net_animator.h  Animator state replication.
 *
 * Unity's NetworkAnimator.  The authority broadcasts what its animator is
 * doing -- which state is playing, how far into it, at what speed, and the
 * parameter values driving the graph -- and receivers apply it.
 *
 * MODELLED ON jce_net_transform.h AND DELIBERATELY THE SAME SHAPE: same
 * register / unregister, same fixed_step + render_step pair, same tick-based
 * interpolation timeline, same authority modes.  A second networking
 * subsystem with its own vocabulary would be two things to learn for one idea.
 *
 * THREE THINGS MAKE ANIMATOR STATE DIFFERENT FROM A TRANSFORM, and each is a
 * bug if it is not handled:
 *
 *   1. NORMALISED TIME WRAPS.  A looping clip runs 0 -> 1 -> 0.  Lerping a
 *      snapshot at 0.95 towards one at 0.05 the way a position is lerped runs
 *      the animation BACKWARDS through the whole clip, once per loop, forever.
 *      The interpolator detects the wrap and goes forward through it.
 *
 *   2. A STATE CHANGE IS NOT A BLEND.  When two snapshots name different
 *      states there is no meaningful value between them -- 40% of the way from
 *      "idle" to "jump" is not a pose.  The interpolator SNAPS to the newer
 *      snapshot instead of interpolating.
 *
 *   3. A TRIGGER IS AN EDGE, NOT A VALUE.  The snapshot stream is UNRELIABLE
 *      by design -- a dropped position snapshot is corrected by the next one.
 *      A dropped trigger is a footstep that never fires.  Triggers therefore
 *      travel on their own RELIABLE packet and are never inferred from the
 *      state stream.  Unity splits them for the same reason.
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 *
 * Scheduling, exactly as for transforms:
 *   jce_net_animator_fixed_step(tick)  from JCE_PHASE_FIXED_UPDATE
 *   jce_net_animator_render_step(a)    from JCE_PHASE_UPDATE / PRE_RENDER
 *
 * Wire format (little-endian, channel JCE_NET_REPL_CHANNEL):
 *   state stream, UNRELIABLE, packet type 7:
 *     [ type u8 = 7 ][ server_tick u32 ][ count u16 ]
 *     count times:
 *       [ net_id u32 ][ state_hash u32 ][ normalized_time f32 ]
 *       [ speed f32 ][ param_count u8 ]
 *       param_count times: [ param_hash u32 ][ value f32 ]
 *   trigger, RELIABLE, packet type 9 (8 is the replication ACK):
 *     [ type u8 = 9 ][ net_id u32 ][ trigger_hash u32 ]
 */

#ifndef JCE_NET_ANIMATOR_H
#define JCE_NET_ANIMATOR_H

#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_NET_ANIMATOR_MAX_PARAMS        8u
#define JCE_NET_ANIMATOR_SNAPSHOT_HISTORY 16u

typedef enum {
    JCE_NET_ANIM_AUTHORITY_SERVER = 0,
    JCE_NET_ANIM_AUTHORITY_OWNER  = 1
} JceNetAnimatorAuthorityMode;

/* What one animator is doing, at one instant. */
typedef struct JceNetAnimatorState {
    /* The playing state / clip, as a hash of its name.  Hashed rather than
     * sent as a string because it goes out every snapshot: a name costs bytes
     * per tick forever, and receivers only ever compare it. */
    uint32_t state_hash;
    /* Position within the clip, 0..1, WRAPPING.  See the header note. */
    float    normalized_time;
    float    speed;
    uint8_t  param_count;
    uint32_t param_hash [JCE_NET_ANIMATOR_MAX_PARAMS];
    float    param_value[JCE_NET_ANIMATOR_MAX_PARAMS];
} JceNetAnimatorState;

typedef struct JceNetAnimatorSnapshot {
    uint32_t            server_tick;
    JceNetAnimatorState state;
} JceNetAnimatorSnapshot;

typedef struct JceNetAnimatorConfig {
    uint8_t                     snapshot_hz;     /* 0 -> 20 */
    uint16_t                    interp_delay_ms; /* 0 -> 100 */
    JceNetAnimatorAuthorityMode authority;
} JceNetAnimatorConfig;

/* FNV-1a over the name, and it is PUBLIC because both sides have to agree.
 * A caller that hashed state names its own way would produce ids this module
 * transports faithfully and the receiver never matches. */
JCE_API uint32_t JCE_CALL jce_net_animator_hash(const char *name);

JCE_API void JCE_CALL
jce_net_animator_set_default_config(const JceNetAnimatorConfig *cfg);
JCE_API void JCE_CALL
jce_net_animator_get_default_config(JceNetAnimatorConfig *out);

/* Bind the scene whose entities these ids resolve to.  NULL detaches. */
JCE_API void JCE_CALL jce_net_animator_set_scene(JceScene *scene);

/* Register / unregister.  Idempotent; a second register refreshes the config
 * in place, exactly as the transform layer's does. */
JCE_API bool JCE_CALL
jce_net_animator_register(JceNetObjectId id, const JceNetAnimatorConfig *cfg_or_null);
JCE_API void JCE_CALL jce_net_animator_unregister(JceNetObjectId id);

/* THE AUTHORITY PUSHES what its animator is doing, once per fixed step.  This
 * layer does not read an animator: the animation graph lives above it, and a
 * net module that reached up into L5 to sample one would invert the layering
 * that jce_net_transform.h is careful to keep. */
JCE_API void JCE_CALL
jce_net_animator_set_local_state(JceNetObjectId id, const JceNetAnimatorState *state);

/* A RECEIVER READS what to apply, after render_step has interpolated it.
 * Returns false for an unknown id or one that has received nothing yet --
 * which is NOT the same as "it is idle", and is why this returns a bool
 * rather than a zeroed state. */
JCE_API bool JCE_CALL
jce_net_animator_get_state(JceNetObjectId id, JceNetAnimatorState *out);

/* Fire a trigger.  RELIABLE and immediate -- not folded into the next
 * snapshot.  A trigger is an edge: the state stream is unreliable by design
 * and a dropped footstep never fires. */
JCE_API void JCE_CALL
jce_net_animator_fire_trigger(JceNetObjectId id, uint32_t trigger_hash);

/* Pop one received trigger for `id`, oldest first.  Returns false when the
 * queue is empty.  Queued rather than delivered by callback so the caller
 * drains them at a point it chooses, on its own thread. */
JCE_API bool JCE_CALL
jce_net_animator_poll_trigger(JceNetObjectId id, uint32_t *out_trigger_hash);

JCE_API void JCE_CALL jce_net_animator_fixed_step(uint32_t tick);
JCE_API void JCE_CALL jce_net_animator_render_step(double interp_alpha);

/* ── Stats / diagnostics ─────────────────────────────────────────── */
JCE_API uint32_t JCE_CALL jce_net_animator_registered_count(void);
JCE_API uint32_t JCE_CALL jce_net_animator_state_changes_count(void);
JCE_API uint32_t JCE_CALL jce_net_animator_triggers_sent_count(void);
JCE_API uint32_t JCE_CALL jce_net_animator_triggers_received_count(void);
JCE_API void     JCE_CALL jce_net_animator_reset_stats(void);

JCE_API void JCE_CALL jce_net_animator_shutdown(void);

/* TEST / LOOPBACK SEAM.  Inject a snapshot as though it had arrived from the
 * authority.  Present for the same reason the transform layer appends
 * server-side samples to the inbound ring: an in-process host and a test need
 * the receive path without a socket, and a receive path only ever exercised
 * through a socket is one nothing can assert about. */
JCE_API void JCE_CALL
jce_net_animator_inject_snapshot(JceNetObjectId id, uint32_t server_tick,
                                 const JceNetAnimatorState *state);

/* The same seam for a trigger: queue one as though it had arrived. */
JCE_API void JCE_CALL
jce_net_animator_inject_trigger(JceNetObjectId id, uint32_t trigger_hash);

JCE_EXTERN_C_END

#endif /* JCE_NET_ANIMATOR_H */
