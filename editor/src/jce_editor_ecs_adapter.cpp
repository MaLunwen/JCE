/*
 * jce_editor_ecs_adapter.cpp  Bridge between editor UI types and engine ECS.
 *
 * See jce_editor_ecs_adapter.h for design rationale.
 */

#include "jce_editor_ecs_adapter.h"
#include "jce_editor_state_internal.h"

#include <string.h>
#include <math.h>

/* ── Transform conversion helpers ─────────────────────────────────── */

static void euler_deg_to_transform(const float pos[3], const float rot_deg[3],
                                   const float scl[3], JceTransform *out)
{
    out->position = jce_v3(pos[0], pos[1], pos[2]);
    out->rotation = jce_euler_to_q(rot_deg[0], rot_deg[1], rot_deg[2]);
    out->scale    = jce_v3(scl[0], scl[1], scl[2]);
}

static void transform_to_euler_deg(const JceTransform *t,
                                   float pos[3], float rot_deg[3], float scl[3])
{
    pos[0] = t->position.x;
    pos[1] = t->position.y;
    pos[2] = t->position.z;

    jce_vec3 euler = jce_q_to_euler(t->rotation);
    rot_deg[0] = euler.x * JCE_RAD2DEG;
    rot_deg[1] = euler.y * JCE_RAD2DEG;
    rot_deg[2] = euler.z * JCE_RAD2DEG;

    scl[0] = t->scale.x;
    scl[1] = t->scale.y;
    scl[2] = t->scale.z;
}

/* ── Per-component: ECS → JceComponentInfo ─────────────────────────── */

static bool read_transform(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceTransform *t = jce_scene_get_transform(sc, e);
    if (!t) return false;
    out->type = JCE_COMP_TRANSFORM;
    transform_to_euler_deg(t, out->data.transform.pos,
                           out->data.transform.rot, out->data.transform.scale);
    return true;
}

static bool read_mesh_renderer(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(sc, e);
    if (!mr) return false;
    out->type = JCE_COMP_MESH_RENDERER;
    auto &d = out->data.mesh_renderer;
    memcpy(d.mesh_path,     mr->mesh_path,     sizeof(d.mesh_path));
    memcpy(d.material_path, mr->material_path, sizeof(d.material_path));
    d.mesh_shape   = mr->mesh_shape;
    memcpy(d.base_color, mr->base_color, sizeof(d.base_color));
    d.metallic     = mr->metallic;
    d.roughness    = mr->roughness;
    memcpy(d.emissive, mr->emissive, sizeof(d.emissive));
    d.normal_scale = mr->normal_scale;
    d.ao_strength  = mr->ao_strength;
    d.alpha_mode   = mr->alpha_mode;
    d.alpha_cutoff = mr->alpha_cutoff;
    d.double_sided = mr->double_sided;
    memcpy(d.albedo_tex,   mr->albedo_tex,   sizeof(d.albedo_tex));
    memcpy(d.mr_tex,       mr->mr_tex,       sizeof(d.mr_tex));
    memcpy(d.normal_tex,   mr->normal_tex,   sizeof(d.normal_tex));
    memcpy(d.ao_tex,       mr->ao_tex,       sizeof(d.ao_tex));
    memcpy(d.emissive_tex, mr->emissive_tex, sizeof(d.emissive_tex));
    return true;
}

static bool read_light(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    /* Check all 3 engine light types → map to unified editor Light. */
    JceDirectionalLight *dl = jce_scene_get_dir_light(sc, e);
    if (dl) {
        out->type = JCE_COMP_LIGHT;
        auto &d = out->data.light;
        d.color[0] = dl->color.x; d.color[1] = dl->color.y;
        d.color[2] = dl->color.z; d.color[3] = 1.0f;
        d.intensity    = dl->intensity;
        d.type         = 0; /* directional */
        d.radius       = 0;
        d.inner_cone_deg = 0;
        d.outer_cone_deg = 0;
        d.casts_shadow = dl->casts_shadow;
        return true;
    }
    JcePointLight *pl = jce_scene_get_point_light(sc, e);
    if (pl) {
        out->type = JCE_COMP_LIGHT;
        auto &d = out->data.light;
        d.color[0] = pl->color.x; d.color[1] = pl->color.y;
        d.color[2] = pl->color.z; d.color[3] = 1.0f;
        d.intensity    = pl->intensity;
        d.type         = 1; /* point */
        d.radius       = pl->radius;
        d.inner_cone_deg = 0;
        d.outer_cone_deg = 0;
        d.casts_shadow = false;
        return true;
    }
    JceSpotLight *sl = jce_scene_get_spot_light(sc, e);
    if (sl) {
        out->type = JCE_COMP_LIGHT;
        auto &d = out->data.light;
        d.color[0] = sl->color.x; d.color[1] = sl->color.y;
        d.color[2] = sl->color.z; d.color[3] = 1.0f;
        d.intensity      = sl->intensity;
        d.type           = 2; /* spot */
        d.radius         = sl->radius;
        d.inner_cone_deg = acosf(sl->inner_cone_cos) * JCE_RAD2DEG;
        d.outer_cone_deg = acosf(sl->outer_cone_cos) * JCE_RAD2DEG;
        d.casts_shadow   = false;
        return true;
    }
    return false;
}

static bool read_camera(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceCameraComponent *c = jce_scene_get_camera(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_CAMERA;
    out->data.camera.fov      = c->fov_deg;
    out->data.camera.near_clip = c->near_plane;
    out->data.camera.far_clip  = c->far_plane;
    out->data.camera.ortho     = c->ortho;
    return true;
}

static bool read_sprite_renderer(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceSpriteRendererComponent *c = jce_scene_get_sprite_renderer(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_SPRITE_RENDERER;
    memcpy(out->data.sprite_renderer.sprite_path, c->sprite_path, 128);
    memcpy(out->data.sprite_renderer.color, c->color, sizeof(c->color));
    out->data.sprite_renderer.flip_x        = c->flip_x;
    out->data.sprite_renderer.flip_y        = c->flip_y;
    out->data.sprite_renderer.sorting_order = c->sorting_order;
    return true;
}

static bool read_animator(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceAnimatorComponent *c = jce_scene_get_animator(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_ANIMATOR;
    memcpy(out->data.animator.clip_name, c->clip_name, 64);
    out->data.animator.speed   = c->speed;
    out->data.animator.loop    = c->loop;
    out->data.animator.playing = c->playing;
    return true;
}

static bool read_skeletal_animator(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceSkeletalAnimatorComponent *c = jce_scene_get_skeletal_animator(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_SKELETAL_ANIMATOR;
    auto &d = out->data.skeletal_animator;
    memcpy(d.skeleton_path, c->skeleton_path, 128);
    memcpy(d.clip_names, c->clip_names, sizeof(d.clip_names));
    d.clip_count  = c->clip_count;
    d.active_clip = c->active_clip;
    d.speed       = c->speed;
    d.loop        = c->loop;
    d.playing     = c->playing;
    return true;
}

static bool read_rigidbody(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceRigidBodyComponent *c = jce_scene_get_rigidbody(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_RIGIDBODY;
    out->data.rigidbody.mass         = c->mass;
    out->data.rigidbody.drag         = c->drag;
    out->data.rigidbody.angular_drag = c->angular_drag;
    out->data.rigidbody.use_gravity  = c->use_gravity;
    out->data.rigidbody.is_kinematic = c->is_kinematic;
    return true;
}

static bool read_box_collider(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceBoxColliderComponent *c = jce_scene_get_box_collider(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_BOX_COLLIDER;
    memcpy(out->data.box_collider.center, c->center, sizeof(c->center));
    memcpy(out->data.box_collider.size, c->size, sizeof(c->size));
    out->data.box_collider.is_trigger = c->is_trigger;
    return true;
}

static bool read_sphere_collider(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceSphereColliderComponent *c = jce_scene_get_sphere_collider(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_SPHERE_COLLIDER;
    memcpy(out->data.sphere_collider.center, c->center, sizeof(c->center));
    out->data.sphere_collider.radius     = c->radius;
    out->data.sphere_collider.is_trigger = c->is_trigger;
    return true;
}

static bool read_character_controller(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceCharacterControllerComponent *c = jce_scene_get_character_controller(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_CHARACTER_CONTROLLER;
    out->data.character_controller.height      = c->height;
    out->data.character_controller.radius      = c->radius;
    out->data.character_controller.step_offset = c->step_offset;
    out->data.character_controller.slope_limit = c->slope_limit;
    return true;
}

static bool read_audio_source(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceAudioSourceComponent *c = jce_scene_get_audio_source(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_AUDIO_SOURCE;
    memcpy(out->data.audio_source.clip_path, c->clip_path, 128);
    out->data.audio_source.volume        = c->volume;
    out->data.audio_source.pitch         = c->pitch;
    out->data.audio_source.spatial_blend = c->spatial_blend;
    out->data.audio_source.loop          = c->loop;
    out->data.audio_source.play_on_awake = c->play_on_awake;
    return true;
}

static bool read_script(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceScriptComponent *c = jce_scene_get_script(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_SCRIPT;
    memcpy(out->data.script.script_path, c->script_path, 128);
    return true;
}

static bool read_skybox(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceSkyboxComponent *c = jce_scene_get_skybox(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_SKYBOX;
    memcpy(out->data.skybox.hdr_path, c->hdr_path, 256);
    out->data.skybox.rotation   = c->rotation;
    out->data.skybox.exposure   = c->exposure;
    out->data.skybox.use_as_ibl = c->use_as_ibl;
    return true;
}

static bool read_sprite_animator(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceSpriteAnimatorComponent *c = jce_scene_get_sprite_animator(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_SPRITE_ANIMATOR;
    auto &d = out->data.sprite_animator;
    memcpy(d.sheet_path, c->sheet_path, 128);
    memcpy(d.atlas_path, c->atlas_path, 128);
    d.frame_width  = c->frame_width;
    d.frame_height = c->frame_height;
    memcpy(d.current_anim, c->current_anim, 64);
    d.speed   = c->speed;
    d.loop    = c->loop;
    d.playing = c->playing;
    return true;
}

static bool read_constraint(JceScene *sc, JceEntity e, JceComponentInfo *out)
{
    JceConstraintComponent *c = jce_scene_get_constraint(sc, e);
    if (!c) return false;
    out->type = JCE_COMP_CONSTRAINT;
    auto &d = out->data.constraint;
    d.constraint_type   = c->constraint_type;
    d.target_entity     = c->target_entity;
    memcpy(d.pivot_a, c->pivot_a, sizeof(d.pivot_a));
    memcpy(d.pivot_b, c->pivot_b, sizeof(d.pivot_b));
    memcpy(d.axis,    c->axis,    sizeof(d.axis));
    d.lower_limit       = c->lower_limit;
    d.upper_limit       = c->upper_limit;
    d.disable_collision = c->disable_collision;
    return true;
}

/* ── Per-component: JceComponentInfo → ECS ─────────────────────────── */

static void write_transform(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceTransform t;
    euler_deg_to_transform(c->data.transform.pos, c->data.transform.rot,
                           c->data.transform.scale, &t);
    jce_scene_set_transform(sc, e, &t);
}

static void write_mesh_renderer(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof(mr));
    const auto &d = c->data.mesh_renderer;
    memcpy(mr.mesh_path,     d.mesh_path,     sizeof(mr.mesh_path));
    memcpy(mr.material_path, d.material_path, sizeof(mr.material_path));
    mr.mesh_shape   = d.mesh_shape;
    memcpy(mr.base_color, d.base_color, sizeof(mr.base_color));
    mr.metallic     = d.metallic;
    mr.roughness    = d.roughness;
    memcpy(mr.emissive, d.emissive, sizeof(mr.emissive));
    mr.normal_scale = d.normal_scale;
    mr.ao_strength  = d.ao_strength;
    mr.alpha_mode   = d.alpha_mode;
    mr.alpha_cutoff = d.alpha_cutoff;
    mr.double_sided = d.double_sided;
    memcpy(mr.albedo_tex,   d.albedo_tex,   sizeof(mr.albedo_tex));
    memcpy(mr.mr_tex,       d.mr_tex,       sizeof(mr.mr_tex));
    memcpy(mr.normal_tex,   d.normal_tex,   sizeof(mr.normal_tex));
    memcpy(mr.ao_tex,       d.ao_tex,       sizeof(mr.ao_tex));
    memcpy(mr.emissive_tex, d.emissive_tex, sizeof(mr.emissive_tex));
    jce_scene_set_mesh_renderer(sc, e, &mr);
}

static void write_light(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    const auto &d = c->data.light;

    /* Remove all existing light types first. */
    jce_scene_remove_dir_light(sc, e);
    jce_scene_remove_point_light(sc, e);
    jce_scene_remove_spot_light(sc, e);

    switch (d.type) {
    case 0: { /* directional */
        JceDirectionalLight dl;
        memset(&dl, 0, sizeof(dl));
        dl.color       = jce_v3(d.color[0], d.color[1], d.color[2]);
        dl.intensity   = d.intensity;
        dl.direction   = jce_v3(0, -1, 0);
        dl.casts_shadow = d.casts_shadow;
        jce_scene_set_dir_light(sc, e, &dl);
        break;
    }
    case 1: { /* point */
        JcePointLight pl;
        memset(&pl, 0, sizeof(pl));
        pl.color     = jce_v3(d.color[0], d.color[1], d.color[2]);
        pl.intensity = d.intensity;
        pl.radius    = d.radius;
        jce_scene_set_point_light(sc, e, &pl);
        break;
    }
    case 2: { /* spot */
        JceSpotLight sl;
        memset(&sl, 0, sizeof(sl));
        sl.color          = jce_v3(d.color[0], d.color[1], d.color[2]);
        sl.intensity      = d.intensity;
        sl.radius         = d.radius;
        sl.inner_cone_cos = cosf(d.inner_cone_deg * JCE_DEG2RAD);
        sl.outer_cone_cos = cosf(d.outer_cone_deg * JCE_DEG2RAD);
        jce_scene_set_spot_light(sc, e, &sl);
        break;
    }
    default: break;
    }
}

static void write_camera(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceCameraComponent cam;
    memset(&cam, 0, sizeof(cam));
    cam.fov_deg    = c->data.camera.fov;
    cam.near_plane = c->data.camera.near_clip;
    cam.far_plane  = c->data.camera.far_clip;
    cam.ortho      = c->data.camera.ortho;
    cam.is_primary = true; /* default for editor */
    jce_scene_set_camera(sc, e, &cam);
}

static void write_sprite_renderer(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceSpriteRendererComponent sr;
    memset(&sr, 0, sizeof(sr));
    memcpy(sr.sprite_path, c->data.sprite_renderer.sprite_path, 128);
    memcpy(sr.color, c->data.sprite_renderer.color, sizeof(sr.color));
    sr.flip_x        = c->data.sprite_renderer.flip_x;
    sr.flip_y        = c->data.sprite_renderer.flip_y;
    sr.sorting_order = c->data.sprite_renderer.sorting_order;
    jce_scene_set_sprite_renderer(sc, e, &sr);
}

static void write_animator(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceAnimatorComponent a;
    memset(&a, 0, sizeof(a));
    memcpy(a.clip_name, c->data.animator.clip_name, 64);
    a.speed   = c->data.animator.speed;
    a.loop    = c->data.animator.loop;
    a.playing = c->data.animator.playing;
    jce_scene_set_animator(sc, e, &a);
}

static void write_skeletal_animator(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceSkeletalAnimatorComponent sa;
    memset(&sa, 0, sizeof(sa));
    const auto &d = c->data.skeletal_animator;
    memcpy(sa.skeleton_path, d.skeleton_path, 128);
    memcpy(sa.clip_names, d.clip_names, sizeof(sa.clip_names));
    sa.clip_count  = d.clip_count;
    sa.active_clip = d.active_clip;
    sa.speed       = d.speed;
    sa.loop        = d.loop;
    sa.playing     = d.playing;
    jce_scene_set_skeletal_animator(sc, e, &sa);
}

static void write_rigidbody(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof(rb));
    rb.mass         = c->data.rigidbody.mass;
    rb.drag         = c->data.rigidbody.drag;
    rb.angular_drag = c->data.rigidbody.angular_drag;
    rb.use_gravity  = c->data.rigidbody.use_gravity;
    rb.is_kinematic = c->data.rigidbody.is_kinematic;
    rb.body_type    = c->data.rigidbody.is_kinematic ? 1 : 0;
    jce_scene_set_rigidbody(sc, e, &rb);
}

static void write_box_collider(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceBoxColliderComponent bc;
    memset(&bc, 0, sizeof(bc));
    memcpy(bc.center, c->data.box_collider.center, sizeof(bc.center));
    memcpy(bc.size,   c->data.box_collider.size,   sizeof(bc.size));
    bc.is_trigger = c->data.box_collider.is_trigger;
    jce_scene_set_box_collider(sc, e, &bc);
}

static void write_sphere_collider(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceSphereColliderComponent sc2;
    memset(&sc2, 0, sizeof(sc2));
    memcpy(sc2.center, c->data.sphere_collider.center, sizeof(sc2.center));
    sc2.radius     = c->data.sphere_collider.radius;
    sc2.is_trigger = c->data.sphere_collider.is_trigger;
    jce_scene_set_sphere_collider(sc, e, &sc2);
}

static void write_character_controller(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceCharacterControllerComponent cc;
    memset(&cc, 0, sizeof(cc));
    cc.height      = c->data.character_controller.height;
    cc.radius      = c->data.character_controller.radius;
    cc.step_offset = c->data.character_controller.step_offset;
    cc.slope_limit = c->data.character_controller.slope_limit;
    jce_scene_set_character_controller(sc, e, &cc);
}

static void write_audio_source(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceAudioSourceComponent as;
    memset(&as, 0, sizeof(as));
    memcpy(as.clip_path, c->data.audio_source.clip_path, 128);
    as.volume        = c->data.audio_source.volume;
    as.pitch         = c->data.audio_source.pitch;
    as.spatial_blend = c->data.audio_source.spatial_blend;
    as.loop          = c->data.audio_source.loop;
    as.play_on_awake = c->data.audio_source.play_on_awake;
    jce_scene_set_audio_source(sc, e, &as);
}

static void write_script(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceScriptComponent s;
    memset(&s, 0, sizeof(s));
    memcpy(s.script_path, c->data.script.script_path, 128);
    jce_scene_set_script(sc, e, &s);
}

static void write_skybox(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceSkyboxComponent sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.hdr_path, c->data.skybox.hdr_path, 256);
    sb.rotation   = c->data.skybox.rotation;
    sb.exposure   = c->data.skybox.exposure;
    sb.use_as_ibl = c->data.skybox.use_as_ibl;
    jce_scene_set_skybox(sc, e, &sb);
}

static void write_sprite_animator(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceSpriteAnimatorComponent sa;
    memset(&sa, 0, sizeof(sa));
    const auto &d = c->data.sprite_animator;
    memcpy(sa.sheet_path, d.sheet_path, 128);
    memcpy(sa.atlas_path, d.atlas_path, 128);
    sa.frame_width  = d.frame_width;
    sa.frame_height = d.frame_height;
    memcpy(sa.current_anim, d.current_anim, 64);
    sa.speed   = d.speed;
    sa.loop    = d.loop;
    sa.playing = d.playing;
    jce_scene_set_sprite_animator(sc, e, &sa);
}

static void write_constraint(JceScene *sc, JceEntity e, const JceComponentInfo *c)
{
    JceConstraintComponent cn;
    memset(&cn, 0, sizeof(cn));
    const auto &d = c->data.constraint;
    cn.constraint_type   = d.constraint_type;
    cn.target_entity     = d.target_entity;
    memcpy(cn.pivot_a, d.pivot_a, sizeof(cn.pivot_a));
    memcpy(cn.pivot_b, d.pivot_b, sizeof(cn.pivot_b));
    memcpy(cn.axis,    d.axis,    sizeof(cn.axis));
    cn.lower_limit       = d.lower_limit;
    cn.upper_limit       = d.upper_limit;
    cn.disable_collision = d.disable_collision;
    jce_scene_set_constraint(sc, e, &cn);
}

/* ── Public: single-component read ─────────────────────────────────── */

bool jce_adapter_ecs_to_comp_info(JceScene *scene, JceEntity entity,
                                  JceComponentType type, JceComponentInfo *out)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !out) return false;
    memset(out, 0, sizeof(*out));
    switch (type) {
    case JCE_COMP_TRANSFORM:            return read_transform(scene, entity, out);
    case JCE_COMP_MESH_RENDERER:        return read_mesh_renderer(scene, entity, out);
    case JCE_COMP_LIGHT:                return read_light(scene, entity, out);
    case JCE_COMP_CAMERA:               return read_camera(scene, entity, out);
    case JCE_COMP_SPRITE_RENDERER:      return read_sprite_renderer(scene, entity, out);
    case JCE_COMP_ANIMATOR:             return read_animator(scene, entity, out);
    case JCE_COMP_SKELETAL_ANIMATOR:    return read_skeletal_animator(scene, entity, out);
    case JCE_COMP_RIGIDBODY:            return read_rigidbody(scene, entity, out);
    case JCE_COMP_BOX_COLLIDER:         return read_box_collider(scene, entity, out);
    case JCE_COMP_SPHERE_COLLIDER:      return read_sphere_collider(scene, entity, out);
    case JCE_COMP_CHARACTER_CONTROLLER: return read_character_controller(scene, entity, out);
    case JCE_COMP_AUDIO_SOURCE:         return read_audio_source(scene, entity, out);
    case JCE_COMP_SCRIPT:               return read_script(scene, entity, out);
    case JCE_COMP_SKYBOX:               return read_skybox(scene, entity, out);
    case JCE_COMP_SPRITE_ANIMATOR:      return read_sprite_animator(scene, entity, out);
    case JCE_COMP_CONSTRAINT:           return read_constraint(scene, entity, out);
    default: return false;
    }
}

/* ── Public: single-component write ────────────────────────────────── */

void jce_adapter_comp_info_to_ecs(JceScene *scene, JceEntity entity,
                                  const JceComponentInfo *comp)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !comp) return;
    switch (comp->type) {
    case JCE_COMP_TRANSFORM:            write_transform(scene, entity, comp); break;
    case JCE_COMP_MESH_RENDERER:        write_mesh_renderer(scene, entity, comp); break;
    case JCE_COMP_LIGHT:                write_light(scene, entity, comp); break;
    case JCE_COMP_CAMERA:               write_camera(scene, entity, comp); break;
    case JCE_COMP_SPRITE_RENDERER:      write_sprite_renderer(scene, entity, comp); break;
    case JCE_COMP_ANIMATOR:             write_animator(scene, entity, comp); break;
    case JCE_COMP_SKELETAL_ANIMATOR:    write_skeletal_animator(scene, entity, comp); break;
    case JCE_COMP_RIGIDBODY:            write_rigidbody(scene, entity, comp); break;
    case JCE_COMP_BOX_COLLIDER:         write_box_collider(scene, entity, comp); break;
    case JCE_COMP_SPHERE_COLLIDER:      write_sphere_collider(scene, entity, comp); break;
    case JCE_COMP_CHARACTER_CONTROLLER: write_character_controller(scene, entity, comp); break;
    case JCE_COMP_AUDIO_SOURCE:         write_audio_source(scene, entity, comp); break;
    case JCE_COMP_SCRIPT:               write_script(scene, entity, comp); break;
    case JCE_COMP_SKYBOX:               write_skybox(scene, entity, comp); break;
    case JCE_COMP_SPRITE_ANIMATOR:      write_sprite_animator(scene, entity, comp); break;
    case JCE_COMP_CONSTRAINT:           write_constraint(scene, entity, comp); break;
    default: break;
    }
}

/* ── Public: remove component ──────────────────────────────────────── */

void jce_adapter_remove_from_ecs(JceScene *scene, JceEntity entity,
                                 JceComponentType type)
{
    if (!scene || entity == JCE_ENTITY_INVALID) return;
    switch (type) {
    case JCE_COMP_TRANSFORM:            jce_scene_remove_transform(scene, entity); break;
    case JCE_COMP_MESH_RENDERER:        jce_scene_remove_mesh_renderer(scene, entity); break;
    case JCE_COMP_LIGHT:
        jce_scene_remove_dir_light(scene, entity);
        jce_scene_remove_point_light(scene, entity);
        jce_scene_remove_spot_light(scene, entity);
        break;
    case JCE_COMP_CAMERA:               jce_scene_remove_camera(scene, entity); break;
    case JCE_COMP_SPRITE_RENDERER:      jce_scene_remove_sprite_renderer(scene, entity); break;
    case JCE_COMP_ANIMATOR:             jce_scene_remove_animator(scene, entity); break;
    case JCE_COMP_SKELETAL_ANIMATOR:    jce_scene_remove_skeletal_animator(scene, entity); break;
    case JCE_COMP_RIGIDBODY:            jce_scene_remove_rigidbody(scene, entity); break;
    case JCE_COMP_BOX_COLLIDER:         jce_scene_remove_box_collider(scene, entity); break;
    case JCE_COMP_SPHERE_COLLIDER:      jce_scene_remove_sphere_collider(scene, entity); break;
    case JCE_COMP_CHARACTER_CONTROLLER: jce_scene_remove_character_controller(scene, entity); break;
    case JCE_COMP_AUDIO_SOURCE:         jce_scene_remove_audio_source(scene, entity); break;
    case JCE_COMP_SCRIPT:               jce_scene_remove_script(scene, entity); break;
    case JCE_COMP_SKYBOX:               jce_scene_remove_skybox(scene, entity); break;
    case JCE_COMP_SPRITE_ANIMATOR:      jce_scene_remove_sprite_animator(scene, entity); break;
    case JCE_COMP_CONSTRAINT:           jce_scene_remove_constraint(scene, entity); break;
    default: break;
    }
}

/* ── Public: entity info ───────────────────────────────────────────── */

void jce_adapter_ecs_to_entity_info(JceScene *scene, JceEntity entity,
                                    JceEntityInfo *out)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !out) return;

    /* Name: prefer EditorMeta.name (supports duplicates). */
    JceEditorMeta *em_info = jce_scene_get_editor_meta(scene, entity);
    if (em_info && em_info->name[0])
        snprintf(out->name, sizeof(out->name), "%s", em_info->name);
    else {
        const char *name = jce_scene_entity_name(scene, entity);
        if (name)
            snprintf(out->name, sizeof(out->name), "%s", name);
        else
            snprintf(out->name, sizeof(out->name), "%s", "Entity");
    }

    out->enabled = true;
    out->tag_color = JCE_TAG_NONE;
    out->tag[0] = '\0';
    out->prefab_instance = false;
    out->prefab_path[0] = '\0';

    /* Pull metadata from EditorMeta component if present. */
    jce_adapter_pull_entity_meta(scene, entity, out);

    out->component_count = jce_adapter_get_comp_count(scene, entity);
}

int jce_adapter_get_comp_count(JceScene *scene, JceEntity entity)
{
    if (!scene || entity == JCE_ENTITY_INVALID) return 0;

    uint32_t flags = jce_scene_get_component_flags(scene, entity);
    int count = 0;

    if (flags & JCE_COMP_FLAG_TRANSFORM)            count++;
    if (flags & JCE_COMP_FLAG_MESH_RENDERER)        count++;
    if (flags & JCE_COMP_FLAG_CAMERA)               count++;
    /* 3 engine light types → 1 editor Light */
    if (flags & (JCE_COMP_FLAG_DIR_LIGHT | JCE_COMP_FLAG_POINT_LIGHT |
                 JCE_COMP_FLAG_SPOT_LIGHT))         count++;
    if (flags & JCE_COMP_FLAG_SPRITE_RENDERER)      count++;
    if (flags & JCE_COMP_FLAG_ANIMATOR)              count++;
    if (flags & JCE_COMP_FLAG_SKELETAL_ANIMATOR)     count++;
    if (flags & JCE_COMP_FLAG_RIGIDBODY)             count++;
    if (flags & JCE_COMP_FLAG_BOX_COLLIDER)          count++;
    if (flags & JCE_COMP_FLAG_SPHERE_COLLIDER)       count++;
    if (flags & JCE_COMP_FLAG_CHARACTER_CONTROLLER)  count++;
    if (flags & JCE_COMP_FLAG_AUDIO_SOURCE)          count++;
    if (flags & JCE_COMP_FLAG_SCRIPT)                count++;
    if (flags & JCE_COMP_FLAG_SKYBOX)                count++;
    if (flags & JCE_COMP_FLAG_SPRITE_ANIMATOR)       count++;
    if (flags & JCE_COMP_FLAG_CONSTRAINT)            count++;
    /* EditorMeta not counted (stored on entity info, not as visible component). */

    return count;
}

int jce_adapter_get_all_comps(JceScene *scene, JceEntity entity,
                              JceComponentInfo *out, int max_out)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !out || max_out <= 0)
        return 0;

    static const JceComponentType types[] = {
        JCE_COMP_TRANSFORM,
        JCE_COMP_MESH_RENDERER,
        JCE_COMP_LIGHT,
        JCE_COMP_CAMERA,
        JCE_COMP_SPRITE_RENDERER,
        JCE_COMP_ANIMATOR,
        JCE_COMP_SKELETAL_ANIMATOR,
        JCE_COMP_RIGIDBODY,
        JCE_COMP_BOX_COLLIDER,
        JCE_COMP_SPHERE_COLLIDER,
        JCE_COMP_CHARACTER_CONTROLLER,
        JCE_COMP_AUDIO_SOURCE,
        JCE_COMP_SCRIPT,
        JCE_COMP_SKYBOX,
        JCE_COMP_SPRITE_ANIMATOR,
        JCE_COMP_CONSTRAINT,
    };

    int count = 0;
    for (int i = 0; i < (int)(sizeof(types) / sizeof(types[0])) && count < max_out; i++) {
        JceComponentInfo ci;
        if (jce_adapter_ecs_to_comp_info(scene, entity, types[i], &ci))
            out[count++] = ci;
    }
    return count;
}

/* ── Public: entity metadata helpers ───────────────────────────────── */

void jce_adapter_push_entity_meta(JceScene *scene, JceEntity entity,
                                  const JceEntityInfo *info)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !info) return;

    JceEditorMeta em;
    memset(&em, 0, sizeof(em));
    snprintf(em.name, sizeof(em.name), "%s", info->name);
    snprintf(em.tag, sizeof(em.tag), "%s", info->tag);
    em.tag_color       = (uint8_t)info->tag_color;
    em.enabled         = info->enabled;
    em.prefab_instance = info->prefab_instance;
    snprintf(em.prefab_path, sizeof(em.prefab_path), "%s", info->prefab_path);

    jce_scene_set_editor_meta(scene, entity, &em);
}

void jce_adapter_pull_entity_meta(JceScene *scene, JceEntity entity,
                                  JceEntityInfo *info)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !info) return;

    JceEditorMeta *em = jce_scene_get_editor_meta(scene, entity);
    if (!em) return;

    if (em->name[0])
        snprintf(info->name, sizeof(info->name), "%s", em->name);
    snprintf(info->tag, sizeof(info->tag), "%s", em->tag);
    info->tag_color       = (JceTagColor)em->tag_color;
    info->enabled         = em->enabled;
    info->prefab_instance = em->prefab_instance;
    snprintf(info->prefab_path, sizeof(info->prefab_path), "%s", em->prefab_path);
}

/* ── Bulk sync: editor arrays → ECS ────────────────────────────────── */

void jce_adapter_sync_editor_to_ecs(void)
{
    JceScene *scene = s.scene;
    if (!scene) return;

    for (int i = 0; i < s.entity_count; i++) {
        JceEntityInfo *e = &s.entities[i];
        if (e->id == 0) continue;

        JceEntity ent = (JceEntity)e->ecs_entity;
        if (ent == JCE_ENTITY_INVALID) continue;

        /* Sync editor metadata → EditorMeta component (includes name). */
        jce_adapter_push_entity_meta(scene, ent, e);

        /* Sync all components. */
        for (int ci = 0; ci < e->component_count; ci++) {
            jce_adapter_comp_info_to_ecs(scene, ent, &s.components[i][ci]);
        }
    }

    /* Sync parent/child relationships. */
    for (int i = 0; i < s.entity_count; i++) {
        JceEntityInfo *e = &s.entities[i];
        if (e->id == 0 || e->ecs_entity == 0) continue;
        if (e->parent_id == 0) continue;

        /* Find parent's ECS entity. */
        for (int j = 0; j < s.entity_count; j++) {
            if (s.entities[j].id == e->parent_id && s.entities[j].ecs_entity != 0) {
                jce_scene_set_parent(scene, (JceEntity)e->ecs_entity,
                                     (JceEntity)s.entities[j].ecs_entity);
                break;
            }
        }
    }
}

/* ── Bulk sync: ECS → editor arrays ────────────────────────────────── */

typedef struct {
    int idx;
} EcsToEditorCtx;

static void ecs_to_editor_cb(JceScene *sc, JceEntity e, void *user_data)
{
    (void)user_data;

    if (s.entity_count >= JCE_MAX_ENTITIES) return;

    int idx = s.entity_count;
    JceEntityInfo *info = &s.entities[idx];
    memset(info, 0, sizeof(*info));

    /* Assign editor ID (monotonic). */
    info->id = s.next_id++;
    info->ecs_entity = (uint64_t)e;

    /* Populate entity info from ECS. */
    jce_adapter_ecs_to_entity_info(sc, e, info);

    /* Populate components from ECS. */
    int comp_count = jce_adapter_get_all_comps(sc, e,
        s.components[idx], JCE_MAX_COMPONENTS);
    info->component_count = comp_count;

    s.entity_count++;
}

void jce_adapter_sync_ecs_to_editor(void)
{
    JceScene *scene = s.scene;
    if (!scene) return;

    /* Clear existing editor data. */
    memset(s.entities, 0, sizeof(s.entities));
    memset(s.components, 0, sizeof(s.components));
    s.entity_count = 0;
    s.next_id = 1;

    /* Iterate all ECS entities → populate editor arrays. */
    jce_scene_each_entity(scene, ecs_to_editor_cb, NULL);

    /* Resolve parent/child relationships using ECS hierarchy.
     * Build a mapping from JceEntity → editor uint32_t ID first. */
    for (int i = 0; i < s.entity_count; i++) {
        JceEntityInfo *info = &s.entities[i];
        JceEntity ecs_e = (JceEntity)info->ecs_entity;

        JceEntity ecs_parent = jce_scene_get_parent(scene, ecs_e);
        if (ecs_parent == JCE_ENTITY_INVALID) {
            info->parent_id = 0;
            continue;
        }

        /* Find parent's editor ID. */
        for (int j = 0; j < s.entity_count; j++) {
            if ((JceEntity)s.entities[j].ecs_entity == ecs_parent) {
                info->parent_id = s.entities[j].id;
                break;
            }
        }
    }

    /* Build children arrays from parent_id references. */
    for (int i = 0; i < s.entity_count; i++) {
        s.entities[i].child_count = 0;
    }
    for (int i = 0; i < s.entity_count; i++) {
        if (s.entities[i].parent_id == 0) continue;
        for (int j = 0; j < s.entity_count; j++) {
            if (s.entities[j].id == s.entities[i].parent_id) {
                JceEntityInfo *parent = &s.entities[j];
                if (parent->child_count < JCE_MAX_CHILDREN)
                    parent->children[parent->child_count++] = s.entities[i].id;
                break;
            }
        }
    }

    /* Clear selection (entities changed). */
    s.selected_count = 0;
    s.focused = 0;

    /* Rebuild O(1) index map from refreshed cache. */
    jce_state_rebuild_id_map();
}
