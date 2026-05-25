/*
 * jce_scene_render_draw.cpp  Editor overlay passes.
 *
 * Phase B refactor: sky, shadows, and entity rendering live in the engine
 * `JceSceneRenderer`. This file now only contains overlay passes drawn on
 * top of the engine output: grid, selection outlines, physics debug,
 * ghost, hover.
 */

#include "jce_scene_render_internal.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_physics_debug.h>
}

#include "../core/jce_editor_state.h"
#include "../gizmo/jce_gizmo_joint.h"
#include "../gizmo/jce_gizmo_cloth.h"

/* ── Animation timer reset (kept for play.cpp compatibility) ─────── */

void jce_editor_scene_reset_anim_timer(void)
{
    s_sr.anim_last_ticks = 0;
}

/* ── Local entity model builder (overlay-only) ───────────────────── */

/* Builds a TRS model matrix from the entity's Transform component and
 * resolves its mesh from the editor asset cache.  Procedural-shape
 * fallback meshes are not provided here: overlays simply skip entities
 * whose mesh isn't loadable from a file path. */
static bool build_overlay_entity_model(uint32_t entity_id,
                                        jce_mat4 *out_model,
                                        JceMesh **out_mesh)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene || entity_id == 0) return false;
    JceEntity e = (JceEntity)entity_id;

    JceTransform *t = jce_scene_get_transform(scene, e);
    if (!t) return false;

    float sx = (t->scale.x != 0.0f) ? t->scale.x : 1.0f;
    float sy = (t->scale.y != 0.0f) ? t->scale.y : 1.0f;
    float sz = (t->scale.z != 0.0f) ? t->scale.z : 1.0f;
    *out_model = jce_m4_from_trs(t->position, t->rotation,
                                  jce_v3(sx, sy, sz));

    if (out_mesh) {
        *out_mesh = NULL;
        if (jce_scene_has_mesh_renderer(scene, e)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            if (mr && mr->mesh_path[0] != '\0') {
                float wp[3] = { t->position.x, t->position.y, t->position.z };
                *out_mesh = get_cached_mesh(mr->mesh_path, wp);
            }
        }
    }

    return true;
}

/* ── Infinite Grid Rendering (Blender-like fullscreen shader) ─────── */

void draw_grid(void)
{
    if (!jce_program_valid(s_sr.prog_grid)) return;

    /* Position-only fullscreen quad in NDC. */
    JceVertexLayout layout;
    jce_vertex_layout_begin(&layout);
    jce_vertex_layout_add(&layout, JCE_ATTRIB_POSITION, 3,
                          JCE_ATTRIB_TYPE_FLOAT, false, false);
    jce_vertex_layout_end(&layout);

    struct GridVertex { float x, y, z; };
    JceTransientVertexBuffer tvb;
    JceTransientIndexBuffer  tib;
    if (!jce_alloc_transient_buffers(&tvb, &layout, 4, &tib, 6, false))
        return;

    GridVertex *v = (GridVertex *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;
    v[0] = { -1.0f, -1.0f, 0.0f };
    v[1] = {  1.0f, -1.0f, 0.0f };
    v[2] = {  1.0f,  1.0f, 0.0f };
    v[3] = { -1.0f,  1.0f, 0.0f };
    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    float fade_near = fmaxf(16.0f, s_sr.orbit_distance * 3.0f);
    float fade_far  = fmaxf(fade_near + 40.0f, s_sr.orbit_distance * 24.0f);
    float grid_camera[4] = { cam_pos.x, cam_pos.y, cam_pos.z, 0.0f };
    float grid_fade[4] = { fade_near, fade_far, 10.0f, 1.0f };

    jce_uniform_set(s_sr.u_grid_camera, grid_camera, 1);
    jce_uniform_set(s_sr.u_grid_fade, grid_fade, 1);
    jce_set_transient_vertex_buffer(0, &tvb, 0, 4);
    jce_set_transient_index_buffer(&tib, 0, 6);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_WRITE_A
                   | JCE_STATE_MSAA
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_SRC_ALPHA,
                                          JCE_BLEND_INV_SRC_ALPHA);
    jce_set_state(state, 0);

    jce_mat4 identity = jce_m4_identity();
    jce_set_transform(identity.raw[0], 1);
    jce_submit(scene_view_id(), s_sr.prog_grid, 0, JCE_DISCARD_ALL);
}

/* ── Selection outlines ───────────────────────────────────────────── */

/* Draw 4 axial rays + an end-cap ring → cone gizmo for spot lights. */
static void outline_draw_cone(jce_vec3 apex, jce_vec3 axis_unit,
                              float length, float half_angle_rad,
                              uint32_t abgr)
{
    if (length <= 0.0f) length = 1.0f;
    if (half_angle_rad < 0.01f) half_angle_rad = 0.01f;

    /* Build an orthonormal basis (axis, u, v). */
    jce_vec3 up = (fabsf(axis_unit.y) < 0.95f)
                  ? jce_v3(0.0f, 1.0f, 0.0f)
                  : jce_v3(1.0f, 0.0f, 0.0f);
    jce_vec3 u = jce_v3_normalize(jce_v3_cross(axis_unit, up));
    jce_vec3 v = jce_v3_cross(axis_unit, u);

    jce_vec3 base   = jce_v3_add(apex, jce_v3_scale(axis_unit, length));
    float    radius = length * tanf(half_angle_rad);

    /* End-cap ring (16 segments). */
    const int seg = 16;
    jce_vec3 prev = base;
    for (int k = 0; k <= seg; k++) {
        float a = (float)k * (6.2831853f / (float)seg);
        jce_vec3 p = jce_v3_add(base,
                        jce_v3_add(jce_v3_scale(u, cosf(a) * radius),
                                   jce_v3_scale(v, sinf(a) * radius)));
        if (k > 0) jce_debug_draw_line(prev, p, abgr);
        prev = p;
    }

    /* 4 axial rays from apex to ring at 0°, 90°, 180°, 270°. */
    for (int k = 0; k < 4; k++) {
        float a = (float)k * (3.1415927f * 0.5f);
        jce_vec3 p = jce_v3_add(base,
                        jce_v3_add(jce_v3_scale(u, cosf(a) * radius),
                                   jce_v3_scale(v, sinf(a) * radius)));
        jce_debug_draw_line(apex, p, abgr);
    }
}

/* Draw a perspective/ortho frustum gizmo for camera entities. */
static void outline_draw_frustum(jce_vec3 origin, jce_quat rot,
                                  float fov_deg, float near_z, float far_z,
                                  bool ortho, uint32_t abgr)
{
    if (near_z <= 0.0f) near_z = 0.1f;
    if (far_z  <= near_z) far_z = near_z + 1.0f;

    /* Right-handed: forward = -Z, up = +Y, right = +X. */
    jce_vec3 fwd   = jce_q_rotate(rot, jce_v3(0.0f, 0.0f, -1.0f));
    jce_vec3 up    = jce_q_rotate(rot, jce_v3(0.0f, 1.0f,  0.0f));
    jce_vec3 right = jce_q_rotate(rot, jce_v3(1.0f, 0.0f,  0.0f));

    const float aspect = 16.0f / 9.0f;
    float hn, wn, hf, wf;
    if (ortho) {
        hn = hf = 1.0f;
        wn = wf = aspect;
    } else {
        float t = tanf(fov_deg * 0.5f * 3.1415927f / 180.0f);
        hn = near_z * t;
        wn = hn * aspect;
        hf = far_z  * t;
        wf = hf * aspect;
    }

    jce_vec3 nc = jce_v3_add(origin, jce_v3_scale(fwd, near_z));
    jce_vec3 fc = jce_v3_add(origin, jce_v3_scale(fwd, far_z));

    jce_vec3 ntl = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up,  hn), jce_v3_scale(right, -wn)));
    jce_vec3 ntr = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up,  hn), jce_v3_scale(right,  wn)));
    jce_vec3 nbl = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up, -hn), jce_v3_scale(right, -wn)));
    jce_vec3 nbr = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up, -hn), jce_v3_scale(right,  wn)));
    jce_vec3 ftl = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up,  hf), jce_v3_scale(right, -wf)));
    jce_vec3 ftr = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up,  hf), jce_v3_scale(right,  wf)));
    jce_vec3 fbl = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up, -hf), jce_v3_scale(right, -wf)));
    jce_vec3 fbr = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up, -hf), jce_v3_scale(right,  wf)));

    /* Near rect. */
    jce_debug_draw_line(ntl, ntr, abgr); jce_debug_draw_line(ntr, nbr, abgr);
    jce_debug_draw_line(nbr, nbl, abgr); jce_debug_draw_line(nbl, ntl, abgr);
    /* Far rect. */
    jce_debug_draw_line(ftl, ftr, abgr); jce_debug_draw_line(ftr, fbr, abgr);
    jce_debug_draw_line(fbr, fbl, abgr); jce_debug_draw_line(fbl, ftl, abgr);
    /* Connectors. */
    jce_debug_draw_line(ntl, ftl, abgr); jce_debug_draw_line(ntr, ftr, abgr);
    jce_debug_draw_line(nbl, fbl, abgr); jce_debug_draw_line(nbr, fbr, abgr);
    /* Apex stub so the origin is visible for ortho cameras too. */
    jce_debug_draw_line(origin, nc, abgr);
}

/* Draws a highlight on every selected entity:
 *   - With a static mesh        → wireframe overlay on the actual geometry.
 *   - Point/Spot light          → real influence sphere or cone.
 *   - Camera                    → real view frustum (fov / near / far).
 *   - Box/Sphere/Capsule/CC     → real collider shape.
 *   - Anything else (empties,
 *     audio sources, particles,
 *     prefab roots, …)         → AABB sized by transform.scale.
 *
 * Goal: selection feedback that visually matches each entity's actual
 * shape — not just static meshes. */
void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    float flat_dir[4]    = { 0.0f, -1.0f, 0.0f, -1.0f };
    float flat_color[4]  = { 1.0f, 0.75f, 0.0f, 1.0f };

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);

    /* ABGR (debug-draw convention). Orange-amber matches the
     * wireframe overlay tone used for selected meshes. */
    const uint32_t col_outline = 0xFF00BFFF;
    bool drew_any_debug = false;

    JceScene *scene = jce_state_get_scene();

    for (int i = 0; i < sel_count; i++) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;

        /* --- Static mesh: keep the high-fidelity wireframe overlay. */
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (build_overlay_entity_model(id, &model, &mesh) && mesh) {
            jce_set_transform(model.raw[0], 1);
            jce_uniform_set(s_sr.u_light_dir,   flat_dir,   1);
            jce_uniform_set(s_sr.u_light_color, flat_color, 1);
            jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);
            jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer,
                                              scene_view_id());
            continue;
        }

        if (!scene) continue;
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (!t) continue;

        bool drew_shape = false;

        /* --- Point light → influence sphere. */
        if (jce_scene_has_point_light(scene, e)) {
            JcePointLight *pl = jce_scene_get_point_light(scene, e);
            float r = (pl && pl->radius > 0.0f) ? pl->radius : 1.0f;
            jce_debug_draw_sphere(t->position, r, col_outline);
            drew_shape = true;
        }

        /* --- Spot light → real cone (apex, axis, length, opening). */
        if (jce_scene_has_spot_light(scene, e)) {
            JceSpotLight *sl = jce_scene_get_spot_light(scene, e);
            if (sl) {
                jce_vec3 axis = jce_v3_normalize(sl->direction);
                if (axis.x == 0.0f && axis.y == 0.0f && axis.z == 0.0f)
                    axis = jce_q_rotate(t->rotation, jce_v3(0, 0, -1));
                float len   = (sl->radius > 0.0f) ? sl->radius : 1.0f;
                float cosA  = (sl->outer_cone_cos > 0.0f)
                              ? sl->outer_cone_cos : 0.7071f;
                if (cosA > 0.9999f) cosA = 0.9999f;
                outline_draw_cone(t->position, axis, len,
                                  acosf(cosA), col_outline);
            }
            drew_shape = true;
        }

        /* --- Camera → real view frustum. */
        if (jce_scene_has_camera(scene, e)) {
            JceCameraComponent *cc = jce_scene_get_camera(scene, e);
            if (cc) {
                outline_draw_frustum(t->position, t->rotation,
                                     (cc->fov_deg > 0.0f) ? cc->fov_deg : 60.0f,
                                     (cc->near_plane > 0.0f) ? cc->near_plane : 0.1f,
                                     (cc->far_plane > cc->near_plane) ? cc->far_plane : 100.0f,
                                     cc->ortho, col_outline);
            }
            drew_shape = true;
        }

        /* --- Collider shapes (real geometry).  size/radius are
         *     interpreted in local entity space and scaled by the
         *     entity's TRS scale, matching Unity-style authoring. */
        if (jce_scene_has_box_collider(scene, e)) {
            JceBoxColliderComponent *bc = jce_scene_get_box_collider(scene, e);
            jce_vec3 ofs = bc
                ? jce_v3(bc->center[0], bc->center[1], bc->center[2])
                : jce_v3(0, 0, 0);
            float sx = (t->scale.x != 0.0f) ? fabsf(t->scale.x) : 1.0f;
            float sy = (t->scale.y != 0.0f) ? fabsf(t->scale.y) : 1.0f;
            float sz = (t->scale.z != 0.0f) ? fabsf(t->scale.z) : 1.0f;
            float bx = bc ? bc->size[0] : 1.0f;
            float by = bc ? bc->size[1] : 1.0f;
            float bz = bc ? bc->size[2] : 1.0f;
            jce_vec3 half = jce_v3(0.5f * bx * sx, 0.5f * by * sy, 0.5f * bz * sz);
            jce_vec3 c = jce_v3_add(t->position,
                              jce_q_rotate(t->rotation,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_box(c, half, t->rotation, col_outline);
            drew_shape = true;
        }
        if (jce_scene_has_sphere_collider(scene, e)) {
            JceSphereColliderComponent *sc = jce_scene_get_sphere_collider(scene, e);
            jce_vec3 ofs = sc
                ? jce_v3(sc->center[0], sc->center[1], sc->center[2])
                : jce_v3(0, 0, 0);
            float sx = (t->scale.x != 0.0f) ? fabsf(t->scale.x) : 1.0f;
            float sy = (t->scale.y != 0.0f) ? fabsf(t->scale.y) : 1.0f;
            float sz = (t->scale.z != 0.0f) ? fabsf(t->scale.z) : 1.0f;
            float smax = fmaxf(sx, fmaxf(sy, sz));
            float r = ((sc && sc->radius > 0.0f) ? sc->radius : 0.5f) * smax;
            jce_vec3 c = jce_v3_add(t->position,
                              jce_q_rotate(t->rotation,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_sphere(c, r, col_outline);
            drew_shape = true;
        }
        if (jce_scene_has_capsule_collider(scene, e)) {
            JceCapsuleColliderComponent *cc = jce_scene_get_capsule_collider(scene, e);
            jce_vec3 ofs = cc
                ? jce_v3(cc->center[0], cc->center[1], cc->center[2])
                : jce_v3(0, 0, 0);
            float sx = (t->scale.x != 0.0f) ? fabsf(t->scale.x) : 1.0f;
            float sy = (t->scale.y != 0.0f) ? fabsf(t->scale.y) : 1.0f;
            float sz = (t->scale.z != 0.0f) ? fabsf(t->scale.z) : 1.0f;
            float r_scale = fmaxf(sx, sz);
            float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * r_scale;
            float h = ((cc && cc->height > 0.0f) ? cc->height : 1.0f) * sy;
            float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
            jce_vec3 c = jce_v3_add(t->position,
                              jce_q_rotate(t->rotation,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_capsule(c, r, hh, t->rotation, col_outline);
            drew_shape = true;
        }
        if (jce_scene_has_character_controller(scene, e)) {
            JceCharacterControllerComponent *cc =
                jce_scene_get_character_controller(scene, e);
            float sx = (t->scale.x != 0.0f) ? fabsf(t->scale.x) : 1.0f;
            float sy = (t->scale.y != 0.0f) ? fabsf(t->scale.y) : 1.0f;
            float sz = (t->scale.z != 0.0f) ? fabsf(t->scale.z) : 1.0f;
            float r_scale = fmaxf(sx, sz);
            float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * r_scale;
            float h = ((cc && cc->height > 0.0f) ? cc->height : 1.6f) * sy;
            float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
            jce_debug_draw_capsule(t->position, r, hh, t->rotation, col_outline);
            drew_shape = true;
        }

        /* --- Skeletal-animated (skinned) model.
         *     Submit the true geometric wireframe of every primitive in
         *     the cached JceModel, walking the full node hierarchy. The
         *     bone palette comes from the live animation player when
         *     available; otherwise the bind pose is used. */
        if (!drew_shape && jce_scene_has_skeletal_animator(scene, e)) {
            JceSkeletalAnimatorComponent *sa =
                jce_scene_get_skeletal_animator(scene, e);
            JceModel *mdl = (sa && sa->skeleton_path[0])
                ? jce_editor_scene_get_model(sa->skeleton_path, id)
                : NULL;
            if (mdl) {
                float sx = (t->scale.x != 0.0f) ? t->scale.x : 1.0f;
                float sy = (t->scale.y != 0.0f) ? t->scale.y : 1.0f;
                float sz = (t->scale.z != 0.0f) ? t->scale.z : 1.0f;
                jce_mat4 world = jce_m4_from_trs(t->position, t->rotation,
                                                  jce_v3(sx, sy, sz));
                jce_uniform_set(s_sr.u_light_dir,   flat_dir,   1);
                jce_uniform_set(s_sr.u_light_color, flat_color, 1);
                jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);
                jce_model_submit_wireframe_overlay(mdl, s_sr.renderer,
                                                    scene_view_id(),
                                                    &world, NULL, 0);
                drew_shape = true;
            } else {
                /* Model not cached yet: humanoid AABB placeholder. */
                float sx = (t->scale.x != 0.0f) ? fabsf(t->scale.x) : 1.0f;
                float sy = (t->scale.y != 0.0f) ? fabsf(t->scale.y) : 1.0f;
                float sz = (t->scale.z != 0.0f) ? fabsf(t->scale.z) : 1.0f;
                jce_vec3 half = jce_v3(0.25f * sx, 1.0f * sy, 0.25f * sz);
                jce_vec3 center = jce_v3_add(t->position,
                                      jce_q_rotate(t->rotation,
                                          jce_v3(0.0f, 1.0f * sy, 0.0f)));
                jce_debug_draw_box(center, half, t->rotation, col_outline);
                drew_shape = true;
            }
        }

        /* --- Generic fallback for transform-only entities (empties,
         *     audio sources, particle emitters, prefab roots …). */
        if (!drew_shape) {
            float sx = (t->scale.x != 0.0f) ? fabsf(t->scale.x) : 1.0f;
            float sy = (t->scale.y != 0.0f) ? fabsf(t->scale.y) : 1.0f;
            float sz = (t->scale.z != 0.0f) ? fabsf(t->scale.z) : 1.0f;
            jce_vec3 half = jce_v3(0.5f * sx, 0.5f * sy, 0.5f * sz);
            jce_debug_draw_box(t->position, half, t->rotation, col_outline);
        }

        drew_any_debug = true;
    }

    if (drew_any_debug)
        jce_debug_draw_flush(scene_view_id(), s_sr.renderer);

    /* Restore default lighting so subsequent draws aren't tinted. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Physics debug visualization ──────────────────────────────────── */

void draw_physics_debug(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    const uint32_t col_box     = 0xFF00FF00; /* green */
    const uint32_t col_sphere  = 0xFF00FFFF; /* cyan */
    const uint32_t col_capsule = 0xFFFFFF00; /* yellow */

    for (int i = 0; i < count; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (!t) continue;

        jce_vec3 center = t->position;
        jce_quat q = t->rotation;

        if (jce_scene_has_box_collider(scene, e)) {
            float sx = (t->scale.x > 0) ? t->scale.x : 1.0f;
            float sy = (t->scale.y > 0) ? t->scale.y : 1.0f;
            float sz = (t->scale.z > 0) ? t->scale.z : 1.0f;
            jce_vec3 half = jce_v3(0.5f * sx, 0.5f * sy, 0.5f * sz);
            jce_debug_draw_box(center, half, q, col_box);
        }
        if (jce_scene_has_sphere_collider(scene, e)) {
            float r = (t->scale.x > 0) ? t->scale.x * 0.5f : 0.5f;
            jce_debug_draw_sphere(center, r, col_sphere);
        }
        if (jce_scene_has_character_controller(scene, e)) {
            float radius = 0.3f;
            float half_h = 0.6f;
            jce_debug_draw_capsule(center, radius, half_h, q, col_capsule);
        }
    }

    jce_debug_draw_flush(scene_view_id(), s_sr.renderer);

    /* P3-C.5 — flush Bullet's debug-draw (wireframes / AABBs / contacts)
     * for the live Play world.  The line sink + flag mask are installed
     * once at editor startup (see jce_panel_physics_debugger.cpp). */
    JcePhysicsWorld *pw = jce_editor_play_get_physics_world();
    if (pw && jce_physics_debug_get_flags() != 0) {
        jce_physics_debug_flush(pw);
        jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
    }
}

/* ── Joint gizmos (P3-C.6) ────────────────────────────────────────── */

/* Draw Unity-parity joint visualisation (anchors, A↔B line, axis,
 * type-specific limit geometry) for every selected entity that owns a
 * JceConstraintComponent.  Selection-driven by design — gizmos for all
 * joints would clutter Scene View. */
void draw_joint_gizmos(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (!sel || sel_count <= 0) return;

    bool drew_any = false;
    for (int i = 0; i < sel_count; ++i) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        if (!jce_scene_has_constraint(scene, e)) continue;

        JceConstraintComponent *c = jce_scene_get_constraint(scene, e);
        if (!c) continue;

        jce_gizmo_joint_draw_from_component(scene, e, c);
        drew_any = true;
    }

    if (drew_any) jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* Draw cloth wireframe for every selected entity that owns a
 * JceClothComponent.  Selection-driven for the same reason as joints. */
void draw_cloth_gizmos(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (!sel || sel_count <= 0) return;

    bool drew_any = false;
    for (int i = 0; i < sel_count; ++i) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        if (!jce_scene_has_cloth(scene, e)) continue;

        JceClothComponent *cl = jce_scene_get_cloth(scene, e);
        if (!cl) continue;

        jce_gizmo_cloth_draw_from_component(cl);
        drew_any = true;
    }

    if (drew_any) jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* ── Ghost (drag-preview) model rendering ─────────────────────────── */

void draw_ghost_entity(void)
{
    if (!s_sr.ghost_active || s_sr.ghost_mesh_path[0] == '\0')
        return;

    JceMesh *mesh = get_cached_mesh(s_sr.ghost_mesh_path, s_sr.ghost_pos);
    if (!mesh) return;

    jce_mat4 model = jce_m4_identity();
    model.raw[3][0] = s_sr.ghost_pos[0];
    model.raw[3][1] = s_sr.ghost_pos[1];
    model.raw[3][2] = s_sr.ghost_pos[2];
    jce_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_WRITE_A
                   | JCE_STATE_DEPTH_TEST_LESS
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_SRC_ALPHA,
                                          JCE_BLEND_INV_SRC_ALPHA)
                   | JCE_STATE_MSAA;
    jce_set_state(state, 0);

    float green_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float green_color[4] = { 0.2f, 0.9f, 0.3f, 0.45f };
    jce_uniform_set(s_sr.u_light_dir,   green_dir,   1);
    jce_uniform_set(s_sr.u_light_color, green_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());
}

/* ── Hover highlight for drag-drop onto entity ────────────────────── */

void draw_hover_highlight(void)
{
    if (s_sr.hover_entity_id == 0) return;

    uint32_t id = s_sr.hover_entity_id;
    if (!jce_state_entity_exists(id) || !jce_state_entity_enabled(id)) return;

    jce_mat4 model;
    JceMesh *mesh = NULL;
    if (!build_overlay_entity_model(id, &model, &mesh)) return;
    if (!mesh) return;

    jce_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_DEPTH_TEST_LEQUAL
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_ONE,
                                          JCE_BLEND_ONE)
                   | JCE_STATE_MSAA;
    jce_set_state(state, 0);

    float hover_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float hover_color[4] = { 0.28f, 0.28f, 0.34f, 1.0f };
    jce_uniform_set(s_sr.u_light_dir,   hover_dir,   1);
    jce_uniform_set(s_sr.u_light_color, hover_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());

    /* Restore default lighting so subsequent draws aren't tinted. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}
