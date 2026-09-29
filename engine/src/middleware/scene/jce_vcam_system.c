/*
 * jce_vcam_system.c  ECS-driven Cinemachine-style camera resolver.
 */

#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_camera_shake.h>

#include <math.h>
#include <stdio.h>   /* snprintf, for the active-name override */
#include <string.h>

/* Singleton damping state. The editor only ever drives one game camera
 * so a static cache is enough. Reset on scene unload. */
typedef struct {
    int           initialised;
    JceVcamOutput cur;
    /* The name that beats priority.  Empty = nobody asked, which is every
     * scene written before this and the state jce_vcam_system_reset()
     * returns to.
     *
     * It is HERE rather than on the component because the obvious
     * implementation -- raise the chosen camera's `priority` -- writes
     * gameplay state into the authored scene: the editor marks it dirty,
     * Ctrl+S bakes a cutscene's camera choice into the level, and ending the
     * cutscene needs the old numbers remembered from somewhere.  It is inside
     * VcamState rather than beside it so this file's file-scope mutable count
     * does not grow: it is the same singleton, not a second one. */
    char          active_name[64];
} VcamState;

static VcamState s_state = { 0, { {0,0,0}, {0,0,0}, 60.0f }, "" };

/* Trauma-based camera shake (gap 6.5): the orphaned jce_camera_shake model is
 * now wired into the single live camera resolver.  Gameplay adds trauma via
 * jce_vcam_system_add_trauma (e.g. on a hit / explosion, ultimately surfaced as
 * jce.shake_camera(amount) in Lua); evaluate() advances it by dt and adds the
 * bounded offset to the resolved pose so an active vcam visibly shakes and
 * decays back to zero.  Lazily initialised so the defaults are applied exactly
 * once even before the first reset(). */
static JceCameraShake s_shake;
static int            s_shake_inited = 0;

static void vcam_shake_ensure_init(void)
{
    if (!s_shake_inited) {
        /* decay ~1.5/s (a hard hit settles in well under a second),
         * 18 Hz oscillation, fixed seed for deterministic playback. */
        jce_camera_shake_init(&s_shake, 1.5f, 18.0f, 0xC0FFEEu);
        s_shake_inited = 1;
    }
}


JCE_API void JCE_CALL
jce_vcam_system_reset(void)
{
    s_state.initialised = 0;
    s_state.cur.fov_deg = 60.0f;

    /* Re-seed the shake to its sane defaults (clears any residual trauma so a
     * fresh scene/Play session starts perfectly still). */
    jce_camera_shake_init(&s_shake, 1.5f, 18.0f, 0xC0FFEEu);
    s_shake_inited = 1;

    /* And drop the named-camera override.  It is deliberately allowed to name
     * a camera that has not streamed in yet, which is exactly why it must not
     * survive a scene change: a cut requested in the last level would sit
     * there waiting to hijack the first camera in the next one that happens to
     * share the name. */
    s_state.active_name[0] = 0;
}

JCE_API void JCE_CALL
jce_vcam_system_add_trauma(float amount)
{
    vcam_shake_ensure_init();
    jce_camera_shake_add_trauma(&s_shake, amount);
}

JCE_API bool JCE_CALL
jce_vcam_system_get_shake_offset(float out_pos[3])
{
    vcam_shake_ensure_init();
    if (out_pos) { out_pos[0] = out_pos[1] = out_pos[2] = 0.0f; }
    if (!jce_camera_shake_active(&s_shake)) return false;
    /* READ-ONLY: the clock is advanced once per frame by jce_vcam_system_evaluate
     * (called every Play frame by the game view), so we only read the offset
     * here — never jce_camera_shake_update — to avoid double-advancing. */
    if (out_pos) jce_camera_shake_offset(&s_shake, out_pos, NULL);
    return true;
}

/* Per-iteration scratch — using static ok since the callback is run
 * synchronously inside jce_scene_each_entity in a single thread. */
typedef struct {
    JceScene                  *scene;
    int                        have_winner;
    int                        winner_is_named;   /* the override took it */
    int32_t                    best_priority;
    JceVirtualCameraComponent  best;
} PickCtx;


static bool vcam_selectable(JceScene *s, JceEntity e,
                            const JceVirtualCameraComponent *vc)
{
    return vc && vc->active &&
           jce_scene_component_enabled(s, e, JCE_COMP_FLAG_VIRTUAL_CAMERA);
}

static void pick_cb(JceScene *s, JceEntity e, void *user)
{
    PickCtx *ctx = (PickCtx *)user;
    if (!jce_scene_has_virtual_camera(s, e)) return;
    JceVirtualCameraComponent *vc = jce_scene_get_virtual_camera(s, e);
    if (!vcam_selectable(s, e, vc)) return;

    /* THE NAMED ONE WINS OUTRIGHT, and only over cameras that would have been
     * eligible anyway: an inactive or disabled camera is not resurrected by
     * being named, because "cut to it" cannot mean "and also turn it on" --
     * an author who disabled a camera said something. */
    if (s_state.active_name[0]) {
        int named = (strncmp(vc->vcam_name, s_state.active_name,
                             sizeof(s_state.active_name) - 1) == 0);
        if (ctx->winner_is_named && !named) return;
        if (!named && !ctx->winner_is_named) {
            /* ordinary priority race, below */
        } else if (named) {
            /* First named match wins; a second camera with the same name is
             * an authoring mistake and taking the first is at least stable. */
            if (ctx->winner_is_named) return;
            ctx->have_winner    = 1;
            ctx->winner_is_named = 1;
            ctx->best_priority  = vc->priority;
            ctx->best           = *vc;
            return;
        }
    }

    if (ctx->have_winner && vc->priority <= ctx->best_priority) return;
    ctx->have_winner   = 1;
    ctx->best_priority = vc->priority;
    ctx->best          = *vc;
}

/* ── Naming a shot ─────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    JceEntity   found;
} FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *user)
{
    FindCtx *ctx = (FindCtx *)user;
    if (ctx->found) return;
    if (!jce_scene_has_virtual_camera(s, e)) return;
    JceVirtualCameraComponent *vc = jce_scene_get_virtual_camera(s, e);
    if (!vc) return;
    if (strncmp(vc->vcam_name, ctx->name, 63) == 0) ctx->found = e;
}

JCE_API JceEntity JCE_CALL jce_vcam_find_by_name(JceScene *scene,
                                                 const char *name)
{
    FindCtx ctx;
    if (!scene || !name || !name[0]) return 0;
    ctx.name  = name;
    ctx.found = 0;
    jce_scene_each_entity(scene, find_cb, &ctx);
    return ctx.found;
}

JCE_API bool JCE_CALL jce_vcam_system_set_active_by_name(JceScene   *scene,
                                                         const char *name)
{
    if (!name || !name[0]) {
        s_state.active_name[0] = '\0';
        return false;
    }
    snprintf(s_state.active_name, sizeof(s_state.active_name), "%s", name);

    /* The RECORD is unconditional; the RETURN says whether it resolves right
     * now.  A streaming cell that has not loaded yet must not silently turn
     * the cut into "whatever priority says" the moment it appears. */
    if (!scene) return false;
    {
        JceEntity e = jce_vcam_find_by_name(scene, name);
        if (!e) return false;
        JceVirtualCameraComponent *vc = jce_scene_get_virtual_camera(scene, e);
        return vcam_selectable(scene, e, vc);
    }
}

JCE_API const char *JCE_CALL jce_vcam_system_get_active_name(void)
{
    return s_state.active_name;
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

    /* Always advance the shake clock so trauma decays even while NO vcam is
     * active.  Otherwise trauma added with no live camera (the header documents
     * a no-op in that state) would be retained and fire at full strength the
     * moment a camera becomes active later.  The decay is cheap and trauma 0 is
     * a no-op, so this is safe to run unconditionally. */
    vcam_shake_ensure_init();
    if (dt > 0.0f)
        jce_camera_shake_update(&s_shake, dt);

    PickCtx ctx = { scene, 0, 0, 0, {{0}} };
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

    /* Trauma shake (gap 6.5): ADD the bounded offset onto the resolved pose.
     * The shake clock was already advanced above (so trauma decays even with no
     * active vcam).  jce_camera_shake_offset is exactly zero at trauma 0 and
     * bounded by the configured max amplitude, so a settled camera is untouched
     * and an active one shakes within a known envelope.  We apply the
     * positional offset to out->position (JceVcamOutput carries no rotation
     * channel); the computed euler is currently unused. */
    if (jce_camera_shake_active(&s_shake)) {
        float shake_pos[3];
        jce_camera_shake_offset(&s_shake, shake_pos, NULL);
        out->position[0] += shake_pos[0];
        out->position[1] += shake_pos[1];
        out->position[2] += shake_pos[2];
    }

    if (out_has_active) *out_has_active = true;
}
