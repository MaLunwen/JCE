/*
 * jce_scene_serial.c  Scene serialization implementation (cJSON).
 *
 * Converts the ECS scene graph to/from a JSON representation.
 * Supports all 22 component types, parent/child hierarchies, and editor metadata.
 *
 * Format (contract envelope):
 * {
 *   "contract": { "name": "jce.scene", "major": 1, "minor": 0 },
 *   "scene": {
 *     "version": 1,
 *     "entities": [
 *       {
 *         "name": "Player",
 *         "parent": "World",
 *         "transform": { "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1] },
 *         "mesh_renderer": { "mesh_path": "...", "visible": true, ... },
 *         "camera": { "fov_deg": 60, "near": 0.1, "far": 1000, "primary": true, "ortho": false },
 *         "dir_light": { "direction": [0,-1,0], "color": [1,1,1], "intensity": 1, "casts_shadow": false }
 *       }
 *     ]
 *   }
 * }
 *
 * Legacy formats are accepted on load:
 * - Root-level {"version":..., "entities":...} (old engine format)
 * - Editor "components" array with "type" field (e.g., {"type":"Transform", "posX":0, ...})
 */

#include <jce/resource/jce_scene_serial.h>
#include <jce/resource/jce_scene_contract.h>
#include <jce/scene/jce_scene.h>
#include <jce/core/jce_filesystem.h>
#include <jce/core/jce_log.h>

#include <physfs.h>
#include <cjson/cJSON.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <math.h>
#include "core/jce_memory.h"

#define LOG_TAG "scene_serial"

/* ── JSON helpers ──────────────────────────────────────────────────── */

static cJSON *vec3_to_json(jce_vec3 v)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v.x));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v.y));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v.z));
    return arr;
}

static jce_vec3 json_to_vec3(const cJSON *arr)
{
    jce_vec3 v = {{0, 0, 0}};
    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 3) return v;
    v.x = (float)cJSON_GetArrayItem(arr, 0)->valuedouble;
    v.y = (float)cJSON_GetArrayItem(arr, 1)->valuedouble;
    v.z = (float)cJSON_GetArrayItem(arr, 2)->valuedouble;
    return v;
}

static cJSON *quat_to_json(jce_quat q)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.x));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.y));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.z));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)q.w));
    return arr;
}

static jce_quat json_to_quat(const cJSON *arr)
{
    jce_quat q = GLMS_QUAT_IDENTITY_INIT;
    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 4) return q;
    q.x = (float)cJSON_GetArrayItem(arr, 0)->valuedouble;
    q.y = (float)cJSON_GetArrayItem(arr, 1)->valuedouble;
    q.z = (float)cJSON_GetArrayItem(arr, 2)->valuedouble;
    q.w = (float)cJSON_GetArrayItem(arr, 3)->valuedouble;
    return q;
}

static double json_get_number(const cJSON *parent, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItem(parent, key);
    if (!item || !cJSON_IsNumber(item)) return fallback;
    return item->valuedouble;
}

static bool read_contract_version(const cJSON *root,
                                  uint32_t *out_major,
                                  uint32_t *out_minor)
{
    if (out_major) *out_major = JCE_SCENE_CONTRACT_MAJOR;
    if (out_minor) *out_minor = JCE_SCENE_CONTRACT_MINOR;
    if (!root || !cJSON_IsObject(root)) return false;

    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_CONTRACT_KEY);
    if (cJSON_IsObject(contract)) {
        const cJSON *major = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MAJOR_KEY);
        const cJSON *minor = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MINOR_KEY);
        if (cJSON_IsNumber(major) && out_major)
            *out_major = (uint32_t)major->valueint;
        if (cJSON_IsNumber(minor) && out_minor)
            *out_minor = (uint32_t)minor->valueint;
        return true;
    }

    const cJSON *legacy_version = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_VERSION_KEY);
    if (cJSON_IsNumber(legacy_version)) {
        if (out_major)
            *out_major = (uint32_t)legacy_version->valueint;
        if (out_minor)
            *out_minor = 0;
        return true;
    }

    return false;
}

static cJSON *resolve_scene_container(cJSON *root)
{
    if (!root || !cJSON_IsObject(root))
        return root;

    cJSON *scene = cJSON_GetObjectItemCaseSensitive(root, JCE_SCENE_ROOT_KEY);
    return cJSON_IsObject(scene) ? scene : root;
}

/* ── Extended JSON helpers ─────────────────────────────────────────── */

static cJSON *float3_to_json(const float v[3])
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[0]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[1]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[2]));
    return arr;
}

static cJSON *float4_to_json(const float v[4])
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[0]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[1]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[2]));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)v[3]));
    return arr;
}

static void json_to_float3(const cJSON *arr, float out[3],
                           float d0, float d1, float d2)
{
    out[0] = d0; out[1] = d1; out[2] = d2;
    if (!arr || !cJSON_IsArray(arr)) return;
    int n = cJSON_GetArraySize(arr);
    if (n > 3) n = 3;
    for (int i = 0; i < n; i++)
        out[i] = (float)cJSON_GetArrayItem(arr, i)->valuedouble;
}

static void json_to_float4(const cJSON *arr, float out[4],
                           float d0, float d1, float d2, float d3)
{
    out[0] = d0; out[1] = d1; out[2] = d2; out[3] = d3;
    if (!arr || !cJSON_IsArray(arr)) return;
    int n = cJSON_GetArraySize(arr);
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++)
        out[i] = (float)cJSON_GetArrayItem(arr, i)->valuedouble;
}

static const char *json_get_str(const cJSON *parent, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(parent, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

static bool json_get_bool(const cJSON *parent, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItem(parent, key);
    if (!item) return fallback;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item) ? true : false;
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    return fallback;
}

static void add_str_ne(cJSON *obj, const char *key, const char *val)
{
    if (val && val[0] != '\0')
        cJSON_AddStringToObject(obj, key, val);
}

/* Try key1, then key2. Return number or fallback. */
static double json_get_num2(const cJSON *p, const char *k1, const char *k2,
                            double fb)
{
    const cJSON *item = cJSON_GetObjectItem(p, k1);
    if (!item || !cJSON_IsNumber(item))
        item = cJSON_GetObjectItem(p, k2);
    if (!item || !cJSON_IsNumber(item)) return fb;
    return item->valuedouble;
}

/* Try key1, then key2. Return string or NULL. */
static const char *json_get_str2(const cJSON *p, const char *k1, const char *k2)
{
    const char *s = json_get_str(p, k1);
    return s ? s : json_get_str(p, k2);
}

static bool json_get_bool2(const cJSON *p, const char *k1, const char *k2,
                           bool fb)
{
    const cJSON *item = cJSON_GetObjectItem(p, k1);
    if (!item) item = cJSON_GetObjectItem(p, k2);
    if (!item) return fb;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item) ? true : false;
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    return fb;
}

/* ── Serialize callback (per entity) ───────────────────────────────── */

typedef struct {
    JceEntity entity;
    int       save_id;
} SaveMapEntry;

typedef struct {
    JceScene      *scene;
    cJSON         *entities_array;
    SaveMapEntry  *map;
    int            map_count;
    int            map_cap;
    int            next_id;
} SaveCtx;

static void save_entity_cb(JceScene *s, JceEntity e, void *user_data)
{
    SaveCtx *ctx = (SaveCtx *)user_data;

    cJSON *ent = cJSON_CreateObject();
    if (!ent) return;

    /* Assign numeric save id and record mapping. */
    int id = ctx->next_id++;
    if (ctx->map_count >= ctx->map_cap) {
        ctx->map_cap *= 2;
        ctx->map = (SaveMapEntry *)JCE_REALLOC(ctx->map,
            (size_t)ctx->map_cap * sizeof(SaveMapEntry));
        if (!ctx->map) { cJSON_Delete(ent); return; }
    }
    ctx->map[ctx->map_count].entity  = e;
    ctx->map[ctx->map_count].save_id = id;
    ctx->map_count++;
    cJSON_AddNumberToObject(ent, "id", (double)id);

    /* Name: prefer EditorMeta.name (supports duplicates), fall back to ecs name. */
    const char *name = NULL;
    JceEditorMeta *em_name = jce_scene_get_editor_meta(s, e);
    if (em_name && em_name->name[0])
        name = em_name->name;
    else
        name = jce_scene_entity_name(s, e);
    cJSON_AddStringToObject(ent, "name", name ? name : "");

    /* Parent (name + numeric parentId for robust resolution). */
    JceEntity parent = jce_scene_get_parent(s, e);
    if (parent != JCE_ENTITY_INVALID) {
        /* Name-based for backward compat. */
        const char *pname_meta = NULL;
        JceEditorMeta *pem = jce_scene_get_editor_meta(s, parent);
        if (pem && pem->name[0]) pname_meta = pem->name;
        else pname_meta = jce_scene_entity_name(s, parent);
        if (pname_meta && pname_meta[0])
            cJSON_AddStringToObject(ent, "parent", pname_meta);

        /* Numeric parentId for robust resolution. */
        for (int mi = 0; mi < ctx->map_count; mi++) {
            if (ctx->map[mi].entity == parent) {
                cJSON_AddNumberToObject(ent, "parentId",
                                        (double)ctx->map[mi].save_id);
                break;
            }
        }
    }

    /* Transform. */
    JceTransform *t = jce_scene_get_transform(s, e);
    if (t) {
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddItemToObject(tc, "position", vec3_to_json(t->position));
        cJSON_AddItemToObject(tc, "rotation", quat_to_json(t->rotation));
        cJSON_AddItemToObject(tc, "scale",    vec3_to_json(t->scale));
        cJSON_AddItemToObject(ent, "transform", tc);
    }

    /* Mesh renderer. */
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
    if (mr) {
        cJSON *mc = cJSON_CreateObject();
        add_str_ne(mc, "mesh_path", mr->mesh_path);
        add_str_ne(mc, "material_path", mr->material_path);
        cJSON_AddNumberToObject(mc, "mesh_shape", mr->mesh_shape);
        cJSON_AddBoolToObject(mc, "visible", mr->visible);
        cJSON_AddItemToObject(mc, "base_color", float4_to_json(mr->base_color));
        cJSON_AddNumberToObject(mc, "metallic",     (double)mr->metallic);
        cJSON_AddNumberToObject(mc, "roughness",    (double)mr->roughness);
        cJSON_AddItemToObject(mc, "emissive", float3_to_json(mr->emissive));
        cJSON_AddNumberToObject(mc, "normal_scale", (double)mr->normal_scale);
        cJSON_AddNumberToObject(mc, "ao_strength",  (double)mr->ao_strength);
        cJSON_AddNumberToObject(mc, "alpha_mode",   mr->alpha_mode);
        cJSON_AddNumberToObject(mc, "alpha_cutoff", (double)mr->alpha_cutoff);
        cJSON_AddBoolToObject(mc, "double_sided", mr->double_sided);
        add_str_ne(mc, "albedo_tex",   mr->albedo_tex);
        add_str_ne(mc, "mr_tex",       mr->mr_tex);
        add_str_ne(mc, "normal_tex",   mr->normal_tex);
        add_str_ne(mc, "ao_tex",       mr->ao_tex);
        add_str_ne(mc, "emissive_tex", mr->emissive_tex);
        cJSON_AddItemToObject(ent, "mesh_renderer", mc);
    }

    /* Camera. */
    JceCameraComponent *cam = jce_scene_get_camera(s, e);
    if (cam) {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddNumberToObject(cc, "fov_deg", (double)cam->fov_deg);
        cJSON_AddNumberToObject(cc, "near",    (double)cam->near_plane);
        cJSON_AddNumberToObject(cc, "far",     (double)cam->far_plane);
        cJSON_AddBoolToObject(cc, "primary",   cam->is_primary);
        cJSON_AddBoolToObject(cc, "ortho",     cam->ortho);
        cJSON_AddItemToObject(ent, "camera", cc);
    }

    /* Directional light. */
    JceDirectionalLight *dl = jce_scene_get_dir_light(s, e);
    if (dl) {
        cJSON *lc = cJSON_CreateObject();
        cJSON_AddItemToObject(lc, "direction", vec3_to_json(dl->direction));
        cJSON_AddItemToObject(lc, "color",     vec3_to_json(dl->color));
        cJSON_AddNumberToObject(lc, "intensity",    (double)dl->intensity);
        cJSON_AddBoolToObject(lc, "casts_shadow", dl->casts_shadow);
        cJSON_AddItemToObject(ent, "dir_light", lc);
    }

    /* Point light. */
    JcePointLight *pl = jce_scene_get_point_light(s, e);
    if (pl) {
        cJSON *lc = cJSON_CreateObject();
        cJSON_AddItemToObject(lc, "color",     vec3_to_json(pl->color));
        cJSON_AddNumberToObject(lc, "intensity",    (double)pl->intensity);
        cJSON_AddNumberToObject(lc, "radius",       (double)pl->radius);
        cJSON_AddItemToObject(ent, "point_light", lc);
    }

    /* Spot light. */
    JceSpotLight *sl = jce_scene_get_spot_light(s, e);
    if (sl) {
        cJSON *lc = cJSON_CreateObject();
        cJSON_AddItemToObject(lc, "direction", vec3_to_json(sl->direction));
        cJSON_AddItemToObject(lc, "color",     vec3_to_json(sl->color));
        cJSON_AddNumberToObject(lc, "intensity",    (double)sl->intensity);
        cJSON_AddNumberToObject(lc, "radius",       (double)sl->radius);
        /* Store cone angles in degrees for human readability. */
        float inner_deg = acosf(sl->inner_cone_cos) * JCE_RAD2DEG;
        float outer_deg = acosf(sl->outer_cone_cos) * JCE_RAD2DEG;
        cJSON_AddNumberToObject(lc, "inner_cone_deg", (double)inner_deg);
        cJSON_AddNumberToObject(lc, "outer_cone_deg", (double)outer_deg);
        cJSON_AddItemToObject(ent, "spot_light", lc);
    }

    /* Skybox. */
    JceSkyboxComponent *sky = jce_scene_get_skybox(s, e);
    if (sky) {
        cJSON *sc = cJSON_CreateObject();
        add_str_ne(sc, "hdr_path", sky->hdr_path);
        cJSON_AddNumberToObject(sc, "rotation", (double)sky->rotation);
        cJSON_AddNumberToObject(sc, "exposure", (double)sky->exposure);
        cJSON_AddBoolToObject(sc, "use_as_ibl", sky->use_as_ibl);
        cJSON_AddItemToObject(ent, "skybox", sc);
    }

    /* Sprite renderer. */
    JceSpriteRendererComponent *sr = jce_scene_get_sprite_renderer(s, e);
    if (sr) {
        cJSON *sc = cJSON_CreateObject();
        add_str_ne(sc, "sprite_path", sr->sprite_path);
        cJSON_AddItemToObject(sc, "color", float4_to_json(sr->color));
        cJSON_AddBoolToObject(sc, "flip_x", sr->flip_x);
        cJSON_AddBoolToObject(sc, "flip_y", sr->flip_y);
        cJSON_AddNumberToObject(sc, "sorting_order", sr->sorting_order);
        cJSON_AddItemToObject(ent, "sprite_renderer", sc);
    }

    /* Sprite animator. */
    JceSpriteAnimatorComponent *sa = jce_scene_get_sprite_animator(s, e);
    if (sa) {
        cJSON *sc = cJSON_CreateObject();
        add_str_ne(sc, "sheet_path", sa->sheet_path);
        add_str_ne(sc, "atlas_path", sa->atlas_path);
        cJSON_AddNumberToObject(sc, "frame_width",  sa->frame_width);
        cJSON_AddNumberToObject(sc, "frame_height", sa->frame_height);
        add_str_ne(sc, "current_anim", sa->current_anim);
        cJSON_AddNumberToObject(sc, "speed", (double)sa->speed);
        cJSON_AddBoolToObject(sc, "loop", sa->loop);
        cJSON_AddItemToObject(ent, "sprite_animator", sc);
    }

    /* Animator. */
    JceAnimatorComponent *anim = jce_scene_get_animator(s, e);
    if (anim) {
        cJSON *ac = cJSON_CreateObject();
        add_str_ne(ac, "clip_name", anim->clip_name);
        cJSON_AddNumberToObject(ac, "speed", (double)anim->speed);
        cJSON_AddBoolToObject(ac, "loop", anim->loop);
        cJSON_AddItemToObject(ent, "animator", ac);
    }

    /* Skeletal animator. */
    JceSkeletalAnimatorComponent *sk = jce_scene_get_skeletal_animator(s, e);
    if (sk) {
        cJSON *sc = cJSON_CreateObject();
        add_str_ne(sc, "skeleton_path", sk->skeleton_path);
        cJSON_AddNumberToObject(sc, "speed",       (double)sk->speed);
        cJSON_AddBoolToObject(sc, "loop", sk->loop);
        cJSON_AddNumberToObject(sc, "active_clip", sk->active_clip);
        if (sk->clip_count > 0) {
            cJSON *clips = cJSON_CreateArray();
            for (int ci = 0; ci < sk->clip_count && ci < 8; ci++)
                cJSON_AddItemToArray(clips,
                    cJSON_CreateString(sk->clip_names[ci]));
            cJSON_AddItemToObject(sc, "clip_names", clips);
        }
        cJSON_AddItemToObject(ent, "skeletal_animator", sc);
    }

    /* Constraint. */
    JceConstraintComponent *con = jce_scene_get_constraint(s, e);
    if (con) {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddNumberToObject(cc, "constraint_type", con->constraint_type);
        cJSON_AddNumberToObject(cc, "target_entity",   con->target_entity);
        cJSON_AddItemToObject(cc, "pivot_a", float3_to_json(con->pivot_a));
        cJSON_AddItemToObject(cc, "pivot_b", float3_to_json(con->pivot_b));
        cJSON_AddItemToObject(cc, "axis",    float3_to_json(con->axis));
        cJSON_AddNumberToObject(cc, "lower_limit", (double)con->lower_limit);
        cJSON_AddNumberToObject(cc, "upper_limit", (double)con->upper_limit);
        cJSON_AddBoolToObject(cc, "disable_collision", con->disable_collision);
        cJSON_AddItemToObject(ent, "constraint", cc);
    }

    /* Rigidbody. */
    JceRigidBodyComponent *rb = jce_scene_get_rigidbody(s, e);
    if (rb) {
        cJSON *rc = cJSON_CreateObject();
        cJSON_AddNumberToObject(rc, "body_type",     rb->body_type);
        cJSON_AddNumberToObject(rc, "shape_type",    rb->shape_type);
        cJSON_AddNumberToObject(rc, "mass",          (double)rb->mass);
        cJSON_AddNumberToObject(rc, "friction",      (double)rb->friction);
        cJSON_AddNumberToObject(rc, "restitution",   (double)rb->restitution);
        cJSON_AddNumberToObject(rc, "drag",          (double)rb->drag);
        cJSON_AddNumberToObject(rc, "angular_drag",  (double)rb->angular_drag);
        cJSON_AddBoolToObject(rc, "use_gravity",     rb->use_gravity);
        cJSON_AddBoolToObject(rc, "is_kinematic",    rb->is_kinematic);
        cJSON_AddItemToObject(ent, "rigidbody", rc);
    }

    /* Rigidbody 2D. */
    JceRigidBody2DComponent *rb2 = jce_scene_get_rigidbody2d(s, e);
    if (rb2) {
        cJSON *rc = cJSON_CreateObject();
        cJSON_AddNumberToObject(rc, "body_type",    rb2->body_type);
        cJSON_AddNumberToObject(rc, "shape_type",   rb2->shape_type);
        cJSON_AddNumberToObject(rc, "mass",         (double)rb2->mass);
        cJSON_AddNumberToObject(rc, "friction",     (double)rb2->friction);
        cJSON_AddNumberToObject(rc, "restitution",  (double)rb2->restitution);
        cJSON_AddBoolToObject(rc, "fixed_rotation", rb2->fixed_rotation);
        cJSON_AddItemToObject(ent, "rigidbody_2d", rc);
    }

    /* Box collider. */
    JceBoxColliderComponent *bc = jce_scene_get_box_collider(s, e);
    if (bc) {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddItemToObject(cc, "center", float3_to_json(bc->center));
        cJSON_AddItemToObject(cc, "size",   float3_to_json(bc->size));
        cJSON_AddBoolToObject(cc, "is_trigger", bc->is_trigger);
        cJSON_AddItemToObject(ent, "box_collider", cc);
    }

    /* Sphere collider. */
    JceSphereColliderComponent *spc = jce_scene_get_sphere_collider(s, e);
    if (spc) {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddItemToObject(cc, "center", float3_to_json(spc->center));
        cJSON_AddNumberToObject(cc, "radius",     (double)spc->radius);
        cJSON_AddBoolToObject(cc, "is_trigger", spc->is_trigger);
        cJSON_AddItemToObject(ent, "sphere_collider", cc);
    }

    /* Character controller. */
    JceCharacterControllerComponent *cctl =
        jce_scene_get_character_controller(s, e);
    if (cctl) {
        cJSON *cc = cJSON_CreateObject();
        cJSON_AddNumberToObject(cc, "height",      (double)cctl->height);
        cJSON_AddNumberToObject(cc, "radius",      (double)cctl->radius);
        cJSON_AddNumberToObject(cc, "step_offset", (double)cctl->step_offset);
        cJSON_AddNumberToObject(cc, "slope_limit", (double)cctl->slope_limit);
        cJSON_AddItemToObject(ent, "character_controller", cc);
    }

    /* Audio source. */
    JceAudioSourceComponent *as = jce_scene_get_audio_source(s, e);
    if (as) {
        cJSON *ac = cJSON_CreateObject();
        add_str_ne(ac, "clip_path", as->clip_path);
        cJSON_AddNumberToObject(ac, "volume",        (double)as->volume);
        cJSON_AddNumberToObject(ac, "pitch",         (double)as->pitch);
        cJSON_AddNumberToObject(ac, "spatial_blend", (double)as->spatial_blend);
        cJSON_AddBoolToObject(ac, "loop",          as->loop);
        cJSON_AddBoolToObject(ac, "play_on_awake", as->play_on_awake);
        cJSON_AddItemToObject(ent, "audio_source", ac);
    }

    /* Script. */
    JceScriptComponent *scr = jce_scene_get_script(s, e);
    if (scr) {
        cJSON *sc = cJSON_CreateObject();
        add_str_ne(sc, "script_path", scr->script_path);
        cJSON_AddItemToObject(ent, "script", sc);
    }

    /* Particle emitter. */
    JceParticleEmitterComponent *pe = jce_scene_get_particle_emitter(s, e);
    if (pe) {
        cJSON *pc = cJSON_CreateObject();
        cJSON_AddNumberToObject(pc, "emit_rate",    (double)pe->emit_rate);
        cJSON_AddNumberToObject(pc, "lifetime_min", (double)pe->lifetime_min);
        cJSON_AddNumberToObject(pc, "lifetime_max", (double)pe->lifetime_max);
        cJSON_AddItemToObject(ent, "particle_emitter", pc);
    }

    /* Behavior tree. */
    JceBehaviorTree *bt = jce_scene_get_behavior_tree(s, e);
    if (bt) {
        cJSON *bc = cJSON_CreateObject();
        cJSON_AddNumberToObject(bc, "tree_handle", bt->tree_handle_idx);
        cJSON_AddBoolToObject(bc, "active", bt->active);
        cJSON_AddItemToObject(ent, "behavior_tree", bc);
    }

    /* Editor metadata. */
    JceEditorMeta *em = jce_scene_get_editor_meta(s, e);
    if (em) {
        cJSON *ec = cJSON_CreateObject();
        add_str_ne(ec, "tag", em->tag);
        cJSON_AddNumberToObject(ec, "tag_color",       em->tag_color);
        cJSON_AddBoolToObject(ec, "enabled",           em->enabled);
        cJSON_AddBoolToObject(ec, "prefab_instance",   em->prefab_instance);
        add_str_ne(ec, "prefab_path", em->prefab_path);
        cJSON_AddItemToObject(ent, "editor_meta", ec);
    }

    cJSON_AddItemToArray(ctx->entities_array, ent);
}

/* ── Save ──────────────────────────────────────────────────────────── */

char *jce_scene_serial_save(const JceScene *scene, size_t *out_len)
{
    if (!scene) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *contract = cJSON_CreateObject();
    cJSON *scene_obj = cJSON_CreateObject();
    cJSON *entities = cJSON_CreateArray();
    if (!root || !contract || !scene_obj || !entities) {
        cJSON_Delete(root);
        cJSON_Delete(contract);
        cJSON_Delete(scene_obj);
        cJSON_Delete(entities);
        return NULL;
    }

    cJSON_AddItemToObject(root, JCE_SCENE_CONTRACT_KEY, contract);
    cJSON_AddStringToObject(contract, JCE_SCENE_CONTRACT_NAME_KEY,
                            JCE_SCENE_CONTRACT_NAME);
    cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
                            JCE_SCENE_CONTRACT_MINOR);

    cJSON_AddItemToObject(root, JCE_SCENE_ROOT_KEY, scene_obj);
    cJSON_AddNumberToObject(scene_obj, JCE_SCENE_VERSION_KEY,
                            JCE_SCENE_CONTRACT_MAJOR);
    cJSON_AddItemToObject(scene_obj, JCE_SCENE_ENTITIES_KEY, entities);

    SaveCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scene          = (JceScene *)scene;
    ctx.entities_array = entities;
    ctx.next_id        = 1;
    ctx.map_count      = 0;
    ctx.map_cap        = 256;
    ctx.map = (SaveMapEntry *)JCE_MALLOC(
        (size_t)ctx.map_cap * sizeof(SaveMapEntry));
    if (!ctx.map) { cJSON_Delete(root); return NULL; }

    jce_scene_each_entity((JceScene *)scene, save_entity_cb, &ctx);

    JCE_FREE(ctx.map);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (out_len && json)
        *out_len = strlen(json);

    return json;
}

bool jce_scene_serial_save_file(const JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    size_t len = 0;
    char *json = jce_scene_serial_save(scene, &len);
    if (!json) return false;

    /* Use PhysFS for cross-platform I/O.
       PhysFS write requires a write directory; if none has been configured
       we extract the directory part of `path` and set it temporarily.
       The filename (leaf) is used as the PhysFS virtual path. */

    const char *sep = strrchr(path, '/');
    if (!sep) sep = strrchr(path, '\\');

    char dir_buf[1024];
    const char *filename = path;

    if (sep) {
        size_t dir_len = (size_t)(sep - path);
        if (dir_len >= sizeof(dir_buf)) dir_len = sizeof(dir_buf) - 1;
        memcpy(dir_buf, path, dir_len);
        dir_buf[dir_len] = '\0';
        filename = sep + 1;
    } else {
        dir_buf[0] = '.';
        dir_buf[1] = '\0';
    }

    /* Save and restore the previous write dir (may be unset). */
    const char *prev_write_dir = PHYSFS_getWriteDir();
    char prev_buf[1024] = {0};
    if (prev_write_dir) {
        size_t plen = strlen(prev_write_dir);
        if (plen < sizeof(prev_buf))
            memcpy(prev_buf, prev_write_dir, plen + 1);
    }

    if (!PHYSFS_isInit()) {
        /* Fall back to fopen if PhysFS is not available. */
        goto fallback_fopen;
    }

    if (!PHYSFS_setWriteDir(dir_buf)) {
        LOG_WARN(LOG_TAG, "PHYSFS_setWriteDir('%s') failed, falling back to fopen: %s",
                 dir_buf, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        goto fallback_fopen;
    }

    {
        PHYSFS_File *fh = PHYSFS_openWrite(filename);
        if (!fh) {
            LOG_WARN(LOG_TAG, "PHYSFS_openWrite('%s') failed, falling back to fopen: %s",
                     filename, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            /* Restore previous write dir. */
            PHYSFS_setWriteDir(prev_buf[0] ? prev_buf : NULL);
            goto fallback_fopen;
        }

        PHYSFS_sint64 written = PHYSFS_writeBytes(fh, json, (PHYSFS_uint64)len);
        PHYSFS_close(fh);

        /* Restore previous write dir. */
        PHYSFS_setWriteDir(prev_buf[0] ? prev_buf : NULL);

        cJSON_free(json);

        if ((size_t)written != len) {
            LOG_ERROR(LOG_TAG, "write error '%s' (physfs)", path);
            return false;
        }

        LOG_SUCCESS(LOG_TAG, "scene saved to '%s' (%zu bytes, physfs)", path, len);
        return true;
    }

fallback_fopen:
    /* Portable fallback using SDL I/O. */
    {
        SDL_IOStream *io = SDL_IOFromFile(path, "wb");
        if (!io) {
            cJSON_free(json);
            LOG_ERROR(LOG_TAG, "cannot open '%s' for writing", path);
            return false;
        }

        size_t written = SDL_WriteIO(io, json, len);
        SDL_CloseIO(io);
        cJSON_free(json);

        if (written != len) {
            LOG_ERROR(LOG_TAG, "write error '%s'", path);
            return false;
        }

        LOG_SUCCESS(LOG_TAG, "scene saved to '%s' (%zu bytes)", path, len);
        return true;
    }
}

/* ── Load ──────────────────────────────────────────────────────────── */

/* ── Per-component loaders (flat-key format) ──────────────────────── */

static void load_transform(JceScene *scene, JceEntity e, const cJSON *tc)
{
    JceTransform t;
    memset(&t, 0, sizeof(t));
    t.position = json_to_vec3(cJSON_GetObjectItem(tc, "position"));
    t.rotation = json_to_quat(cJSON_GetObjectItem(tc, "rotation"));
    t.scale    = json_to_vec3(cJSON_GetObjectItem(tc, "scale"));
    /* Legacy editor scalar keys: posX/posY/posZ, rotX/rotY/rotZ, scaleX/scaleY/scaleZ */
    if (!cJSON_GetObjectItem(tc, "position")) {
        t.position.x = (float)json_get_num2(tc, "posX", "pos_x", 0);
        t.position.y = (float)json_get_num2(tc, "posY", "pos_y", 0);
        t.position.z = (float)json_get_num2(tc, "posZ", "pos_z", 0);
    }
    if (!cJSON_GetObjectItem(tc, "rotation")) {
        /* If rotW present → quaternion, otherwise euler degrees. */
        float rx = (float)json_get_num2(tc, "rotX", "rot_x", 0);
        float ry = (float)json_get_num2(tc, "rotY", "rot_y", 0);
        float rz = (float)json_get_num2(tc, "rotZ", "rot_z", 0);
        const cJSON *rw = cJSON_GetObjectItem(tc, "rotW");
        if (!rw) rw = cJSON_GetObjectItem(tc, "rot_w");
        if (rw && cJSON_IsNumber(rw)) {
            t.rotation.x = rx;
            t.rotation.y = ry;
            t.rotation.z = rz;
            t.rotation.w = (float)rw->valuedouble;
        } else {
            /* Euler degrees → quaternion. */
            t.rotation = jce_euler_to_q(rx, ry, rz);
        }
    }
    if (!cJSON_GetObjectItem(tc, "scale")) {
        t.scale.x = (float)json_get_num2(tc, "scaleX", "scale_x", 1);
        t.scale.y = (float)json_get_num2(tc, "scaleY", "scale_y", 1);
        t.scale.z = (float)json_get_num2(tc, "scaleZ", "scale_z", 1);
    }
    jce_scene_set_transform(scene, e, &t);
}

static void load_mesh_renderer(JceScene *scene, JceEntity e, const cJSON *mc)
{
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof(mr));
    mr.model.idx  = UINT16_MAX;
    mr.shader.idx = UINT16_MAX;
    mr.visible = json_get_bool(mc, "visible", true);
    /* Paths. */
    const char *mp = json_get_str2(mc, "mesh_path", "meshPath");
    const char *matp = json_get_str2(mc, "material_path", "materialPath");
    if (mp)   snprintf(mr.mesh_path, sizeof(mr.mesh_path), "%s", mp);
    if (matp) snprintf(mr.material_path, sizeof(mr.material_path), "%s", matp);
    mr.mesh_shape = (int)json_get_number(mc, "mesh_shape",
        json_get_number(mc, "meshShape", 0));
    /* PBR material. */
    const cJSON *bc_arr = cJSON_GetObjectItem(mc, "base_color");
    if (bc_arr) {
        json_to_float4(bc_arr, mr.base_color, 1, 1, 1, 1);
    } else {
        mr.base_color[0] = (float)json_get_number(mc, "baseColorR", 1);
        mr.base_color[1] = (float)json_get_number(mc, "baseColorG", 1);
        mr.base_color[2] = (float)json_get_number(mc, "baseColorB", 1);
        mr.base_color[3] = (float)json_get_number(mc, "baseColorA", 1);
    }
    mr.metallic     = (float)json_get_number(mc, "metallic",     0);
    mr.roughness    = (float)json_get_number(mc, "roughness",    1);
    const cJSON *em_arr = cJSON_GetObjectItem(mc, "emissive");
    if (em_arr) {
        json_to_float3(em_arr, mr.emissive, 0, 0, 0);
    } else {
        mr.emissive[0] = (float)json_get_number(mc, "emissiveR", 0);
        mr.emissive[1] = (float)json_get_number(mc, "emissiveG", 0);
        mr.emissive[2] = (float)json_get_number(mc, "emissiveB", 0);
    }
    mr.normal_scale = (float)json_get_number(mc, "normal_scale",
        json_get_number(mc, "normalScale", 1));
    mr.ao_strength  = (float)json_get_number(mc, "ao_strength",
        json_get_number(mc, "aoStrength", 1));
    mr.alpha_mode   = (int)json_get_number(mc, "alpha_mode",
        json_get_number(mc, "alphaMode", 0));
    mr.alpha_cutoff = (float)json_get_number(mc, "alpha_cutoff",
        json_get_number(mc, "alphaCutoff", 0.5));
    mr.double_sided = json_get_bool2(mc, "double_sided", "doubleSided", false);
    /* Texture paths. */
    const char *s;
    s = json_get_str2(mc, "albedo_tex", "albedoTex");
    if (s) snprintf(mr.albedo_tex, sizeof(mr.albedo_tex), "%s", s);
    s = json_get_str2(mc, "mr_tex", "mrTex");
    if (s) snprintf(mr.mr_tex, sizeof(mr.mr_tex), "%s", s);
    s = json_get_str2(mc, "normal_tex", "normalTex");
    if (s) snprintf(mr.normal_tex, sizeof(mr.normal_tex), "%s", s);
    s = json_get_str2(mc, "ao_tex", "aoTex");
    if (s) snprintf(mr.ao_tex, sizeof(mr.ao_tex), "%s", s);
    s = json_get_str2(mc, "emissive_tex", "emissiveTex");
    if (s) snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", s);
    /* Legacy handle indices (for old engine format files). */
    if (cJSON_GetObjectItem(mc, "model"))
        mr.model.idx = (uint16_t)json_get_number(mc, "model", UINT16_MAX);
    if (cJSON_GetObjectItem(mc, "shader"))
        mr.shader.idx = (uint16_t)json_get_number(mc, "shader", UINT16_MAX);
    jce_scene_set_mesh_renderer(scene, e, &mr);
}

static void load_camera(JceScene *scene, JceEntity e, const cJSON *cc)
{
    JceCameraComponent cam;
    memset(&cam, 0, sizeof(cam));
    cam.fov_deg    = (float)json_get_num2(cc, "fov_deg", "fov", 60);
    cam.near_plane = (float)json_get_num2(cc, "near", "near_clip",
        json_get_num2(cc, "nearClip", "near_clip", 0.1));
    cam.far_plane  = (float)json_get_num2(cc, "far", "far_clip",
        json_get_num2(cc, "farClip", "far_clip", 1000));
    cam.is_primary = json_get_bool(cc, "primary", false);
    cam.ortho      = json_get_bool2(cc, "ortho", "orthographic", false);
    jce_scene_set_camera(scene, e, &cam);
}

static void load_dir_light(JceScene *scene, JceEntity e, const cJSON *lc)
{
    JceDirectionalLight dl;
    memset(&dl, 0, sizeof(dl));
    dl.direction   = json_to_vec3(cJSON_GetObjectItem(lc, "direction"));
    dl.color       = json_to_vec3(cJSON_GetObjectItem(lc, "color"));
    dl.intensity   = (float)json_get_number(lc, "intensity", 1);
    dl.casts_shadow = json_get_bool2(lc, "casts_shadow", "castsShadow", false);
    jce_scene_set_dir_light(scene, e, &dl);
}

static void load_point_light(JceScene *scene, JceEntity e, const cJSON *lc)
{
    JcePointLight pl;
    memset(&pl, 0, sizeof(pl));
    pl.color     = json_to_vec3(cJSON_GetObjectItem(lc, "color"));
    pl.intensity = (float)json_get_number(lc, "intensity", 1);
    pl.radius    = (float)json_get_number(lc, "radius", 10);
    jce_scene_set_point_light(scene, e, &pl);
}

static void load_spot_light(JceScene *scene, JceEntity e, const cJSON *lc)
{
    JceSpotLight sl;
    memset(&sl, 0, sizeof(sl));
    sl.direction = json_to_vec3(cJSON_GetObjectItem(lc, "direction"));
    sl.color     = json_to_vec3(cJSON_GetObjectItem(lc, "color"));
    sl.intensity = (float)json_get_number(lc, "intensity", 1);
    sl.radius    = (float)json_get_number(lc, "radius", 10);
    float inner_deg = (float)json_get_num2(lc, "inner_cone_deg", "innerConeDeg", 25);
    float outer_deg = (float)json_get_num2(lc, "outer_cone_deg", "outerConeDeg", 35);
    sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
    sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
    jce_scene_set_spot_light(scene, e, &sl);
}

static void load_skybox(JceScene *scene, JceEntity e, const cJSON *sc)
{
    JceSkyboxComponent sky;
    memset(&sky, 0, sizeof(sky));
    const char *hp = json_get_str2(sc, "hdr_path", "hdrPath");
    if (hp) snprintf(sky.hdr_path, sizeof(sky.hdr_path), "%s", hp);
    sky.rotation  = (float)json_get_number(sc, "rotation", 0);
    sky.exposure  = (float)json_get_number(sc, "exposure", 1);
    sky.use_as_ibl = json_get_bool2(sc, "use_as_ibl", "useAsIbl", false);
    jce_scene_set_skybox(scene, e, &sky);
}

static void load_sprite_renderer(JceScene *scene, JceEntity e, const cJSON *sc)
{
    JceSpriteRendererComponent sr;
    memset(&sr, 0, sizeof(sr));
    const char *sp = json_get_str2(sc, "sprite_path", "spritePath");
    if (sp) snprintf(sr.sprite_path, sizeof(sr.sprite_path), "%s", sp);
    const cJSON *ca = cJSON_GetObjectItem(sc, "color");
    if (ca) {
        json_to_float4(ca, sr.color, 1, 1, 1, 1);
    } else {
        sr.color[0] = (float)json_get_number(sc, "colorR", 1);
        sr.color[1] = (float)json_get_number(sc, "colorG", 1);
        sr.color[2] = (float)json_get_number(sc, "colorB", 1);
        sr.color[3] = (float)json_get_number(sc, "colorA", 1);
    }
    sr.flip_x = json_get_bool(sc, "flipX", false);
    sr.flip_y = json_get_bool(sc, "flipY", false);
    sr.sorting_order = (int)json_get_number(sc, "sortingOrder",
        json_get_number(sc, "sorting_order", 0));
    jce_scene_set_sprite_renderer(scene, e, &sr);
}

static void load_sprite_animator(JceScene *scene, JceEntity e, const cJSON *sc)
{
    JceSpriteAnimatorComponent sa;
    memset(&sa, 0, sizeof(sa));
    const char *v;
    v = json_get_str2(sc, "sheet_path", "sheetPath");
    if (v) snprintf(sa.sheet_path, sizeof(sa.sheet_path), "%s", v);
    v = json_get_str2(sc, "atlas_path", "atlasPath");
    if (v) snprintf(sa.atlas_path, sizeof(sa.atlas_path), "%s", v);
    sa.frame_width  = (int)json_get_num2(sc, "frame_width", "frameWidth", 64);
    sa.frame_height = (int)json_get_num2(sc, "frame_height", "frameHeight", 64);
    v = json_get_str2(sc, "current_anim", "currentAnim");
    if (v) snprintf(sa.current_anim, sizeof(sa.current_anim), "%s", v);
    sa.speed   = (float)json_get_number(sc, "speed", 1);
    sa.loop    = json_get_bool(sc, "loop", false);
    sa.playing = false;
    jce_scene_set_sprite_animator(scene, e, &sa);
}

static void load_animator(JceScene *scene, JceEntity e, const cJSON *ac)
{
    JceAnimatorComponent anim;
    memset(&anim, 0, sizeof(anim));
    const char *cn = json_get_str2(ac, "clip_name", "clipName");
    if (cn) snprintf(anim.clip_name, sizeof(anim.clip_name), "%s", cn);
    anim.speed   = (float)json_get_number(ac, "speed", 1);
    anim.loop    = json_get_bool(ac, "loop", false);
    anim.playing = false;
    jce_scene_set_animator(scene, e, &anim);
}

static void load_skeletal_animator(JceScene *scene, JceEntity e,
                                   const cJSON *sc)
{
    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof(sk));
    const char *sp = json_get_str2(sc, "skeleton_path", "skeletonPath");
    if (sp) snprintf(sk.skeleton_path, sizeof(sk.skeleton_path), "%s", sp);
    sk.speed       = (float)json_get_number(sc, "speed", 1);
    sk.loop        = json_get_bool(sc, "loop", false);
    sk.active_clip = (int)json_get_number(sc, "active_clip",
        json_get_number(sc, "activeClip", 0));
    sk.playing     = false;
    const cJSON *clips = cJSON_GetObjectItem(sc, "clip_names");
    if (!clips) clips = cJSON_GetObjectItem(sc, "clipNames");
    if (cJSON_IsArray(clips)) {
        int n = cJSON_GetArraySize(clips);
        if (n > 8) n = 8;
        for (int ci = 0; ci < n; ci++) {
            const cJSON *ce = cJSON_GetArrayItem(clips, ci);
            if (cJSON_IsString(ce) && ce->valuestring)
                snprintf(sk.clip_names[ci], 64, "%s", ce->valuestring);
        }
        sk.clip_count = n;
    }
    jce_scene_set_skeletal_animator(scene, e, &sk);
}

static void load_constraint(JceScene *scene, JceEntity e, const cJSON *cc)
{
    JceConstraintComponent con;
    memset(&con, 0, sizeof(con));
    con.constraint_type = (int)json_get_num2(cc, "constraint_type",
        "constraintType", 0);
    con.target_entity = (uint32_t)json_get_num2(cc, "target_entity",
        "targetEntity", 0);
    const cJSON *pa = cJSON_GetObjectItem(cc, "pivot_a");
    if (pa) {
        json_to_float3(pa, con.pivot_a, 0, 0, 0);
    } else {
        con.pivot_a[0] = (float)json_get_number(cc, "pivotAx", 0);
        con.pivot_a[1] = (float)json_get_number(cc, "pivotAy", 0);
        con.pivot_a[2] = (float)json_get_number(cc, "pivotAz", 0);
    }
    const cJSON *pb = cJSON_GetObjectItem(cc, "pivot_b");
    if (pb) {
        json_to_float3(pb, con.pivot_b, 0, 0, 0);
    } else {
        con.pivot_b[0] = (float)json_get_number(cc, "pivotBx", 0);
        con.pivot_b[1] = (float)json_get_number(cc, "pivotBy", 0);
        con.pivot_b[2] = (float)json_get_number(cc, "pivotBz", 0);
    }
    const cJSON *ax = cJSON_GetObjectItem(cc, "axis");
    if (ax) {
        json_to_float3(ax, con.axis, 0, 1, 0);
    } else {
        con.axis[0] = (float)json_get_number(cc, "axisX", 0);
        con.axis[1] = (float)json_get_number(cc, "axisY", 1);
        con.axis[2] = (float)json_get_number(cc, "axisZ", 0);
    }
    con.lower_limit = (float)json_get_number(cc, "lower_limit",
        json_get_number(cc, "lowerLimit", 0));
    con.upper_limit = (float)json_get_number(cc, "upper_limit",
        json_get_number(cc, "upperLimit", 0));
    con.disable_collision = json_get_bool2(cc, "disable_collision",
        "disableCollision", false);
    jce_scene_set_constraint(scene, e, &con);
}

static void load_rigidbody(JceScene *scene, JceEntity e, const cJSON *rc)
{
    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof(rb));
    rb.body_type     = (uint8_t)json_get_number(rc, "body_type", 0);
    rb.shape_type    = (uint8_t)json_get_number(rc, "shape_type", 0);
    rb.mass          = (float)json_get_number(rc, "mass", 1);
    rb.friction      = (float)json_get_number(rc, "friction", 0.5);
    rb.restitution   = (float)json_get_number(rc, "restitution", 0);
    rb.drag          = (float)json_get_num2(rc, "drag", "linearDamping", 0);
    rb.angular_drag  = (float)json_get_num2(rc, "angular_drag", "angularDrag", 0);
    rb.use_gravity   = json_get_bool2(rc, "use_gravity", "useGravity", true);
    rb.is_kinematic  = json_get_bool2(rc, "is_kinematic", "isKinematic", false);
    jce_scene_set_rigidbody(scene, e, &rb);
}

static void load_rigidbody_2d(JceScene *scene, JceEntity e, const cJSON *rc)
{
    JceRigidBody2DComponent rb2;
    memset(&rb2, 0, sizeof(rb2));
    rb2.body_type      = (uint8_t)json_get_number(rc, "body_type", 0);
    rb2.shape_type     = (uint8_t)json_get_number(rc, "shape_type", 0);
    rb2.mass           = (float)json_get_number(rc, "mass", 1);
    rb2.friction       = (float)json_get_number(rc, "friction", 0.5);
    rb2.restitution    = (float)json_get_number(rc, "restitution", 0);
    rb2.fixed_rotation = json_get_bool2(rc, "fixed_rotation",
        "fixedRotation", false);
    jce_scene_set_rigidbody2d(scene, e, &rb2);
}

static void load_box_collider(JceScene *scene, JceEntity e, const cJSON *cc)
{
    JceBoxColliderComponent bc;
    memset(&bc, 0, sizeof(bc));
    const cJSON *ca = cJSON_GetObjectItem(cc, "center");
    if (ca) {
        json_to_float3(ca, bc.center, 0, 0, 0);
    } else {
        bc.center[0] = (float)json_get_number(cc, "centerX", 0);
        bc.center[1] = (float)json_get_number(cc, "centerY", 0);
        bc.center[2] = (float)json_get_number(cc, "centerZ", 0);
    }
    const cJSON *sa = cJSON_GetObjectItem(cc, "size");
    if (sa) {
        json_to_float3(sa, bc.size, 1, 1, 1);
    } else {
        bc.size[0] = (float)json_get_number(cc, "sizeX", 1);
        bc.size[1] = (float)json_get_number(cc, "sizeY", 1);
        bc.size[2] = (float)json_get_number(cc, "sizeZ", 1);
    }
    bc.is_trigger = json_get_bool2(cc, "is_trigger", "isTrigger", false);
    jce_scene_set_box_collider(scene, e, &bc);
}

static void load_sphere_collider(JceScene *scene, JceEntity e, const cJSON *cc)
{
    JceSphereColliderComponent sc;
    memset(&sc, 0, sizeof(sc));
    const cJSON *ca = cJSON_GetObjectItem(cc, "center");
    if (ca) {
        json_to_float3(ca, sc.center, 0, 0, 0);
    } else {
        sc.center[0] = (float)json_get_number(cc, "centerX", 0);
        sc.center[1] = (float)json_get_number(cc, "centerY", 0);
        sc.center[2] = (float)json_get_number(cc, "centerZ", 0);
    }
    sc.radius     = (float)json_get_number(cc, "radius", 0.5);
    sc.is_trigger = json_get_bool2(cc, "is_trigger", "isTrigger", false);
    jce_scene_set_sphere_collider(scene, e, &sc);
}

static void load_character_controller(JceScene *scene, JceEntity e,
                                      const cJSON *cc)
{
    JceCharacterControllerComponent cctl;
    memset(&cctl, 0, sizeof(cctl));
    cctl.height      = (float)json_get_num2(cc, "height", "height", 2);
    cctl.radius      = (float)json_get_number(cc, "radius", 0.5);
    cctl.step_offset = (float)json_get_num2(cc, "step_offset", "stepOffset", 0.3);
    cctl.slope_limit = (float)json_get_num2(cc, "slope_limit", "slopeLimit", 45);
    jce_scene_set_character_controller(scene, e, &cctl);
}

static void load_audio_source(JceScene *scene, JceEntity e, const cJSON *ac)
{
    JceAudioSourceComponent as;
    memset(&as, 0, sizeof(as));
    const char *cp = json_get_str2(ac, "clip_path", "clipPath");
    if (cp) snprintf(as.clip_path, sizeof(as.clip_path), "%s", cp);
    as.volume        = (float)json_get_number(ac, "volume", 1);
    as.pitch         = (float)json_get_number(ac, "pitch", 1);
    as.spatial_blend  = (float)json_get_num2(ac, "spatial_blend",
        "spatialBlend", 0);
    as.loop          = json_get_bool(ac, "loop", false);
    as.play_on_awake = json_get_bool2(ac, "play_on_awake", "playOnAwake", true);
    jce_scene_set_audio_source(scene, e, &as);
}

static void load_script(JceScene *scene, JceEntity e, const cJSON *sc)
{
    JceScriptComponent scr;
    memset(&scr, 0, sizeof(scr));
    const char *sp = json_get_str2(sc, "script_path", "scriptPath");
    if (sp) snprintf(scr.script_path, sizeof(scr.script_path), "%s", sp);
    jce_scene_set_script(scene, e, &scr);
}

static void load_particle_emitter(JceScene *scene, JceEntity e,
                                  const cJSON *pc)
{
    JceParticleEmitterComponent pe;
    memset(&pe, 0, sizeof(pe));
    pe.emit_rate    = (float)json_get_num2(pc, "emit_rate", "emitRate", 10);
    pe.lifetime_min = (float)json_get_num2(pc, "lifetime_min",
        "lifetimeMin", 0.5);
    pe.lifetime_max = (float)json_get_num2(pc, "lifetime_max",
        "lifetimeMax", 2);
    jce_scene_set_particle_emitter(scene, e, &pe);
}

static void load_behavior_tree(JceScene *scene, JceEntity e, const cJSON *bc)
{
    JceBehaviorTree bt;
    memset(&bt, 0, sizeof(bt));
    bt.tree_handle_idx    = (uint32_t)json_get_num2(bc, "tree_handle",
        "treeHandle", 0);
    bt.context_handle_idx = 0;
    bt.active = json_get_bool(bc, "active", true);
    jce_scene_set_behavior_tree(scene, e, &bt);
}

static void load_editor_meta(JceScene *scene, JceEntity e, const cJSON *ec)
{
    JceEditorMeta em;
    memset(&em, 0, sizeof(em));
    const char *em_name = json_get_str(ec, "name");
    if (em_name) snprintf(em.name, sizeof(em.name), "%s", em_name);
    const char *tg = json_get_str(ec, "tag");
    if (tg) snprintf(em.tag, sizeof(em.tag), "%s", tg);
    em.tag_color = (uint8_t)json_get_num2(ec, "tag_color", "tagColor", 0);
    em.enabled = json_get_bool(ec, "enabled", true);
    em.prefab_instance = json_get_bool2(ec, "prefab_instance",
        "prefabInstance", false);
    const char *pp = json_get_str2(ec, "prefab_path", "prefabPath");
    if (pp) snprintf(em.prefab_path, sizeof(em.prefab_path), "%s", pp);
    jce_scene_set_editor_meta(scene, e, &em);
}

/* ── Legacy editor format: "components" array with "type" field ───── */

static void load_legacy_light(JceScene *scene, JceEntity e, const cJSON *props)
{
    /* Editor unifies dir/point/spot into one "Light" type. */
    const cJSON *lt = cJSON_GetObjectItem(props, "lightType");
    if (!lt) lt = cJSON_GetObjectItem(props, "type");
    /* If "type" is a string matching "Light", skip it and check lightType. */
    int light_type = 0;
    if (cJSON_IsNumber(lt))
        light_type = lt->valueint;
    else if (cJSON_IsString(lt) && lt->valuestring) {
        if (strcmp(lt->valuestring, "point") == 0 ||
            strcmp(lt->valuestring, "Point") == 0)
            light_type = 1;
        else if (strcmp(lt->valuestring, "spot") == 0 ||
                 strcmp(lt->valuestring, "Spot") == 0)
            light_type = 2;
    }

    /* Parse common fields. */
    jce_vec3 color;
    color.x = (float)json_get_num2(props, "colorR", "color_r", 1);
    color.y = (float)json_get_num2(props, "colorG", "color_g", 1);
    color.z = (float)json_get_num2(props, "colorB", "color_b", 1);
    float intensity = (float)json_get_number(props, "intensity", 1);
    bool shadow = json_get_bool2(props, "casts_shadow", "castsShadow", false);

    if (light_type == 0) {
        JceDirectionalLight dl;
        memset(&dl, 0, sizeof(dl));
        dl.color = color;
        dl.intensity = intensity;
        dl.casts_shadow = shadow;
        dl.direction = (jce_vec3){{ 0, -1, 0 }};
        jce_scene_set_dir_light(scene, e, &dl);
    } else if (light_type == 1) {
        JcePointLight pl;
        memset(&pl, 0, sizeof(pl));
        pl.color = color;
        pl.intensity = intensity;
        pl.radius = (float)json_get_number(props, "radius", 10);
        jce_scene_set_point_light(scene, e, &pl);
    } else {
        JceSpotLight sl;
        memset(&sl, 0, sizeof(sl));
        sl.color = color;
        sl.intensity = intensity;
        sl.radius = (float)json_get_number(props, "radius", 10);
        sl.direction = (jce_vec3){{ 0, -1, 0 }};
        float inner = (float)json_get_num2(props, "inner_cone_deg",
            "innerConeDeg", 25);
        float outer = (float)json_get_num2(props, "outer_cone_deg",
            "outerConeDeg", 35);
        sl.inner_cone_cos = cosf(inner * JCE_DEG2RAD);
        sl.outer_cone_cos = cosf(outer * JCE_DEG2RAD);
        jce_scene_set_spot_light(scene, e, &sl);
    }
}

static void load_legacy_component(JceScene *scene, JceEntity e,
                                  const char *type_str, const cJSON *props)
{
    if (!type_str || !props) return;
    if (strcmp(type_str, "Transform") == 0 || strcmp(type_str, "transform") == 0)
        load_transform(scene, e, props);
    else if (strcmp(type_str, "MeshRenderer") == 0 ||
             strcmp(type_str, "Mesh Renderer") == 0 ||
             strcmp(type_str, "meshRenderer") == 0 ||
             strcmp(type_str, "mesh_renderer") == 0)
        load_mesh_renderer(scene, e, props);
    else if (strcmp(type_str, "Camera") == 0 || strcmp(type_str, "camera") == 0)
        load_camera(scene, e, props);
    else if (strcmp(type_str, "Light") == 0 || strcmp(type_str, "light") == 0)
        load_legacy_light(scene, e, props);
    else if (strcmp(type_str, "Skybox") == 0 || strcmp(type_str, "skybox") == 0)
        load_skybox(scene, e, props);
    else if (strcmp(type_str, "SpriteRenderer") == 0 ||
             strcmp(type_str, "Sprite Renderer") == 0 ||
             strcmp(type_str, "sprite_renderer") == 0)
        load_sprite_renderer(scene, e, props);
    else if (strcmp(type_str, "SpriteAnimator") == 0 ||
             strcmp(type_str, "Sprite Animator") == 0 ||
             strcmp(type_str, "sprite_animator") == 0)
        load_sprite_animator(scene, e, props);
    else if (strcmp(type_str, "Animator") == 0 ||
             strcmp(type_str, "animator") == 0)
        load_animator(scene, e, props);
    else if (strcmp(type_str, "SkeletalAnimator") == 0 ||
             strcmp(type_str, "Skeletal Animator") == 0 ||
             strcmp(type_str, "skeletal_animator") == 0)
        load_skeletal_animator(scene, e, props);
    else if (strcmp(type_str, "Constraint") == 0 ||
             strcmp(type_str, "constraint") == 0)
        load_constraint(scene, e, props);
    else if (strcmp(type_str, "Rigidbody") == 0 ||
             strcmp(type_str, "rigidbody") == 0)
        load_rigidbody(scene, e, props);
    else if (strcmp(type_str, "BoxCollider") == 0 ||
             strcmp(type_str, "Box Collider") == 0 ||
             strcmp(type_str, "box_collider") == 0)
        load_box_collider(scene, e, props);
    else if (strcmp(type_str, "SphereCollider") == 0 ||
             strcmp(type_str, "Sphere Collider") == 0 ||
             strcmp(type_str, "sphere_collider") == 0)
        load_sphere_collider(scene, e, props);
    else if (strcmp(type_str, "CharacterController") == 0 ||
             strcmp(type_str, "Character Controller") == 0 ||
             strcmp(type_str, "character_controller") == 0)
        load_character_controller(scene, e, props);
    else if (strcmp(type_str, "AudioSource") == 0 ||
             strcmp(type_str, "Audio Source") == 0 ||
             strcmp(type_str, "audio_source") == 0)
        load_audio_source(scene, e, props);
    else if (strcmp(type_str, "Script") == 0 || strcmp(type_str, "script") == 0)
        load_script(scene, e, props);
    else
        LOG_WARN(LOG_TAG, "unknown component type '%s'", type_str);
}

/* ── Load entity flat-key components (engine format) ─────────────── */

static void load_entity_flat(JceScene *scene, JceEntity e, const cJSON *ent)
{
    cJSON *tc = cJSON_GetObjectItem(ent, "transform");
    if (tc) load_transform(scene, e, tc);

    cJSON *mc = cJSON_GetObjectItem(ent, "mesh_renderer");
    if (mc) load_mesh_renderer(scene, e, mc);

    cJSON *cc = cJSON_GetObjectItem(ent, "camera");
    if (cc) load_camera(scene, e, cc);

    cJSON *dl = cJSON_GetObjectItem(ent, "dir_light");
    if (dl) load_dir_light(scene, e, dl);

    cJSON *pl = cJSON_GetObjectItem(ent, "point_light");
    if (pl) load_point_light(scene, e, pl);

    cJSON *sl = cJSON_GetObjectItem(ent, "spot_light");
    if (sl) load_spot_light(scene, e, sl);

    cJSON *sk = cJSON_GetObjectItem(ent, "skybox");
    if (sk) load_skybox(scene, e, sk);

    cJSON *sr = cJSON_GetObjectItem(ent, "sprite_renderer");
    if (sr) load_sprite_renderer(scene, e, sr);

    cJSON *sa = cJSON_GetObjectItem(ent, "sprite_animator");
    if (sa) load_sprite_animator(scene, e, sa);

    cJSON *an = cJSON_GetObjectItem(ent, "animator");
    if (an) load_animator(scene, e, an);

    cJSON *ska = cJSON_GetObjectItem(ent, "skeletal_animator");
    if (ska) load_skeletal_animator(scene, e, ska);

    cJSON *cn = cJSON_GetObjectItem(ent, "constraint");
    if (cn) load_constraint(scene, e, cn);

    cJSON *rb = cJSON_GetObjectItem(ent, "rigidbody");
    if (rb) load_rigidbody(scene, e, rb);

    cJSON *r2 = cJSON_GetObjectItem(ent, "rigidbody_2d");
    if (r2) load_rigidbody_2d(scene, e, r2);

    cJSON *bc = cJSON_GetObjectItem(ent, "box_collider");
    if (bc) load_box_collider(scene, e, bc);

    cJSON *spc = cJSON_GetObjectItem(ent, "sphere_collider");
    if (spc) load_sphere_collider(scene, e, spc);

    cJSON *cctl = cJSON_GetObjectItem(ent, "character_controller");
    if (cctl) load_character_controller(scene, e, cctl);

    cJSON *as = cJSON_GetObjectItem(ent, "audio_source");
    if (as) load_audio_source(scene, e, as);

    cJSON *scr = cJSON_GetObjectItem(ent, "script");
    if (scr) load_script(scene, e, scr);

    cJSON *pe = cJSON_GetObjectItem(ent, "particle_emitter");
    if (pe) load_particle_emitter(scene, e, pe);

    cJSON *bt = cJSON_GetObjectItem(ent, "behavior_tree");
    if (bt) load_behavior_tree(scene, e, bt);

    cJSON *em = cJSON_GetObjectItem(ent, "editor_meta");
    if (em) load_editor_meta(scene, e, em);
}

/* ── Load (two-pass for parent resolution) ─────────────────────────── */

typedef struct {
    JceEntity   entity;
    const char *parent_name;  /* NULL if no parent */
    int         save_id;      /* from JSON "id", or -1 */
    int         parent_id;    /* from JSON "parentId", or -1 */
    const char *name;
} LoadEntry;

bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len)
{
    if (!scene || !json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        LOG_ERROR(LOG_TAG, "JSON parse error");
        return false;
    }

    uint32_t contract_major = JCE_SCENE_CONTRACT_MAJOR;
    uint32_t contract_minor = JCE_SCENE_CONTRACT_MINOR;
    read_contract_version(root, &contract_major, &contract_minor);
    if (!jce_scene_contract_major_compatible(contract_major)) {
        LOG_ERROR(LOG_TAG,
                  "unsupported scene contract major version: %u (expected %u)",
                  (unsigned)contract_major,
                  (unsigned)JCE_SCENE_CONTRACT_MAJOR);
        cJSON_Delete(root);
        return false;
    }
    (void)contract_minor;

    cJSON *container = resolve_scene_container(root);
    cJSON *entities = cJSON_GetObjectItemCaseSensitive(container,
        JCE_SCENE_ENTITIES_KEY);
    if (!entities || !cJSON_IsArray(entities)) {
        LOG_ERROR(LOG_TAG, "missing 'entities' array");
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(entities);
    LoadEntry *loaded = (LoadEntry *)JCE_MALLOC(
        (size_t)count * sizeof(LoadEntry));
    if (!loaded) { cJSON_Delete(root); return false; }
    memset(loaded, 0, (size_t)count * sizeof(LoadEntry));

    /* Pass 1: create entities and load components. */
    for (int i = 0; i < count; i++) {
        cJSON *ent = cJSON_GetArrayItem(entities, i);
        if (!ent) { loaded[i].entity = JCE_ENTITY_INVALID; continue; }

        const cJSON *name_j = cJSON_GetObjectItem(ent, "name");
        const char *name = (name_j && cJSON_IsString(name_j))
                         ? name_j->valuestring : NULL;

        /* Create entity WITHOUT flecs name (avoids duplicate-name aborts). */
        JceEntity e = jce_scene_create_entity(scene, NULL);
        loaded[i].entity = e;
        loaded[i].name   = name;
        if (e == JCE_ENTITY_INVALID) continue;

        /* Read numeric id and parentId for robust resolution. */
        const cJSON *id_j = cJSON_GetObjectItem(ent, "id");
        loaded[i].save_id = (id_j && cJSON_IsNumber(id_j)) ? id_j->valueint : -1;

        const cJSON *pid_j = cJSON_GetObjectItem(ent, "parentId");
        loaded[i].parent_id = (pid_j && cJSON_IsNumber(pid_j)) ? pid_j->valueint : -1;

        /* Store parent name reference for fallback in pass 2. */
        const char *pn = json_get_str(ent, "parent");
        if (!pn) pn = json_get_str(ent, "parent_id");
        loaded[i].parent_name = pn;

        /* Detect format: legacy "components" array or flat keys. */
        cJSON *components_arr = cJSON_GetObjectItem(ent, "components");
        if (components_arr && cJSON_IsArray(components_arr)) {
            /* Legacy editor format. */
            int cn = cJSON_GetArraySize(components_arr);
            for (int ci = 0; ci < cn; ci++) {
                const cJSON *comp = cJSON_GetArrayItem(components_arr, ci);
                if (!cJSON_IsObject(comp)) continue;
                const cJSON *type_j2 = cJSON_GetObjectItem(comp, "type");
                if (!type_j2) type_j2 = cJSON_GetObjectItem(comp, "componentType");
                if (!type_j2) type_j2 = cJSON_GetObjectItem(comp, "class");
                const char *type_str =
                    (type_j2 && cJSON_IsString(type_j2)) ? type_j2->valuestring
                                                         : NULL;
                load_legacy_component(scene, e, type_str, comp);
            }
        } else {
            /* Engine flat-key format. */
            load_entity_flat(scene, e, ent);
        }

        /* Editor metadata from entity-level keys (legacy). */
        if (!cJSON_GetObjectItem(ent, "editor_meta")) {
            const cJSON *tag_c = cJSON_GetObjectItem(ent, "tag_color");
            if (!tag_c) tag_c = cJSON_GetObjectItem(ent, "tagColor");
            const cJSON *en_j = cJSON_GetObjectItem(ent, "enabled");
            if (tag_c || en_j) {
                JceEditorMeta em;
                memset(&em, 0, sizeof(em));
                em.enabled = en_j ? (cJSON_IsTrue(en_j) ? true : false) : true;
                if (tag_c && cJSON_IsNumber(tag_c))
                    em.tag_color = (uint8_t)tag_c->valueint;
                jce_scene_set_editor_meta(scene, e, &em);
            }
        }

        /* Store display name in EditorMeta.name. */
        if (name && name[0]) {
            JceEditorMeta *existing_em = jce_scene_get_editor_meta(scene, e);
            if (existing_em) {
                snprintf(existing_em->name, sizeof(existing_em->name),
                         "%s", name);
            } else {
                JceEditorMeta new_em;
                memset(&new_em, 0, sizeof(new_em));
                new_em.enabled = true;
                snprintf(new_em.name, sizeof(new_em.name), "%s", name);
                jce_scene_set_editor_meta(scene, e, &new_em);
            }
        }
    }

    /* Pass 2: resolve parent relationships (prefer numeric parentId). */
    for (int i = 0; i < count; i++) {
        if (loaded[i].entity == JCE_ENTITY_INVALID) continue;

        /* Try numeric parentId first. */
        if (loaded[i].parent_id >= 0) {
            int found = 0;
            for (int j = 0; j < count; j++) {
                if (j == i || loaded[j].entity == JCE_ENTITY_INVALID) continue;
                if (loaded[j].save_id == loaded[i].parent_id) {
                    jce_scene_set_parent(scene, loaded[i].entity,
                                         loaded[j].entity);
                    found = 1;
                    break;
                }
            }
            if (found) continue;
        }

        /* Fall back to name-based. */
        if (loaded[i].parent_name) {
            for (int j = 0; j < count; j++) {
                if (j == i || loaded[j].entity == JCE_ENTITY_INVALID) continue;
                if (loaded[j].name &&
                    strcmp(loaded[j].name, loaded[i].parent_name) == 0) {
                    jce_scene_set_parent(scene, loaded[i].entity,
                                         loaded[j].entity);
                    break;
                }
            }
        }
    }

    JCE_FREE(loaded);
    cJSON_Delete(root);
    LOG_SUCCESS(LOG_TAG, "scene loaded: %d entities", count);
    return true;
}

bool jce_scene_serial_load_file(JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    /* Try PhysFS first for cross-platform consistency. */
    if (PHYSFS_isInit()) {
        /* Split path into directory + filename to mount temporarily. */
        const char *sep = strrchr(path, '/');
        if (!sep) sep = strrchr(path, '\\');

        char dir_buf[1024];
        const char *filename = path;

        if (sep) {
            size_t dir_len = (size_t)(sep - path);
            if (dir_len >= sizeof(dir_buf)) dir_len = sizeof(dir_buf) - 1;
            memcpy(dir_buf, path, dir_len);
            dir_buf[dir_len] = '\0';
            filename = sep + 1;
        } else {
            dir_buf[0] = '.';
            dir_buf[1] = '\0';
        }

        /* Mount the directory temporarily for reading this file. */
        int mounted = PHYSFS_mount(dir_buf, NULL, 1);
        if (mounted) {
            PHYSFS_File *fh = PHYSFS_openRead(filename);
            if (fh) {
                PHYSFS_sint64 sz = PHYSFS_fileLength(fh);
                if (sz > 0) {
                    char *buf = (char *)JCE_MALLOC((size_t)sz + 1);
                    if (buf) {
                        PHYSFS_sint64 nread = PHYSFS_readBytes(fh, buf, (PHYSFS_uint64)sz);
                        PHYSFS_close(fh);
                        PHYSFS_unmount(dir_buf);

                        if (nread > 0) {
                            buf[nread] = '\0';
                            bool ok = jce_scene_serial_load(scene, buf, (size_t)nread);
                            JCE_FREE(buf);
                            return ok;
                        }
                        JCE_FREE(buf);
                        return false;
                    }
                    PHYSFS_close(fh);
                }
            }
            PHYSFS_unmount(dir_buf);
        }

        LOG_DEBUG(LOG_TAG, "PhysFS load of '%s' failed, using fallback", path);
    }

    /* Fallback to SDL I/O when PhysFS unavailable or path not found. */
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        LOG_ERROR(LOG_TAG, "cannot open '%s' for reading", path);
        return false;
    }

    Sint64 sz = SDL_GetIOSize(io);

    if (sz <= 0) {
        SDL_CloseIO(io);
        return false;
    }

    char *buf = (char *)JCE_MALLOC((size_t)sz + 1);
    if (!buf) { SDL_CloseIO(io); return false; }

    size_t read_bytes = SDL_ReadIO(io, buf, (size_t)sz);
    SDL_CloseIO(io);
    buf[read_bytes] = '\0';

    bool ok = jce_scene_serial_load(scene, buf, read_bytes);
    JCE_FREE(buf);
    return ok;
}

bool jce_scene_serial_load_vfs(JceScene *scene,
                               const JceFileSystem *fs,
                               const char *virtual_path)
{
    if (!scene || !fs || !virtual_path) return false;

    size_t size = 0;
    void *data = jce_fs_read_all(fs, virtual_path, &size);
    if (!data) {
        LOG_ERROR(LOG_TAG, "cannot open '%s' via VFS", virtual_path);
        return false;
    }

    /* Ensure NUL-terminated for JSON parsing. */
    char *buf = (char *)JCE_MALLOC(size + 1);
    if (!buf) { JCE_FREE(data); return false; }
    memcpy(buf, data, size);
    buf[size] = '\0';
    JCE_FREE(data);

    bool ok = jce_scene_serial_load(scene, buf, size);
    JCE_FREE(buf);
    return ok;
}

/* ── Memory ────────────────────────────────────────────────────────── */

void jce_scene_serial_free(char *json)
{
    /* cJSON_PrintUnformatted allocates with cJSON_malloc. */
    if (json) cJSON_free(json);
}
