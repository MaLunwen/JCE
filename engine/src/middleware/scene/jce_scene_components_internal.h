/* jce_scene_components_internal.h  Internal shared glue for the scene
 * component (de)serialize modules.
 *
 * The (formerly monolithic) jce_scene_components_json.c is split across
 * jce_scene_components_<domain>.c translation units that all share ONE set
 * of small JSON/Euler helper functions and ONE include surface.  The shared
 * helpers are `static inline` here so every TU gets its own copy with NO
 * link conflict (they are pure — no file-scope state).  The per-component
 * parse_/serw_ functions referenced by the central registry table (which
 * stays in jce_scene_components_json.c) are declared in the "Shared
 * component (de)serialize prototypes" section below so the registry can take
 * their address across TUs.
 *
 * Internal to the scene-serial implementation — NOT a public header and never
 * installed.  (Extracted from jce_scene_components_json.c.)
 */
#ifndef JCE_SCENE_COMPONENTS_INTERNAL_H
#define JCE_SCENE_COMPONENTS_INTERNAL_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/resource/jce_scene_contract.h>

#include "jce_component_registry_internal.h"
#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <SDL3/SDL_filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LOG_TAG
#define LOG_TAG "scene_serial"
#endif

/* ── Local Euler ↔ Quaternion helpers (degrees) ───────────────────── */

static inline void q_to_euler_deg(jce_quat q, float out[3])
{
    jce_vec3 e = jce_q_to_euler(q);
    out[0] = e.x * JCE_RAD2DEG;
    out[1] = e.y * JCE_RAD2DEG;
    out[2] = e.z * JCE_RAD2DEG;
}

static inline jce_quat q_from_euler_deg(float x_deg, float y_deg, float z_deg)
{
    return jce_q_from_euler(x_deg * JCE_DEG2RAD,
                            y_deg * JCE_DEG2RAD,
                            z_deg * JCE_DEG2RAD);
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

static inline double j_num(const cJSON *o, const char *k, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    return def;
}

static inline double j_num2(const cJSON *o, const char *k1, const char *k2, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k1);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    it = cJSON_GetObjectItemCaseSensitive(o, k2);
    if (cJSON_IsNumber(it)) return it->valuedouble;
    return def;
}

static inline bool j_bool(const cJSON *o, const char *k, bool def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return def;
}

static inline void j_float3(const cJSON *o, const char *k, float out[3],
                            const float def[3])
{
    if (!out) return;
    if (def) {
        out[0] = def[0];
        out[1] = def[1];
        out[2] = def[2];
    }
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) < 3)
        return;
    const cJSON *x = cJSON_GetArrayItem(it, 0);
    const cJSON *y = cJSON_GetArrayItem(it, 1);
    const cJSON *z = cJSON_GetArrayItem(it, 2);
    if (cJSON_IsNumber(x)) out[0] = (float)x->valuedouble;
    if (cJSON_IsNumber(y)) out[1] = (float)y->valuedouble;
    if (cJSON_IsNumber(z)) out[2] = (float)z->valuedouble;
}

static inline const char *j_str(const cJSON *o, const char *k, const char *def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return def;
}

/* Pointer-form variants: extract from a pre-fetched cJSON item so a field is
   looked up once (presence + value) instead of twice. Identical coercion. */
static inline double j_num_it(const cJSON *it, double def)
{
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}
static inline bool j_bool_it(const cJSON *it, bool def)
{
    if (cJSON_IsBool(it))   return cJSON_IsTrue(it);
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return def;
}
static inline const char *j_str_it(const cJSON *it, const char *def)
{
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return def;
}

static inline const char *j_str_any(const cJSON *o, const char *const *keys, int n)
{
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, keys[i]);
        if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    }
    return NULL;
}

static inline void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline int streq_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Clamp `v` to [lo,hi] then snap DOWN to the nearest power of two within that
 * range (the FFT core requires a power-of-two grid).  lo/hi are assumed powers
 * of two.  Used by the Water component's fft_resolution. */
static inline int clamp_pow2_i(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    int p = lo;
    while ((p << 1) <= v && (p << 1) <= hi) p <<= 1;
    return p;
}

static inline cJSON *json_float3(const float v[3])
{
    cJSON *a = cJSON_CreateArray();
    if (!a) return NULL;
    cJSON_AddItemToArray(a, cJSON_CreateNumber(v ? v[0] : 0.0f));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(v ? v[1] : 0.0f));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(v ? v[2] : 0.0f));
    return a;
}

/* ── Shared component (de)serialize prototypes ──────────────────────
 *
 * The central registry table in jce_scene_components_json.c references each
 * component's parse_<x> (PARSE) and serw_<x> (SER) by address; when those
 * definitions live in a domain module (jce_scene_components_<domain>.c) they
 * are external (no `static`) and declared here so the registry TU can resolve
 * them.  Each domain module fills in its own block below as it is extracted.
 * The per-component ser_<x> writers stay file-static inside their own domain
 * module (only their serw_<x> wrapper calls them). */

/* audio domain (jce_scene_components_audio.c) */
void parse_audio_source(JceScene *s, JceEntity e, const cJSON *c);
void parse_music_track(JceScene *s, JceEntity e, const cJSON *c);
void parse_audio_listener(JceScene *s, JceEntity e, const cJSON *c);
void parse_audio_reverb_zone(JceScene *s, JceEntity e, const cJSON *c);
void parse_audio_occlusion(JceScene *s, JceEntity e, const cJSON *c);
void serw_audio_source(JceScene *s, JceEntity e, cJSON *arr);
void serw_music_track(JceScene *s, JceEntity e, cJSON *arr);
void serw_audio_listener(JceScene *s, JceEntity e, cJSON *arr);
void serw_audio_reverb_zone(JceScene *s, JceEntity e, cJSON *arr);
void serw_audio_occlusion(JceScene *s, JceEntity e, cJSON *arr);

/* net domain (jce_scene_components_net.c) */
void parse_network_variable(JceScene *s, JceEntity e, const cJSON *c);
void parse_network_object(JceScene *s, JceEntity e, const cJSON *c);
void parse_net_transform(JceScene *s, JceEntity e, const cJSON *c);
void parse_net_animator(JceScene *s, JceEntity e, const cJSON *c);
void parse_net_rigidbody(JceScene *s, JceEntity e, const cJSON *c);
void serw_network_variable(JceScene *s, JceEntity e, cJSON *arr);
void serw_network_object(JceScene *s, JceEntity e, cJSON *arr);
void serw_net_transform(JceScene *s, JceEntity e, cJSON *arr);
void serw_net_animator(JceScene *s, JceEntity e, cJSON *arr);
void serw_net_rigidbody(JceScene *s, JceEntity e, cJSON *arr);

/* ui domain (jce_scene_components_ui.c) */
void parse_canvas(JceScene *s, JceEntity e, const cJSON *c);
void parse_canvas_group(JceScene *s, JceEntity e, const cJSON *c);
void parse_layout_group(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_image(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_text(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_button(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_slider(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_toggle(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_input_field(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_scroll_view(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_progress_bar(JceScene *s, JceEntity e, const cJSON *c);
void parse_ui_dropdown(JceScene *s, JceEntity e, const cJSON *c);
void serw_canvas(JceScene *s, JceEntity e, cJSON *arr);
void serw_canvas_group(JceScene *s, JceEntity e, cJSON *arr);
void serw_layout_group(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_image(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_text(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_button(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_slider(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_toggle(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_input_field(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_scroll_view(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_progress_bar(JceScene *s, JceEntity e, cJSON *arr);
void serw_ui_dropdown(JceScene *s, JceEntity e, cJSON *arr);

/* world domain (jce_scene_components_world.c) */
void parse_behavior_tree(JceScene *s, JceEntity e, const cJSON *c);
void parse_script(JceScene *s, JceEntity e, const cJSON *c);
void parse_spawn_manager(JceScene *s, JceEntity e, const cJSON *c);
void parse_weapon(JceScene *s, JceEntity e, const cJSON *c);
void parse_save_point(JceScene *s, JceEntity e, const cJSON *c);
void parse_nav_agent(JceScene *s, JceEntity e, const cJSON *c);
void parse_gas(JceScene *s, JceEntity e, const cJSON *c);
void serw_behavior_tree(JceScene *s, JceEntity e, cJSON *arr);
void serw_nav_agent(JceScene *s, JceEntity e, cJSON *arr);
void serw_gas(JceScene *s, JceEntity e, cJSON *arr);
void serw_script(JceScene *s, JceEntity e, cJSON *arr);
void serw_spawn_manager(JceScene *s, JceEntity e, cJSON *arr);
void serw_weapon(JceScene *s, JceEntity e, cJSON *arr);
void serw_save_point(JceScene *s, JceEntity e, cJSON *arr);

/* terrain domain (jce_scene_components_terrain.c) */
void parse_terrain(JceScene *s, JceEntity e, const cJSON *c);
void parse_vegetation_scatter(JceScene *s, JceEntity e, const cJSON *c);
void parse_water(JceScene *s, JceEntity e, const cJSON *c);
void parse_virtual_camera(JceScene *s, JceEntity e, const cJSON *c);
void parse_tilemap(JceScene *s, JceEntity e, const cJSON *c);
void parse_tilemap_collider2d(JceScene *s, JceEntity e, const cJSON *c);
void serw_terrain(JceScene *s, JceEntity e, cJSON *arr);
void serw_vegetation_scatter(JceScene *s, JceEntity e, cJSON *arr);
void serw_water(JceScene *s, JceEntity e, cJSON *arr);
void serw_virtual_camera(JceScene *s, JceEntity e, cJSON *arr);
void serw_tilemap(JceScene *s, JceEntity e, cJSON *arr);
void serw_tilemap_collider2d(JceScene *s, JceEntity e, cJSON *arr);

/* anim domain (jce_scene_components_anim.c) */
void parse_sprite_animator(JceScene *s, JceEntity e, const cJSON *c);
void parse_skeletal_animator(JceScene *s, JceEntity e, const cJSON *c);
void parse_animator(JceScene *s, JceEntity e, const cJSON *c);
void parse_sequence_player(JceScene *s, JceEntity e, const cJSON *c);
void parse_morph_weights(JceScene *s, JceEntity e, const cJSON *c);
void parse_ik_constraints(JceScene *s, JceEntity e, const cJSON *c);
void parse_foot_ik(JceScene *s, JceEntity e, const cJSON *c);
void parse_full_body_ik(JceScene *s, JceEntity e, const cJSON *c);
void parse_avatar(JceScene *s, JceEntity e, const cJSON *c);
void serw_sprite_animator(JceScene *s, JceEntity e, cJSON *arr);
void serw_animator(JceScene *s, JceEntity e, cJSON *arr);
void serw_skeletal_animator(JceScene *s, JceEntity e, cJSON *arr);
void serw_ik_constraints(JceScene *s, JceEntity e, cJSON *arr);
void serw_foot_ik(JceScene *s, JceEntity e, cJSON *arr);
void serw_full_body_ik(JceScene *s, JceEntity e, cJSON *arr);
void serw_sequence_player(JceScene *s, JceEntity e, cJSON *arr);
void serw_morph_weights(JceScene *s, JceEntity e, cJSON *arr);
void serw_avatar(JceScene *s, JceEntity e, cJSON *arr);

/* physics domain (jce_scene_components_physics.c) */
void parse_rigidbody(JceScene *s, JceEntity e, const cJSON *c);
void parse_rigidbody2d(JceScene *s, JceEntity e, const cJSON *c);
void parse_box_collider(JceScene *s, JceEntity e, const cJSON *c);
void parse_sphere_collider(JceScene *s, JceEntity e, const cJSON *c);
void parse_character_controller(JceScene *s, JceEntity e, const cJSON *c);
void parse_buoyancy(JceScene *s, JceEntity e, const cJSON *c);
void parse_trigger_volume(JceScene *s, JceEntity e, const cJSON *c);
void parse_capsule_collider(JceScene *s, JceEntity e, const cJSON *c);
void parse_mesh_collider(JceScene *s, JceEntity e, const cJSON *c);
void parse_compound_collider(JceScene *s, JceEntity e, const cJSON *c);
void parse_collider2d(JceScene *s, JceEntity e, const cJSON *c);
void parse_wheel_collider(JceScene *s, JceEntity e, const cJSON *c);
void parse_constant_force(JceScene *s, JceEntity e, const cJSON *c);
void parse_ragdoll(JceScene *s, JceEntity e, const cJSON *c);
void parse_fracture(JceScene *s, JceEntity e, const cJSON *c);
void parse_vehicle(JceScene *s, JceEntity e, const cJSON *c);
void parse_soft_body(JceScene *s, JceEntity e, const cJSON *c);
void parse_configurable_joint(JceScene *s, JceEntity e, const cJSON *c);
void parse_cloth(JceScene *s, JceEntity e, const cJSON *c);
void parse_joint2d(JceScene *s, JceEntity e, const cJSON *c);
void parse_constraint(JceScene *s, JceEntity e, const cJSON *c);
void serw_rigidbody(JceScene *s, JceEntity e, cJSON *arr);
void serw_rigidbody2d(JceScene *s, JceEntity e, cJSON *arr);
void serw_box_collider(JceScene *s, JceEntity e, cJSON *arr);
void serw_sphere_collider(JceScene *s, JceEntity e, cJSON *arr);
void serw_character_controller(JceScene *s, JceEntity e, cJSON *arr);
void serw_ragdoll(JceScene *s, JceEntity e, cJSON *arr);
void serw_fracture(JceScene *s, JceEntity e, cJSON *arr);
void serw_vehicle(JceScene *s, JceEntity e, cJSON *arr);
void serw_soft_body(JceScene *s, JceEntity e, cJSON *arr);
void serw_constraint(JceScene *s, JceEntity e, cJSON *arr);
void serw_buoyancy(JceScene *s, JceEntity e, cJSON *arr);
void serw_trigger_volume(JceScene *s, JceEntity e, cJSON *arr);
void serw_capsule_collider(JceScene *s, JceEntity e, cJSON *arr);
void serw_mesh_collider(JceScene *s, JceEntity e, cJSON *arr);
void serw_compound_collider(JceScene *s, JceEntity e, cJSON *arr);
void serw_collider2d(JceScene *s, JceEntity e, cJSON *arr);
void serw_wheel_collider(JceScene *s, JceEntity e, cJSON *arr);
void serw_constant_force(JceScene *s, JceEntity e, cJSON *arr);
void serw_configurable_joint(JceScene *s, JceEntity e, cJSON *arr);
void serw_cloth(JceScene *s, JceEntity e, cJSON *arr);
void serw_joint2d(JceScene *s, JceEntity e, cJSON *arr);

/* render domain (jce_scene_components_render.c) */
void parse_dir_light(JceScene *s, JceEntity e, const cJSON *c);
void parse_point_light(JceScene *s, JceEntity e, const cJSON *c);
void parse_spot_light(JceScene *s, JceEntity e, const cJSON *c);
void parse_skybox(JceScene *s, JceEntity e, const cJSON *c);
void parse_sprite_renderer(JceScene *s, JceEntity e, const cJSON *c);
void parse_unified_light(JceScene *s, JceEntity e, const cJSON *c);
void parse_lod_group(JceScene *s, JceEntity e, const cJSON *c);
void parse_trail_renderer(JceScene *s, JceEntity e, const cJSON *c);
void parse_line_renderer(JceScene *s, JceEntity e, const cJSON *c);
void parse_reflection_probe(JceScene *s, JceEntity e, const cJSON *c);
void parse_decal(JceScene *s, JceEntity e, const cJSON *c);
void parse_light_probe_group(JceScene *s, JceEntity e, const cJSON *c);
void parse_billboard_renderer(JceScene *s, JceEntity e, const cJSON *c);
void parse_volume(JceScene *s, JceEntity e, const cJSON *c);
void parse_occlusion_portal(JceScene *s, JceEntity e, const cJSON *c);
void serw_mesh_renderer(JceScene *s, JceEntity e, cJSON *arr);
void serw_light_unified(JceScene *s, JceEntity e, cJSON *arr);
void serw_skybox(JceScene *s, JceEntity e, cJSON *arr);
void serw_sprite_renderer(JceScene *s, JceEntity e, cJSON *arr);
void serw_lod_group(JceScene *s, JceEntity e, cJSON *arr);
void serw_trail_renderer(JceScene *s, JceEntity e, cJSON *arr);
void serw_line_renderer(JceScene *s, JceEntity e, cJSON *arr);
void serw_reflection_probe(JceScene *s, JceEntity e, cJSON *arr);
void serw_decal(JceScene *s, JceEntity e, cJSON *arr);
void serw_light_probe_group(JceScene *s, JceEntity e, cJSON *arr);
void serw_billboard_renderer(JceScene *s, JceEntity e, cJSON *arr);
void serw_volume(JceScene *s, JceEntity e, cJSON *arr);
void serw_occlusion_portal(JceScene *s, JceEntity e, cJSON *arr);

#endif /* JCE_SCENE_COMPONENTS_INTERNAL_H */
