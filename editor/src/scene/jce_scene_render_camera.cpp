/*
 * jce_scene_render_camera.cpp  Maya-style orbit camera for the editor scene.
 */

#include "jce_scene_render_internal.h"
#include "core/jce_editor_project_state.h"

#include <cstdlib>   /* getenv/atof — headless vista camera env overrides */
#include <cstdio>    /* snprintf for the spin-capture path */
#include <jce/ui/jce_imgui_renderer.h>  /* deterministic spin capture */

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

/* Persist the orbit pose under the current scene's per-scene state so
 * re-opening the scene lands where the user left off.  Gated on an
 * actual value change: the store's writes are debounced/cheap, but
 * skipping no-ops keeps static frames from dirtying the state file. */
static void orbit_persist_pose(void)
{
    const char *scene = jce_state_get_current_scene_path();
    if (!scene || !scene[0]) return;

    static float s_last[6] = { 0 };
    static bool  s_last_valid = false;
    const float cur[6] = {
        s_sr.orbit_target.x, s_sr.orbit_target.y, s_sr.orbit_target.z,
        s_sr.orbit_yaw, s_sr.orbit_pitch, s_sr.orbit_distance,
    };
    if (s_last_valid && memcmp(cur, s_last, sizeof(cur)) == 0) return;
    memcpy(s_last, cur, sizeof(s_last));
    s_last_valid = true;

    jce_editor_pstate_scene_set_float(scene, "cam.tx",    cur[0]);
    jce_editor_pstate_scene_set_float(scene, "cam.ty",    cur[1]);
    jce_editor_pstate_scene_set_float(scene, "cam.tz",    cur[2]);
    jce_editor_pstate_scene_set_float(scene, "cam.yaw",   cur[3]);
    jce_editor_pstate_scene_set_float(scene, "cam.pitch", cur[4]);
    jce_editor_pstate_scene_set_float(scene, "cam.dist",  cur[5]);
}

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
    /* JCE_DBG_VISTA_FAR: headless override of the far clip — lets a stress test
     * narrow the frustum so most of a huge instance field falls OFF-screen (the
     * default vista far = d*100 sees everything → no GPU cull benefit to measure). */
    { static float s_far = -2.0f;
      if (s_far < -1.0f) { const char *e = getenv("JCE_DBG_VISTA_FAR"); s_far = e ? (float)atof(e) : -1.0f; }
      if (s_far > 0.0f) far_clip_target = s_far; }

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

    /* Every pose change funnels through here, so this is the one save
     * seam needed for per-scene camera persistence. */
    orbit_persist_pose();
}

static void orbit_cancel_focus_anim(void)
{
    jce_editor_scene_focus_anim_cancel(&s_sr.focus_anim);
}

void jce_editor_scene_camera_update(float dt_sec)
{
    /* JCE_DBG_VISTA_SPIN=<deg/frame>: slowly orbit the fixed headless vista so an
     * A/B can stress the Hi-Z occlusion cull's 1-frame latency (it reads LAST
     * frame's depth) under camera MOTION — a static vista can never reveal the
     * occlusion-edge popping that latency risks.  Frame-count-deterministic (a
     * fixed per-frame delta from the one-shot vista yaw), so frame N lands on the
     * identical pose in the Hi-Z-on and Hi-Z-off runs → any diff is a cull delta,
     * not a camera mismatch. */
    /* JCE_BENCH_CAM="yaw,pitch,dist[,tx,ty,tz]" (yaw/pitch in degrees) pins the
     * viewport pose every frame.
     *
     * Without it a benchmark does not measure the same scene twice. The editor
     * persists camera pose in per-user session state, so the 200k bench has
     * been recorded at 3.10 ms / 45 draws / ~25k visible and, after the pose
     * drifted, at 19.07 ms / 133 draws with all 200,833 entities in one
     * instanced batch -- both real, neither comparable. That drift has now
     * produced two defects on its own: it hid a transient-pool overrun until
     * the whole scene came into view, and it broke a memo keyed on
     * frame_list_gen. Pinning is the fix for the measurement, not for either
     * defect.
     *
     * Applied AFTER any other camera work each frame so nothing can move it,
     * and before the spin below, which then advances from this yaw. */
    {
        static int   s_bcam = -2;          /* -2 unparsed, 0 off, 1 on */
        static float s_bc[6] = { 0, 0, 0, 0, 0, 0 };
        static bool  s_bc_target = false;
        if (s_bcam == -2) {
            s_bcam = 0;
            const char *v = getenv("JCE_BENCH_CAM");
            if (v && v[0]) {
                int n = sscanf(v, "%f,%f,%f,%f,%f,%f", &s_bc[0], &s_bc[1],
                               &s_bc[2], &s_bc[3], &s_bc[4], &s_bc[5]);
                if (n >= 3) { s_bcam = 1; s_bc_target = (n >= 6); }
                else LOG_WARN("scene_render", "JCE_BENCH_CAM needs at least "
                              "yaw,pitch,dist -- ignoring '%s'", v);
            }
        }
        if (s_bcam == 1 && s_sr.initialized && s_sr.camera) {
            s_sr.orbit_yaw      = s_bc[0] * JCE_DEG2RAD;
            s_sr.orbit_pitch    = s_bc[1] * JCE_DEG2RAD;
            s_sr.orbit_distance = s_bc[2] > 0.0f ? s_bc[2] : s_sr.orbit_distance;
            if (s_bc_target) s_sr.orbit_target = jce_v3(s_bc[3], s_bc[4], s_bc[5]);
            s_sr.camera_cache_valid = false;
            s_sr.orbit_clip_valid   = false;
            orbit_apply();
        }
    }

    {
        static float s_spin = -999.0f;
        static float s_spin_base = 0.0f;
        static uint32_t s_spin_n = 0;
        static int   s_spin_cap = -2;     /* capture step, -1 = off */
        static char  s_spin_cap_path[512];
        if (s_spin < -900.0f) { const char *e = getenv("JCE_DBG_VISTA_SPIN");
                                s_spin = e ? (float)atof(e) : 0.0f; }
        if (s_spin != 0.0f && s_sr.initialized && s_sr.camera) {
            if (s_spin_n == 0) {
                /* ABSOLUTE base, not the yaw that happens to be there.
                 * The editor persists camera pose in its per-user session
                 * state, so a relative base starts each run where the previous
                 * one stopped: measured 92.174 rad in one run and 94.258 in the
                 * next, same build. Every pixel of the 3D view differed, which
                 * is what a shadow A/B was being compared against. */
                const char *sb = getenv("JCE_DBG_VISTA_SPIN_START");
                /* JCE_BENCH_CAM pinned the yaw just above; start from it so the
                 * two switches compose instead of fighting. */
                s_spin_base = (sb && sb[0]) ? (float)atof(sb) * JCE_DEG2RAD
                                            : s_sr.orbit_yaw;
            }
            /* Pose as a pure function of the step count, not an accumulator:
             * `yaw += step` also drifts with float accumulation, and more
             * importantly it made the pose depend on HOW MANY TIMES this ran. */
            s_sr.orbit_yaw = s_spin_base + s_spin * (float)s_spin_n * JCE_DEG2RAD;
            s_spin_n++;
            s_sr.camera_cache_valid = false;
            s_sr.orbit_clip_valid   = false;
            orbit_apply();

            /* Capture keyed to the SAME counter that placed the camera.
             *
             * The comment above claims this spin is frame-count-deterministic so
             * that "frame N lands on the identical pose" in an A/B. It was not:
             * this runs per viewport render while JCE_WINCAP_FRAME counts editor
             * updates, and the ratio varies run to run, so frame N landed on a
             * different yaw each time. Two captures of the SAME BUILD differed
             * across 19% of pixels, which is what a shadow A/B was being
             * compared against.
             *
             * JCE_DBG_VISTA_SPIN_CAPTURE=<n> requests the window capture on spin
             * step n instead, into JCE_WINCAP_PATH. Pose and capture then share
             * one counter by construction and cannot drift apart. */
            if (s_spin_cap == -2) {
                s_spin_cap = -1;
                const char *cs = getenv("JCE_DBG_VISTA_SPIN_CAPTURE");
                const char *cp = getenv("JCE_WINCAP_PATH");
                if (cs && cs[0] && cp && cp[0]) {
                    s_spin_cap = atoi(cs);
                    snprintf(s_spin_cap_path, sizeof s_spin_cap_path, "%s", cp);
                }
            }
            if (s_spin_cap > 0 && s_spin_n == (uint32_t)s_spin_cap) {
                const jce_vec3 cp = jce_camera_get_position(s_sr.camera);
                LOG_INFO("scene_render",
                    "spin-capture step %u: yaw=%.6f base=%.6f pitch=%.6f dist=%.4f "
                    "target=(%.4f,%.4f,%.4f) pos=(%.4f,%.4f,%.4f)",
                    s_spin_n, (double)s_sr.orbit_yaw, (double)s_spin_base,
                    (double)s_sr.orbit_pitch, (double)s_sr.orbit_distance,
                    (double)s_sr.orbit_target.x, (double)s_sr.orbit_target.y,
                    (double)s_sr.orbit_target.z,
                    (double)cp.x, (double)cp.y, (double)cp.z);
                jce_imgui_renderer_request_capture(s_spin_cap_path);
                s_spin_cap = -1;
            }
        }
    }
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

void jce_editor_scene_get_orbit(float *out_target3, float *out_distance,
                                float *out_pitch_deg, float *out_yaw_deg)
{
    if (out_target3) {
        out_target3[0] = s_sr.orbit_target.x;
        out_target3[1] = s_sr.orbit_target.y;
        out_target3[2] = s_sr.orbit_target.z;
    }
    if (out_distance)  *out_distance  = s_sr.orbit_distance;
    /* Degrees, because that is what tools/envshot.py takes and what a person
     * reads. Converting here rather than at the call site keeps the one
     * conversion in the file that owns the radians. */
    if (out_pitch_deg) *out_pitch_deg = s_sr.orbit_pitch * (180.0f / 3.14159265f);
    if (out_yaw_deg)   *out_yaw_deg   = s_sr.orbit_yaw   * (180.0f / 3.14159265f);
}

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

void jce_editor_scene_camera_set_projection(bool orthographic,
                                            float fov_deg,
                                            float near_plane,
                                            float far_plane,
                                            float ortho_width,
                                            float ortho_height)
{
    if (!s_sr.initialized || !s_sr.camera) return;

    jce_camera_set_mode(s_sr.camera, orthographic
        ? JCE_CAMERA_ORTHO : JCE_CAMERA_PERSPECTIVE);
    jce_camera_set_fov(s_sr.camera, fov_deg);
    jce_camera_set_near_far(s_sr.camera, near_plane, far_plane);
    if (orthographic)
        jce_camera_set_ortho_size(s_sr.camera, ortho_width, ortho_height);
    s_sr.camera_cache_valid = false;
}

bool jce_editor_scene_camera_restore_pose(const char *scene_path)
{
    if (!s_sr.initialized || !scene_path || !scene_path[0]) return false;
    /* Distance doubles as the presence probe: a stored pose always has a
     * positive distance, so the negative fallback means "no pose yet" —
     * leave the default framing untouched. */
    float dist = jce_editor_pstate_scene_get_float(scene_path, "cam.dist", -1.0f);
    if (dist <= 0.0f) return false;
    float target[3];
    target[0]   = jce_editor_pstate_scene_get_float(scene_path, "cam.tx",    0.0f);
    target[1]   = jce_editor_pstate_scene_get_float(scene_path, "cam.ty",    0.0f);
    target[2]   = jce_editor_pstate_scene_get_float(scene_path, "cam.tz",    0.0f);
    float yaw   = jce_editor_pstate_scene_get_float(scene_path, "cam.yaw",   0.0f);
    float pitch = jce_editor_pstate_scene_get_float(scene_path, "cam.pitch", 0.0f);
    jce_editor_scene_camera_set_state(target, yaw, pitch, dist);
    return true;
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
     * deterministic and comparable across tuning passes.  A focus animation armed
     * on scene load would otherwise re-drive orbit_target/distance every frame via
     * jce_editor_scene_camera_update — at a FRAME-RATE-DEPENDENT rate — so two runs
     * of different speed (e.g. GPU-cull on vs off) land on DIFFERENT cameras at the
     * same capture frame.  Cancel it so the vista truly stays put. */
    orbit_cancel_focus_anim();
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
      if ((e = getenv("JCE_DBG_VISTA_YAW")))   s_sr.orbit_yaw   = (float)atof(e) * JCE_DEG2RAD;
      /* JCE_DBG_VISTA_FOV: narrow the camera cone so a huge instance field falls
       * mostly off-screen (verifies GPU frustum cull — see JCE_DBG_VISTA_FAR). */
      if ((e = getenv("JCE_DBG_VISTA_FOV")))   jce_camera_set_fov(s_sr.camera, (float)atof(e)); }
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
