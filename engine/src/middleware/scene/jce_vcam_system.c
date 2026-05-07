/*
 * jce_vcam_system.c  ECS-driven Cinemachine-style camera resolver.
 */

#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/middleware/scene/jce_scene.h>

#include <math.h>
#include <string.h>

/* Singleton damping state. The editor only ever drives one game camera
 * so a static cache is enough. Reset on scene unload. */
typedef struct {
    int           initialised;
    JceVcamOutput cur;
} VcamState;

static VcamState s_state = { 0, { {0,0,0}, {0,0,0}, 60.0f } };

JCE_API void JCE_CALL
jce_vcam_system_reset(void)
{
    s_state.initialised = 0;
    s_state.cur.fov_deg = 60.0f;
}

/* Per-iteration scratch — using static ok since the callback is run
 * synchronously inside jce_scene_each_entity in a single thread. */
typedef struct {
    JceScene                  *scene;
    int                        have_winner;
    int32_t                    best_priority;
    JceVirtualCameraComponent  best;
} PickCtx;

static void pick_cb(JceScene *s, JceEntity e, void *user)
{
    PickCtx *ctx = (PickCtx *)user;
    if (!jce_scene_has_virtual_camera(s, e)) return;
    JceVirtualCameraComponent *vc = jce_scene_get_virtual_camera(s, e);
    if (!vc || !vc->active) return;
    if (ctx->have_winner && vc->priority <= ctx->best_priority) return;
    ctx->have_winner   = 1;
    ctx->best_priority = vc->priority;
    ctx->best          = *vc;
}

static void resolve_pose(JceScene                        *scene,
                          const JceVirtualCameraComponent *vc,
                          JceVcamOutput                   *desired)
{
    /* Defaults from static fields. */
    desired->position[0] = vc->position[0];
    desired->position[1] = vc->position[1];
    desired->position[2] = vc->position[2];
    desired->target  [0] = vc->look_at [0];
    desired->target  [1] = vc->look_at [1];
    desired->target  [2] = vc->look_at [2];
    desired->fov_deg     = vc->fov_deg > 0.0f ? vc->fov_deg : 60.0f;

    /* Follow target overrides position. */
    if ((vc->track_mode == JCE_VCAM_COMP_TRACK_FOLLOW ||
         vc->track_mode == JCE_VCAM_COMP_TRACK_FOLLOW_LOOK) &&
        vc->follow_target != 0) {
        JceTransform *t =
            jce_scene_get_transform(scene, (JceEntity)vc->follow_target);
        if (t) {
            desired->position[0] = t->position.x + vc->follow_offset[0];
            desired->position[1] = t->position.y + vc->follow_offset[1];
            desired->position[2] = t->position.z + vc->follow_offset[2];
        }
    }

    /* Look-at target overrides target. */
    if ((vc->track_mode == JCE_VCAM_COMP_TRACK_LOOK_AT ||
         vc->track_mode == JCE_VCAM_COMP_TRACK_FOLLOW_LOOK) &&
        vc->look_at_target != 0) {
        JceTransform *t =
            jce_scene_get_transform(scene, (JceEntity)vc->look_at_target);
        if (t) {
            desired->target[0] = t->position.x;
            desired->target[1] = t->position.y;
            desired->target[2] = t->position.z;
        }
    }
}

JCE_API void JCE_CALL
jce_vcam_system_evaluate(JceScene      *scene,
                          float          dt,
                          JceVcamOutput *out,
                          bool          *out_has_active)
{
    if (out_has_active) *out_has_active = false;
    if (!scene || !out) return;

    PickCtx ctx = { scene, 0, 0, {{0}} };
    jce_scene_each_entity(scene, pick_cb, &ctx);
    if (!ctx.have_winner) {
        s_state.initialised = 0; /* snap on next acquire */
        return;
    }

    JceVcamOutput desired;
    resolve_pose(scene, &ctx.best, &desired);

    /* Damping: exponential approach. damping in [0,1]; treat as a
     * "smoothness" factor where 0 = snap, 1 = ~1s half-life. */
    float damping = ctx.best.damping;
    if (damping < 0.0f) damping = 0.0f;
    if (damping > 1.0f) damping = 1.0f;

    if (!s_state.initialised || damping <= 0.0001f || dt <= 0.0f) {
        s_state.cur = desired;
        s_state.initialised = 1;
    } else {
        /* Per-frame blend factor: alpha = 1 - exp(-dt / tau)
         * with tau = damping (seconds). */
        float tau   = damping;
        float alpha = 1.0f - expf(-dt / tau);
        for (int i = 0; i < 3; ++i) {
            s_state.cur.position[i] +=
                (desired.position[i] - s_state.cur.position[i]) * alpha;
            s_state.cur.target[i] +=
                (desired.target[i]   - s_state.cur.target[i])   * alpha;
        }
        s_state.cur.fov_deg +=
            (desired.fov_deg - s_state.cur.fov_deg) * alpha;
    }

    *out = s_state.cur;
    if (out_has_active) *out_has_active = true;
}
