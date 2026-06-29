/*
 * jce_scene_render_camera.cpp  Maya-style orbit camera for the editor scene.
 */

#include "jce_scene_render_internal.h"

#include <cstdlib>   /* getenv/atof — headless vista camera env overrides */

/* ── Orbit constants ──────────────────────────────────────────────── */

#define ORBIT_PITCH_MAX  (89.0f * JCE_DEG2RAD)
#define ORBIT_DIST_MIN   0.001f
#define ORBIT_DIST_MAX   100000.0f
#define ORBIT_CLIP_HYSTERESIS     0.02f
#define ORBIT_CLIP_MIN_NEAR_DELTA 0.00025f
#define ORBIT_CLIP_MIN_FAR_DELTA  1.0f
#define ORBIT_FOCUS_DURATION      0.30f
#define ORBIT_FOCUS_BOUNDS_EPS    0.001f

/* ── Internal: recompute camera position from orbit state ─────────── */

void orbit_apply(void)
{
    if (!s_sr.camera) return;

    float y = s_sr.orbit_pitch;
    float x = s_sr.orbit_yaw;
    float d = s_sr.orbit_distance;

    jce_vec3 pos;
    pos.x = s_sr.orbit_target.x + d * sinf(x) * cosf(y);
    pos.y = s_sr.orbit_target.y + d * sinf(y);
    pos.z = s_sr.orbit_target.z - d * cosf(x) * cosf(y);

    jce_camera_set_position(s_sr.camera, pos);
    jce_camera_look_at(s_sr.camera, s_sr.orbit_target);

    /* Dynamic near/far clip planes based on orbit distance.
     * Prevents Z-fighting when very close, and extends far plane when zoom
     * is very far out. */
    float near_clip_target = d * 0.001f;
    if (near_clip_target < 0.001f) near_clip_target = 0.001f;
    if (near_clip_target > 1.0f)   near_clip_target = 1.0f;
    float far_clip_target = d * 100.0f;
    if (far_clip_target < 100.0f)    far_clip_target = 100.0f;
    if (far_clip_target > 100000.0f) far_clip_target = 100000.0f;

    if (!s_sr.orbit_clip_valid) {
        s_sr.orbit_near_cached = near_clip_target;
        s_sr.orbit_far_cached = far_clip_target;
        s_sr.orbit_clip_valid = true;
    } else {
        float near_delta = fabsf(near_clip_target - s_sr.orbit_near_cached);
        float near_rel = near_delta / fmaxf(s_sr.orbit_near_cached, 0.001f);
        if (near_delta > ORBIT_CLIP_MIN_NEAR_DELTA
            && (near_rel > ORBIT_CLIP_HYSTERESIS || near_delta > 0.001f))
        {
            s_sr.orbit_near_cached = near_clip_target;
        }

        float far_delta = fabsf(far_clip_target - s_sr.orbit_far_cached);
        float far_rel = far_delta / fmaxf(s_sr.orbit_far_cached, 100.0f);
        if (far_delta > ORBIT_CLIP_MIN_FAR_DELTA
            && far_rel > ORBIT_CLIP_HYSTERESIS)
        {
            s_sr.orbit_far_cached = far_clip_target;
        }
    }

    jce_camera_set_near_far(s_sr.camera,
                            s_sr.orbit_near_cached,
                            s_sr.orbit_far_cached);
}

static void orbit_cancel_focus_anim(void)
{
    jce_editor_scene_focus_anim_cancel(&s_sr.focus_anim);
}

void jce_editor_scene_camera_update(float dt_sec)
{
    if (!s_sr.initialized || !s_sr.camera || !s_sr.focus_anim.active) return;

    JceEditorSceneFocusSample sample =
        jce_editor_scene_focus_anim_step(&s_sr.focus_anim, dt_sec);
    s_sr.orbit_target = sample.center;
    s_sr.orbit_distance = sample.distance;
    if (s_sr.orbit_distance < ORBIT_DIST_MIN)
        s_sr.orbit_distance = ORBIT_DIST_MIN;
    if (s_sr.orbit_distance > ORBIT_DIST_MAX)
        s_sr.orbit_distance = ORBIT_DIST_MAX;
    orbit_apply();
}

/* ── Public camera API ────────────────────────────────────────────── */

JceCamera *jce_editor_scene_get_camera(void)
{
    return s_sr.camera;
}

bool jce_editor_scene_get_camera_matrices(float *out_view16,
                                           float *out_proj16,
                                           float *out_eye3,
                                           float viewport_w,
                                           float viewport_h)
{
    if (!s_sr.initialized || !s_sr.camera) return false;
    if (!out_view16 || !out_proj16 || !out_eye3) return false;

    uint32_t req_w = (uint32_t)fmaxf(1.0f, floorf(viewport_w));
    uint32_t req_h = (uint32_t)fmaxf(1.0f, floorf(viewport_h));

    if (s_sr.camera_cache_valid
        && req_w == s_sr.viewport_width
        && req_h == s_sr.viewport_height)
    {
        memcpy(out_view16, s_sr.cached_view, sizeof(s_sr.cached_view));
        memcpy(out_proj16, s_sr.cached_proj, sizeof(s_sr.cached_proj));
        out_eye3[0] = s_sr.cached_eye[0];
        out_eye3[1] = s_sr.cached_eye[1];
        out_eye3[2] = s_sr.cached_eye[2];
        return true;
    }

    float aspect = (req_h > 0) ? ((float)req_w / (float)req_h) : 1.0f;
    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, s_sr.homogeneous_depth);
    memcpy(out_view16, JCE_M4_PTR(view), 16 * sizeof(float));
    memcpy(out_proj16, JCE_M4_PTR(proj), 16 * sizeof(float));

    jce_vec3 pos = jce_camera_get_position(s_sr.camera);
    out_eye3[0] = pos.x;
    out_eye3[1] = pos.y;
    out_eye3[2] = pos.z;

    memcpy(s_sr.cached_view, out_view16, sizeof(s_sr.cached_view));
    memcpy(s_sr.cached_proj, out_proj16, sizeof(s_sr.cached_proj));
    s_sr.cached_eye[0] = out_eye3[0];
    s_sr.cached_eye[1] = out_eye3[1];
    s_sr.cached_eye[2] = out_eye3[2];
    s_sr.camera_cache_valid = true;
    return true;
}

void jce_editor_scene_camera_orbit(float dyaw, float dpitch)
{
    if (!s_sr.initialized) return;
    orbit_cancel_focus_anim();
    s_sr.orbit_yaw   += dyaw;
    s_sr.orbit_pitch += dpitch;
    if (s_sr.orbit_pitch >  ORBIT_PITCH_MAX) s_sr.orbit_pitch =  ORBIT_PITCH_MAX;
    if (s_sr.orbit_pitch < -ORBIT_PITCH_MAX) s_sr.orbit_pitch = -ORBIT_PITCH_MAX;
    orbit_apply();
}

void jce_editor_scene_camera_pan(float dx, float dy)
{
    if (!s_sr.initialized || !s_sr.camera) return;
    orbit_cancel_focus_anim();
    jce_vec3 right = jce_camera_get_right(s_sr.camera);
    jce_vec3 up    = jce_camera_get_up(s_sr.camera);

    float scale = s_sr.orbit_distance * 0.002f;
    jce_vec3 offset = jce_v3_add(
        jce_v3_scale(right, -dx * scale),
        jce_v3_scale(up,     dy * scale));

    s_sr.orbit_target = jce_v3_add(s_sr.orbit_target, offset);
    orbit_apply();
}

/* ── Maya-style adaptive zoom: nearest object along view ray ──────── */

static float nearest_hit_along_view_ray(void)
{
    if (!s_sr.camera) return 1e30f;

    jce_vec3 eye = jce_camera_get_position(s_sr.camera);

    /* Ray direction: from camera eye toward the orbit target. */
    jce_vec3 dir = jce_v3_sub(s_sr.orbit_target, eye);
    float dir_len = jce_v3_len(dir);
    if (dir_len < 1e-8f) return 1e30f;
    dir = jce_v3_scale(dir, 1.0f / dir_len);

    float best_t = 1e30f;
    int total = jce_state_get_entity_count();
    JceScene *scene = jce_state_get_scene();
    if (!scene) return best_t;

    for (int i = 0; i < total; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (!t) continue;

        float hx = fabsf(t->scale.x) * 0.5f;
        float hy = fabsf(t->scale.y) * 0.5f;
        float hz = fabsf(t->scale.z) * 0.5f;
        if (hx < 0.1f) hx = 0.1f;
        if (hy < 0.1f) hy = 0.1f;
        if (hz < 0.1f) hz = 0.1f;

        jce_vec3 bmin = jce_v3(t->position.x - hx, t->position.y - hy, t->position.z - hz);
        jce_vec3 bmax = jce_v3(t->position.x + hx, t->position.y + hy, t->position.z + hz);

        float th;
        if (jce_ray_aabb_intersect(eye, dir, bmin, bmax, &th) && th >= 0.0f) {
            if (th < best_t) best_t = th;
        }
    }
    return best_t;
}

void jce_editor_scene_camera_zoom(float delta)
{
    if (!s_sr.initialized) return;
    orbit_cancel_focus_anim();

    float base_step = delta * s_sr.orbit_distance * 0.1f;

    /* When zooming IN (distance decreasing, base_step > 0), apply Maya-style
     * deceleration: reduce zoom speed proportionally to the nearest object
     * distance along the view ray.  This prevents camera tunneling through
     * objects without ever fully stopping the zoom. */
    if (base_step > 0.0f) {
        float t_hit = nearest_hit_along_view_ray();
        if (t_hit < s_sr.orbit_distance) {
            float ratio = t_hit / s_sr.orbit_distance;
            if (ratio < 0.01f) ratio = 0.01f;
            base_step *= ratio;
        }
    }

    s_sr.orbit_distance -= base_step;
    if (s_sr.orbit_distance < ORBIT_DIST_MIN) s_sr.orbit_distance = ORBIT_DIST_MIN;
    if (s_sr.orbit_distance > ORBIT_DIST_MAX) s_sr.orbit_distance = ORBIT_DIST_MAX;
    orbit_apply();
}

void jce_editor_scene_camera_get_target(float *out3)
{
    if (out3) {
        out3[0] = s_sr.orbit_target.x;
        out3[1] = s_sr.orbit_target.y;
        out3[2] = s_sr.orbit_target.z;
    }
}

void jce_editor_scene_camera_set_target(float x, float y, float z)
{
    orbit_cancel_focus_anim();
    s_sr.orbit_target = jce_v3(x, y, z);
    if (s_sr.initialized) orbit_apply();
}

/* ── Full orbit-state capture / restore (camera bookmarks) ──────────── */

void jce_editor_scene_camera_get_state(float out_target3[3], float *out_yaw,
                                       float *out_pitch, float *out_distance)
{
    if (out_target3) {
        out_target3[0] = s_sr.orbit_target.x;
        out_target3[1] = s_sr.orbit_target.y;
        out_target3[2] = s_sr.orbit_target.z;
    }
    if (out_yaw)      *out_yaw      = s_sr.orbit_yaw;
    if (out_pitch)    *out_pitch    = s_sr.orbit_pitch;
    if (out_distance) *out_distance = s_sr.orbit_distance;
}

void jce_editor_scene_camera_set_state(const float target3[3], float yaw,
                                       float pitch, float distance)
{
    if (!s_sr.initialized) return;
    orbit_cancel_focus_anim();
    if (target3) s_sr.orbit_target = jce_v3(target3[0], target3[1], target3[2]);
    s_sr.orbit_yaw = yaw;
    if (pitch >  ORBIT_PITCH_MAX) pitch =  ORBIT_PITCH_MAX;
    if (pitch < -ORBIT_PITCH_MAX) pitch = -ORBIT_PITCH_MAX;
    s_sr.orbit_pitch = pitch;
    if (distance < ORBIT_DIST_MIN) distance = ORBIT_DIST_MIN;
    if (distance > ORBIT_DIST_MAX) distance = ORBIT_DIST_MAX;
    s_sr.orbit_distance = distance;
    orbit_apply();
}

void jce_editor_scene_camera_snap_view(JceCamPresetView preset)
{
    if (!s_sr.initialized) return;
    orbit_cancel_focus_anim();

    switch (preset) {
    case JCE_CAM_VIEW_FRONT:   s_sr.orbit_yaw = 0;              s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_BACK:    s_sr.orbit_yaw = JCE_PI;         s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_LEFT:    s_sr.orbit_yaw = -JCE_PI * 0.5f; s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_RIGHT:   s_sr.orbit_yaw =  JCE_PI * 0.5f; s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_TOP:     s_sr.orbit_yaw = 0;              s_sr.orbit_pitch =  ORBIT_PITCH_MAX; break;
    case JCE_CAM_VIEW_BOTTOM:  s_sr.orbit_yaw = 0;              s_sr.orbit_pitch = -ORBIT_PITCH_MAX; break;
    }
    orbit_apply();
}

void jce_editor_scene_camera_reset(void)
{
    if (!s_sr.initialized) return;
    orbit_cancel_focus_anim();
    s_sr.focus_last_bounds_valid = false;
    s_sr.focus_zoom_step = 0;
    s_sr.orbit_target   = jce_v3(0.0f, 0.0f, 0.0f);
    s_sr.orbit_distance = sqrtf(8.0f*8.0f + 6.0f*6.0f + 8.0f*8.0f);
    s_sr.orbit_yaw      = atan2f(8.0f, -8.0f);
    s_sr.orbit_pitch    = asinf(6.0f / s_sr.orbit_distance);
    orbit_apply();
}

void jce_editor_scene_camera_focus_aabb(const float min3[3], const float max3[3])
{
    if (!s_sr.initialized || !s_sr.camera || !min3 || !max3) return;

    float fov_deg = jce_camera_get_fov(s_sr.camera);
    bool same_bounds = s_sr.focus_last_bounds_valid
        && jce_editor_scene_focus_same_bounds(s_sr.focus_last_min,
                                              s_sr.focus_last_max,
                                              min3,
                                              max3,
                                              ORBIT_FOCUS_BOUNDS_EPS);
    int zoom_step =
        jce_editor_scene_focus_next_zoom_step(same_bounds,
                                              s_sr.focus_zoom_step);
    JceEditorSceneFocusTarget focus =
        jce_editor_scene_focus_make_target(min3, max3, fov_deg, zoom_step);

    jce_editor_scene_focus_anim_start(&s_sr.focus_anim,
                                      s_sr.orbit_target,
                                      s_sr.orbit_distance,
                                      focus.center,
                                      focus.distance,
                                      ORBIT_FOCUS_DURATION);

    memcpy(s_sr.focus_last_min, min3, sizeof(s_sr.focus_last_min));
    memcpy(s_sr.focus_last_max, max3, sizeof(s_sr.focus_last_max));
    s_sr.focus_last_bounds_valid = true;
    s_sr.focus_zoom_step = focus.zoom_step;
}

/* ── Bird's-eye overview: frame the WHOLE streamed world from above ─── */

void jce_editor_scene_frame_overview(void)
{
    if (!s_sr.initialized || !s_sr.camera) return;

    /* World AABB: union of all authored streaming chunks (center ± radius).
     * This covers the FULL world, not just the chunks currently loaded.
     * Fall back to the known ±2757 m world span when no streaming settings
     * exist (non-streaming scenes), with a sensible vertical range. */
    float wmin[3];
    float wmax[3];
    bool  have_bounds = false;

    JceScene *scene = jce_state_get_scene();
    if (scene) {
        const JceSceneStreamingSettings *ss =
            jce_scene_get_streaming_settings(scene);
        if (ss && ss->chunk_count > 0) {
            wmin[0] = wmin[1] = wmin[2] =  1e30f;
            wmax[0] = wmax[1] = wmax[2] = -1e30f;
            for (uint32_t i = 0; i < ss->chunk_count; i++) {
                const JceSceneStreamChunk *c = &ss->chunks[i];
                float r = c->radius > 0.0f ? c->radius : 1.0f;
                for (int k = 0; k < 3; k++) {
                    float lo = c->center[k] - r;
                    float hi = c->center[k] + r;
                    if (lo < wmin[k]) wmin[k] = lo;
                    if (hi > wmax[k]) wmax[k] = hi;
                }
            }
            /* Chunk centres often share one ground plane (flat radius in Y),
             * so widen the vertical span to include tall buildings + sky. */
            if (wmax[1] - wmin[1] < 100.0f) {
                wmin[1] -= 50.0f;
                wmax[1] += 400.0f;
            }
            have_bounds = true;
        }
    }

    if (!have_bounds) {
        wmin[0] = -2757.0f; wmin[1] = -50.0f;  wmin[2] = -2757.0f;
        wmax[0] =  2757.0f; wmax[1] =  400.0f; wmax[2] =  2757.0f;
    }

    /* Steep bird's-eye angle, anchored regardless of the current orbit.
     * Animating target/distance (below) while the pitch is pre-set gives a
     * smooth swing up to the overview. */
    /* +pitch = bird's-eye looking DOWN (pos.y = target.y + d*sin(pitch); matches
     * CAM_VIEW_TOP = +PITCH_MAX).  -65 put the camera BELOW the world looking UP
     * (a worm's-eye view) — that was the "overview direction reversed" bug. */
    s_sr.orbit_pitch = 65.0f * JCE_DEG2RAD;
    s_sr.orbit_yaw   = 0.6f;

    float fov_deg = jce_camera_get_fov(s_sr.camera);
    JceEditorSceneFocusTarget focus =
        jce_editor_scene_focus_make_target(wmin, wmax, fov_deg, 0);

    jce_editor_scene_focus_anim_start(&s_sr.focus_anim,
                                      s_sr.orbit_target,
                                      s_sr.orbit_distance,
                                      focus.center,
                                      focus.distance,
                                      0.60f);

    /* Distinct enough from a normal focus that the next F won't think the
     * bounds are unchanged (avoids the zoom-step toggle). */
    s_sr.focus_last_bounds_valid = false;
    s_sr.focus_zoom_step = 0;
}

void jce_editor_scene_frame_vista(void)
{
    if (!s_sr.initialized || !s_sr.camera) return;
    /* Fixed eye-level-ish vista for headless visual QA (JCE_DBG_VISTA): a few
     * metres above the meadow centre, looking across the grass toward the
     * backdrop + sky.  Set directly (no focus anim) so a one-shot capture is
     * deterministic and comparable across tuning passes. */
    s_sr.orbit_target   = jce_v3(0.0f, 10.0f, 0.0f);
    s_sr.orbit_pitch    = 3.0f * JCE_DEG2RAD;   /* near-horizontal: ground+horizon+sky */
    s_sr.orbit_yaw      = 0.4f;
    s_sr.orbit_distance = 50.0f;
    /* Optional env overrides so a headless capture can aim at a specific test
       subject (e.g. a top-down look at a point light to see its omni shadows).
       JCE_DBG_VISTA_TX/TY/TZ = target, _DIST = distance, _PITCH/_YAW = degrees. */
    { const char *e;
      if ((e = getenv("JCE_DBG_VISTA_TX"))) s_sr.orbit_target.x = (float)atof(e);
      if ((e = getenv("JCE_DBG_VISTA_TY"))) s_sr.orbit_target.y = (float)atof(e);
      if ((e = getenv("JCE_DBG_VISTA_TZ"))) s_sr.orbit_target.z = (float)atof(e);
      if ((e = getenv("JCE_DBG_VISTA_DIST")))  s_sr.orbit_distance = (float)atof(e);
      if ((e = getenv("JCE_DBG_VISTA_PITCH"))) s_sr.orbit_pitch = (float)atof(e) * JCE_DEG2RAD;
      if ((e = getenv("JCE_DBG_VISTA_YAW")))   s_sr.orbit_yaw   = (float)atof(e) * JCE_DEG2RAD; }
    s_sr.focus_last_bounds_valid = false;
    s_sr.focus_zoom_step = 0;
    orbit_apply();
}

void jce_editor_scene_camera_focus_transform(const JceTransform *transform)
{
    if (!transform) return;

    float bmin[3] = {
        -0.5f,
        -0.5f,
        -0.5f,
    };
    float bmax[3] = {
         0.5f,
         0.5f,
         0.5f,
    };

    jce_vec3 scale = transform->scale;
    if (fabsf(scale.x) < 0.2f) scale.x = 0.2f;
    if (fabsf(scale.y) < 0.2f) scale.y = 0.2f;
    if (fabsf(scale.z) < 0.2f) scale.z = 0.2f;

    float world_min[3];
    float world_max[3];
    jce_editor_scene_focus_transform_aabb(bmin, bmax,
                                          transform->position,
                                          transform->rotation,
                                          scale,
                                          world_min,
                                          world_max);
    jce_editor_scene_camera_focus_aabb(world_min, world_max);
}

bool jce_editor_scene_camera_get_entity_focus_bounds(uint32_t entity_id,
                                                     float out_min3[3],
                                                     float out_max3[3])
{
    if (!out_min3 || !out_max3) return false;

    JceScene *scene = jce_state_get_scene();
    if (!scene || entity_id == 0 || !jce_state_entity_exists(entity_id))
        return false;

    JceEntity e = (JceEntity)entity_id;
    JceTransform *t = jce_scene_get_transform(scene, e);
    if (!t) return false;

    if (jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr && mr->mesh_path[0] != '\0') {
            float wp[3] = { t->position.x, t->position.y, t->position.z };
            JceMesh *mesh = get_cached_mesh(mr->mesh_path, wp);
            if (mesh) {
                float local_min[3];
                float local_max[3];
                jce_mesh_get_aabb(mesh, local_min, local_max);
                jce_editor_scene_focus_transform_aabb(local_min,
                                                      local_max,
                                                      t->position,
                                                      t->rotation,
                                                      t->scale,
                                                      out_min3,
                                                      out_max3);
                return true;
            }
        }
    }

    float local_min[3] = { -0.5f, -0.5f, -0.5f };
    float local_max[3] = {  0.5f,  0.5f,  0.5f };
    jce_vec3 scale = t->scale;
    if (fabsf(scale.x) < 0.2f) scale.x = 0.2f;
    if (fabsf(scale.y) < 0.2f) scale.y = 0.2f;
    if (fabsf(scale.z) < 0.2f) scale.z = 0.2f;

    jce_editor_scene_focus_transform_aabb(local_min,
                                          local_max,
                                          t->position,
                                          t->rotation,
                                          scale,
                                          out_min3,
                                          out_max3);
    return true;
}

void jce_editor_scene_camera_focus_entity(uint32_t entity_id)
{
    float bmin[3];
    float bmax[3];
    if (jce_editor_scene_camera_get_entity_focus_bounds(entity_id, bmin, bmax))
        jce_editor_scene_camera_focus_aabb(bmin, bmax);
}
