/*
 * jce_scene_sequencer.c  SequencePlayer component system (Sequencer P1-L).
 *
 * Drives every entity carrying a JceSequencePlayerComponent: lazily loads
 * the authored .seq.json on first play (opened_hash pattern, mirroring the
 * VideoPlayer driver in jce_scene_video.c), resolves track bindings, and
 * applies evaluated track values to scene components every frame through
 * the canonical property catalogue (jce_seq_prop_*).
 *
 * Track → entity resolution order (per track index i):
 *   1. component bindings[i]   — entity refs persisted IN the scene file,
 *                                remapped on load like IkConstraints refs;
 *   2. bindEntityName          — resolved by name once at open;
 *   3. bindEntity hint         — the raw id authored into the .seq.json
 *                                (only valid within the authoring session).
 *
 * Layer: Middleware/scene.  Consumes jce_sequencer + public scene API.
 */

#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>

#include "os/core/jce_memory.h"

#include <flecs.h>
#include <string.h>

#define LOG_TAG "scene_seq"

/* ── Canonical property catalogue ────────────────────────────────── */

static const char *const k_prop_names[JCE_SEQ_PROP_COUNT] = {
    /* JCE_SEQ_PROP_NONE            */ "",
    /* JCE_SEQ_PROP_POS_X           */ "transform.position.x",
    /* JCE_SEQ_PROP_POS_Y           */ "transform.position.y",
    /* JCE_SEQ_PROP_POS_Z           */ "transform.position.z",
    /* JCE_SEQ_PROP_ROT_EULER_X     */ "transform.rotation.euler.x",
    /* JCE_SEQ_PROP_ROT_EULER_Y     */ "transform.rotation.euler.y",
    /* JCE_SEQ_PROP_ROT_EULER_Z     */ "transform.rotation.euler.z",
    /* JCE_SEQ_PROP_SCALE_X         */ "transform.scale.x",
    /* JCE_SEQ_PROP_SCALE_Y         */ "transform.scale.y",
    /* JCE_SEQ_PROP_SCALE_Z         */ "transform.scale.z",
    /* JCE_SEQ_PROP_SCALE_UNIFORM   */ "transform.scale.uniform",
    /* JCE_SEQ_PROP_LIGHT_INTENSITY */ "light.intensity",
    /* JCE_SEQ_PROP_CAMERA_FOV      */ "camera.fov",
    /* JCE_SEQ_PROP_AUDIO_VOLUME    */ "audio.volume",
    /* JCE_SEQ_PROP_VOLUME_WEIGHT   */ "volume.weight",
    /* JCE_SEQ_PROP_LIGHT_COLOR     */ "light.color",
    /* JCE_SEQ_PROP_MESH_BASE_COLOR */ "meshrenderer.base_color",
};

const char *jce_seq_prop_name(JceSeqPropId id)
{
    if (id <= JCE_SEQ_PROP_NONE || id >= JCE_SEQ_PROP_COUNT) return "";
    return k_prop_names[id];
}

JceSeqPropId jce_seq_prop_from_name(const char *name)
{
    if (!name || !name[0]) return JCE_SEQ_PROP_NONE;
    for (int i = JCE_SEQ_PROP_NONE + 1; i < JCE_SEQ_PROP_COUNT; ++i) {
        if (strcmp(k_prop_names[i], name) == 0) return (JceSeqPropId)i;
    }
    return JCE_SEQ_PROP_NONE;
}

bool jce_seq_prop_is_color(JceSeqPropId id)
{
    return id == JCE_SEQ_PROP_LIGHT_COLOR ||
           id == JCE_SEQ_PROP_MESH_BASE_COLOR;
}

bool jce_seq_prop_supported(JceScene *s, JceEntity e, JceSeqPropId id)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    switch (id) {
    case JCE_SEQ_PROP_POS_X:
    case JCE_SEQ_PROP_POS_Y:
    case JCE_SEQ_PROP_POS_Z:
    case JCE_SEQ_PROP_ROT_EULER_X:
    case JCE_SEQ_PROP_ROT_EULER_Y:
    case JCE_SEQ_PROP_ROT_EULER_Z:
    case JCE_SEQ_PROP_SCALE_X:
    case JCE_SEQ_PROP_SCALE_Y:
    case JCE_SEQ_PROP_SCALE_Z:
    case JCE_SEQ_PROP_SCALE_UNIFORM:
        return jce_scene_has_transform(s, e);
    case JCE_SEQ_PROP_LIGHT_INTENSITY:
    case JCE_SEQ_PROP_LIGHT_COLOR:
        return jce_scene_has_dir_light(s, e)   ||
               jce_scene_has_point_light(s, e) ||
               jce_scene_has_spot_light(s, e);
    case JCE_SEQ_PROP_CAMERA_FOV:
        return jce_scene_has_camera(s, e);
    case JCE_SEQ_PROP_AUDIO_VOLUME:
        return jce_scene_has_audio_source(s, e);
    case JCE_SEQ_PROP_VOLUME_WEIGHT:
        return jce_scene_has_volume(s, e);
    case JCE_SEQ_PROP_MESH_BASE_COLOR:
        return jce_scene_has_mesh_renderer(s, e);
    default:
        return false;
    }
}

/* ── Float read / write ──────────────────────────────────────────── */

/* Rotation props author DEGREES (matches the inspector's euler fields). */
static jce_vec3 seq_euler_deg(const JceTransform *t)
{
    jce_vec3 e = jce_q_to_euler(t->rotation);
    return jce_v3(e.x * JCE_RAD2DEG, e.y * JCE_RAD2DEG, e.z * JCE_RAD2DEG);
}

bool jce_seq_prop_get_float(JceScene *s, JceEntity e, JceSeqPropId id,
                            float *out)
{
    if (!out || !jce_seq_prop_supported(s, e, id)) return false;
    switch (id) {
    case JCE_SEQ_PROP_POS_X: case JCE_SEQ_PROP_POS_Y: case JCE_SEQ_PROP_POS_Z: {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (!t) return false;
        *out = (id == JCE_SEQ_PROP_POS_X) ? t->position.x
             : (id == JCE_SEQ_PROP_POS_Y) ? t->position.y : t->position.z;
        return true;
    }
    case JCE_SEQ_PROP_ROT_EULER_X:
    case JCE_SEQ_PROP_ROT_EULER_Y:
    case JCE_SEQ_PROP_ROT_EULER_Z: {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (!t) return false;
        jce_vec3 deg = seq_euler_deg(t);
        *out = (id == JCE_SEQ_PROP_ROT_EULER_X) ? deg.x
             : (id == JCE_SEQ_PROP_ROT_EULER_Y) ? deg.y : deg.z;
        return true;
    }
    case JCE_SEQ_PROP_SCALE_X: case JCE_SEQ_PROP_SCALE_Y:
    case JCE_SEQ_PROP_SCALE_Z: case JCE_SEQ_PROP_SCALE_UNIFORM: {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (!t) return false;
        *out = (id == JCE_SEQ_PROP_SCALE_Y) ? t->scale.y
             : (id == JCE_SEQ_PROP_SCALE_Z) ? t->scale.z : t->scale.x;
        return true;
    }
    case JCE_SEQ_PROP_LIGHT_INTENSITY: {
        if (jce_scene_has_dir_light(s, e)) {
            JceDirectionalLight *l = jce_scene_get_dir_light(s, e);
            if (l) { *out = l->intensity; return true; }
        } else if (jce_scene_has_point_light(s, e)) {
            JcePointLight *l = jce_scene_get_point_light(s, e);
            if (l) { *out = l->intensity; return true; }
        } else if (jce_scene_has_spot_light(s, e)) {
            JceSpotLight *l = jce_scene_get_spot_light(s, e);
            if (l) { *out = l->intensity; return true; }
        }
        return false;
    }
    case JCE_SEQ_PROP_CAMERA_FOV: {
        JceCameraComponent *c = jce_scene_get_camera(s, e);
        if (!c) return false;
        *out = c->fov_deg;
        return true;
    }
    case JCE_SEQ_PROP_AUDIO_VOLUME: {
        JceAudioSourceComponent *a = jce_scene_get_audio_source(s, e);
        if (!a) return false;
        *out = a->volume;
        return true;
    }
    case JCE_SEQ_PROP_VOLUME_WEIGHT: {
        JceVolumeComponent *v = jce_scene_get_volume(s, e);
        if (!v) return false;
        *out = v->weight;
        return true;
    }
    default:
        return false;
    }
}

void jce_seq_prop_apply_float(JceScene *s, JceEntity e, JceSeqPropId id,
                              float v)
{
    if (!jce_seq_prop_supported(s, e, id)) return;
    switch (id) {
    case JCE_SEQ_PROP_POS_X: case JCE_SEQ_PROP_POS_Y: case JCE_SEQ_PROP_POS_Z: {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (!t) return;
        if      (id == JCE_SEQ_PROP_POS_X) t->position.x = v;
        else if (id == JCE_SEQ_PROP_POS_Y) t->position.y = v;
        else                               t->position.z = v;
        return;
    }
    case JCE_SEQ_PROP_ROT_EULER_X:
    case JCE_SEQ_PROP_ROT_EULER_Y:
    case JCE_SEQ_PROP_ROT_EULER_Z: {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (!t) return;
        jce_vec3 deg = seq_euler_deg(t);
        if      (id == JCE_SEQ_PROP_ROT_EULER_X) deg.x = v;
        else if (id == JCE_SEQ_PROP_ROT_EULER_Y) deg.y = v;
        else                                     deg.z = v;
        t->rotation = jce_euler_to_q(deg.x, deg.y, deg.z);
        return;
    }
    case JCE_SEQ_PROP_SCALE_X: case JCE_SEQ_PROP_SCALE_Y:
    case JCE_SEQ_PROP_SCALE_Z: case JCE_SEQ_PROP_SCALE_UNIFORM: {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (!t) return;
        if      (id == JCE_SEQ_PROP_SCALE_X) t->scale.x = v;
        else if (id == JCE_SEQ_PROP_SCALE_Y) t->scale.y = v;
        else if (id == JCE_SEQ_PROP_SCALE_Z) t->scale.z = v;
        else { t->scale.x = v; t->scale.y = v; t->scale.z = v; }
        return;
    }
    case JCE_SEQ_PROP_LIGHT_INTENSITY: {
        if (jce_scene_has_dir_light(s, e)) {
            JceDirectionalLight *l = jce_scene_get_dir_light(s, e);
            if (l) l->intensity = v;
        } else if (jce_scene_has_point_light(s, e)) {
            JcePointLight *l = jce_scene_get_point_light(s, e);
            if (l) l->intensity = v;
        } else if (jce_scene_has_spot_light(s, e)) {
            JceSpotLight *l = jce_scene_get_spot_light(s, e);
            if (l) l->intensity = v;
        }
        return;
    }
    case JCE_SEQ_PROP_CAMERA_FOV: {
        JceCameraComponent *c = jce_scene_get_camera(s, e);
        if (c) c->fov_deg = v;
        return;
    }
    case JCE_SEQ_PROP_AUDIO_VOLUME: {
        JceAudioSourceComponent *a = jce_scene_get_audio_source(s, e);
        if (a) a->volume = v;
        return;
    }
    case JCE_SEQ_PROP_VOLUME_WEIGHT: {
        JceVolumeComponent *vol = jce_scene_get_volume(s, e);
        if (vol) vol->weight = v;
        return;
    }
    default:
        return;
    }
}

/* ── Color read / write ──────────────────────────────────────────── */

bool jce_seq_prop_get_color(JceScene *s, JceEntity e, JceSeqPropId id,
                            float out_rgb[3])
{
    if (!out_rgb || !jce_seq_prop_supported(s, e, id)) return false;
    if (id == JCE_SEQ_PROP_LIGHT_COLOR) {
        jce_vec3 c;
        if (jce_scene_has_dir_light(s, e)) {
            JceDirectionalLight *l = jce_scene_get_dir_light(s, e);
            if (!l) return false;
            c = l->color;
        } else if (jce_scene_has_point_light(s, e)) {
            JcePointLight *l = jce_scene_get_point_light(s, e);
            if (!l) return false;
            c = l->color;
        } else if (jce_scene_has_spot_light(s, e)) {
            JceSpotLight *l = jce_scene_get_spot_light(s, e);
            if (!l) return false;
            c = l->color;
        } else {
            return false;
        }
        out_rgb[0] = c.x; out_rgb[1] = c.y; out_rgb[2] = c.z;
        return true;
    }
    if (id == JCE_SEQ_PROP_MESH_BASE_COLOR) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
        if (!mr) return false;
        out_rgb[0] = mr->base_color[0];
        out_rgb[1] = mr->base_color[1];
        out_rgb[2] = mr->base_color[2];
        return true;
    }
    return false;
}

void jce_seq_prop_apply_color(JceScene *s, JceEntity e, JceSeqPropId id,
                              const float rgb[3])
{
    if (!rgb || !jce_seq_prop_supported(s, e, id)) return;
    if (id == JCE_SEQ_PROP_LIGHT_COLOR) {
        jce_vec3 c = jce_v3(rgb[0], rgb[1], rgb[2]);
        if (jce_scene_has_dir_light(s, e)) {
            JceDirectionalLight *l = jce_scene_get_dir_light(s, e);
            if (l) l->color = c;
        } else if (jce_scene_has_point_light(s, e)) {
            JcePointLight *l = jce_scene_get_point_light(s, e);
            if (l) l->color = c;
        } else if (jce_scene_has_spot_light(s, e)) {
            JceSpotLight *l = jce_scene_get_spot_light(s, e);
            if (l) l->color = c;
        }
        return;
    }
    if (id == JCE_SEQ_PROP_MESH_BASE_COLOR) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
        if (!mr) return;
        mr->base_color[0] = rgb[0];
        mr->base_color[1] = rgb[1];
        mr->base_color[2] = rgb[2];   /* alpha untouched */
        return;
    }
}

/* ── SequencePlayer runtime blob ─────────────────────────────────────
 *
 * c->seq points at this wrapper (not the bare JceSequencer) so the
 * once-at-open name-resolution results live exactly as long as the open
 * sequencer.  Freed by the flecs hooks / explicit release below. */

typedef struct {
    JceSequencer *seq;
    int           track_count;
    JceEntity    *resolved;   /* per-track bindEntityName result (0 = none) */
} JceSeqPlayerRuntime;

static void sq_clear_runtime(JceSequencePlayerComponent *c)
{
    c->seq         = NULL;
    c->prev_time   = 0.0f;
    c->started     = false;
    c->opened_hash = 0;
}

static void sq_release_one(JceSequencePlayerComponent *c)
{
    JceSeqPlayerRuntime *rt = (JceSeqPlayerRuntime *)c->seq;
    if (rt) {
        if (rt->seq)      jce_sequencer_free(rt->seq);
        if (rt->resolved) JCE_FREE(rt->resolved);
        JCE_FREE(rt);
    }
    sq_clear_runtime(c);
}

/* ── flecs lifecycle hooks (mirror jce_scene_video.c) ───────────────
 *
 *   ctor : zero-init.
 *   dtor : free the open sequencer + resolution table.
 *   move : transfer bytes, invalidate the source (so the derived
 *          move-dtor is a no-op when flecs moves the entity between
 *          tables on add/remove of OTHER components).
 *   copy : duplicate authoring fields ONLY; each copy opens its own
 *          sequencer so two entities never share one handle. */

static void sq_hook_ctor(void *ptr, int32_t count, const ecs_type_info_t *ti)
{
    (void)ti;
    JceSequencePlayerComponent *arr = (JceSequencePlayerComponent *)ptr;
    for (int32_t i = 0; i < count; ++i) {
        memset(&arr[i], 0, sizeof(arr[i]));
        sq_clear_runtime(&arr[i]);
    }
}

static void sq_hook_dtor(void *ptr, int32_t count, const ecs_type_info_t *ti)
{
    (void)ti;
    JceSequencePlayerComponent *arr = (JceSequencePlayerComponent *)ptr;
    for (int32_t i = 0; i < count; ++i)
        sq_release_one(&arr[i]);
}

static void sq_hook_move(void *dst_ptr, void *src_ptr, int32_t count,
                         const ecs_type_info_t *ti)
{
    (void)ti;
    JceSequencePlayerComponent *dst = (JceSequencePlayerComponent *)dst_ptr;
    JceSequencePlayerComponent *src = (JceSequencePlayerComponent *)src_ptr;
    for (int32_t i = 0; i < count; ++i) {
        sq_release_one(&dst[i]);
        dst[i] = src[i];
        sq_clear_runtime(&src[i]);
    }
}

static void sq_hook_copy(void *dst_ptr, const void *src_ptr, int32_t count,
                         const ecs_type_info_t *ti)
{
    (void)ti;
    JceSequencePlayerComponent       *dst = (JceSequencePlayerComponent *)dst_ptr;
    const JceSequencePlayerComponent *src = (const JceSequencePlayerComponent *)src_ptr;
    for (int32_t i = 0; i < count; ++i) {
        sq_release_one(&dst[i]);
        memcpy(dst[i].seq_path, src[i].seq_path, sizeof(dst[i].seq_path));
        dst[i].play_on_awake = src[i].play_on_awake;
        dst[i].loop_override = src[i].loop_override;
        dst[i].override_loop = src[i].override_loop;
        dst[i].speed         = src[i].speed;
        dst[i].binding_count = src[i].binding_count;
        memcpy(dst[i].bindings, src[i].bindings, sizeof(dst[i].bindings));
        sq_clear_runtime(&dst[i]);
    }
}

void jce_scene_sequencer_install_hooks(ecs_world_t *world, ecs_entity_t comp_id)
{
    if (!world || !comp_id) return;
    ecs_type_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.ctor = sq_hook_ctor;
    hooks.dtor = sq_hook_dtor;
    hooks.move = sq_hook_move;
    hooks.copy = sq_hook_copy;
    ecs_set_hooks_id(world, comp_id, &hooks);
}

/* ── Name resolution (once at open) ──────────────────────────────── */

typedef struct {
    const char *want;
    JceEntity   found;
} SqNameCtx;

static void sq_find_by_name_cb(JceScene *s, JceEntity e, void *ud)
{
    SqNameCtx *ctx = (SqNameCtx *)ud;
    if (ctx->found != JCE_ENTITY_INVALID) return;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0) { ctx->found = e; return; }
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (m && strcmp(m->name, ctx->want) == 0) ctx->found = e;
}

static JceEntity sq_find_entity_by_name(JceScene *s, const char *name)
{
    if (!name || !name[0]) return JCE_ENTITY_INVALID;
    SqNameCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, sq_find_by_name_cb, &ctx);
    return ctx.found;
}

/* ── Lazy open ───────────────────────────────────────────────────── */

static void sq_ensure_open(JceScene *s, JceSequencePlayerComponent *c)
{
    if (c->seq) return;
    if (c->seq_path[0] == '\0') return;
    /* Already attempted this exact path (load failed): don't re-hit the
     * filesystem every frame.  A later path edit clears `started` via the
     * hash reconciliation in sq_each below, re-enabling one attempt. */
    if (c->started) return;

    /* FNV-1a over the authored asset path (jce_hash.h; mirrors the
     * VideoPlayer driver). */
    const uint64_t want = jce_fnv1a64_str(c->seq_path);
    c->started     = true;
    c->opened_hash = want;

    JceSequencer *seq = jce_sequencer_load_file(c->seq_path);
    if (!seq) {
        LOG_WARN(LOG_TAG, "sequence not loadable: %s", c->seq_path);
        return;
    }

    JceSeqPlayerRuntime *rt = JCE_NEW(JceSeqPlayerRuntime);
    if (!rt) { jce_sequencer_free(seq); return; }
    rt->seq         = seq;
    rt->track_count = jce_sequencer_track_count(seq);
    rt->resolved    = NULL;

    if (rt->track_count > 0) {
        rt->resolved = JCE_NEW_ARRAY(JceEntity, rt->track_count);
        if (rt->resolved) {
            for (int i = 0; i < rt->track_count; ++i) {
                const char *nm = jce_sequencer_track_bind_entity_name(seq, i);
                rt->resolved[i] = sq_find_entity_by_name(s, nm);
            }
        }
    }

    if (c->override_loop)
        jce_sequencer_set_looping(seq, c->loop_override);
    jce_sequencer_set_time(seq, 0.0f);
    jce_sequencer_set_playing(seq, true);

    c->seq       = rt;
    c->prev_time = 0.0f;
    LOG_INFO(LOG_TAG, "sequence opened: %s tracks=%d", c->seq_path,
             rt->track_count);
}

/* ── Per-entity tick ─────────────────────────────────────────────── */

static JceEntity sq_track_target(const JceSequencePlayerComponent *c,
                                 const JceSeqPlayerRuntime *rt, int i)
{
    /* 1. scene-file binding (remapped on load) */
    if (i < c->binding_count && i < JCE_SEQ_PLAYER_MAX_BINDINGS &&
        c->bindings[i] != 0)
        return (JceEntity)c->bindings[i];
    /* 2. name resolution from open time */
    if (rt->resolved && i < rt->track_count &&
        rt->resolved[i] != JCE_ENTITY_INVALID)
        return rt->resolved[i];
    /* 3. raw authored id hint */
    return (JceEntity)jce_sequencer_track_bind_entity_hint(rt->seq, i);
}

static void sq_each(JceScene *s, JceEntity e, void *ud)
{
    float dt = *(const float *)ud;
    JceSequencePlayerComponent *c = jce_scene_get_sequence_player(s, e);
    if (!c) return;

    /* Reconcile an in-place seq_path change (inspector edit / Reset
     * Component / undo-redo): drop the stale sequencer so the driver
     * re-opens the new asset. */
    {
        const uint64_t want = c->seq_path[0] ? jce_fnv1a64_str(c->seq_path) : 0;
        if (c->started && want != c->opened_hash)
            sq_release_one(c);
    }

    if (!c->play_on_awake && !c->seq) return;

    sq_ensure_open(s, c);
    JceSeqPlayerRuntime *rt = (JceSeqPlayerRuntime *)c->seq;
    if (!rt || !rt->seq) return;

    JceSequencer *seq = rt->seq;
    const float speed = (c->speed > 0.0f) ? c->speed : 1.0f;
    const float prev  = c->prev_time;
    jce_sequencer_update(seq, dt * speed);
    const float now      = jce_sequencer_get_time(seq);
    const float duration = jce_sequencer_duration(seq);

    const int n = rt->track_count;
    for (int i = 0; i < n; ++i) {
        const JceSeqTrackType type = jce_sequencer_track_type(seq, i);

        if (type == JCE_SEQ_TRACK_EVENT) {
            /* Loop wrap splits the scan: (prev, duration] + (0, now]. */
            int fired;
            if (now < prev)
                fired = jce_sequencer_track_events_in_range(seq, i, prev, duration)
                      + jce_sequencer_track_events_in_range(seq, i, 0.0f, now);
            else
                fired = jce_sequencer_track_events_in_range(seq, i, prev, now);
            if (fired > 0)
                LOG_DEBUG(LOG_TAG, "sequence '%s' track %d fired %d event(s)",
                          c->seq_path, i, fired);
            continue;
        }

        JceEntity target = sq_track_target(c, rt, i);
        if (target == JCE_ENTITY_INVALID ||
            !jce_scene_has_transform(s, target))
            continue;   /* unbound or dead */

        const JceSeqPropId prop =
            jce_seq_prop_from_name(jce_sequencer_track_bind_prop_name(seq, i));
        if (prop == JCE_SEQ_PROP_NONE) continue;

        if (type == JCE_SEQ_TRACK_COLOR) {
            if (!jce_seq_prop_is_color(prop)) continue;
            float rgb[3];
            jce_sequencer_track_eval_color(seq, i, now, rgb);
            jce_seq_prop_apply_color(s, target, prop, rgb);
        } else { /* JCE_SEQ_TRACK_PROPERTY */
            if (jce_seq_prop_is_color(prop)) continue;
            const float v = jce_sequencer_track_eval_float(seq, i, now);
            jce_seq_prop_apply_float(s, target, prop, v);
        }
    }

    c->prev_time = now;
}

void jce_scene_sequencer_update(JceScene *s, float dt)
{
    if (!s) return;
    jce_scene_each_entity(s, sq_each, &dt);
}
