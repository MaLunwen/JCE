/*
 * jce_scene.c  ECS scene implementation (flecs backend).
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include "os/core/jce_memory.h"

#include <flecs.h>
#include <string.h>

#define LOG_TAG "scene"

/* ── Component IDs (registered once per world) ─────────────────────── */

static ECS_COMPONENT_DECLARE(JceTransform);
static ECS_COMPONENT_DECLARE(JceMeshRenderer);
static ECS_COMPONENT_DECLARE(JceCameraComponent);
static ECS_COMPONENT_DECLARE(JceDirectionalLight);
static ECS_COMPONENT_DECLARE(JcePointLight);
static ECS_COMPONENT_DECLARE(JceSpotLight);
static ECS_COMPONENT_DECLARE(JceTagActive);
static ECS_COMPONENT_DECLARE(JceRigidBodyComponent);
static ECS_COMPONENT_DECLARE(JceRigidBody2DComponent);
static ECS_COMPONENT_DECLARE(JceParticleEmitterComponent);
static ECS_COMPONENT_DECLARE(JceBehaviorTree);
static ECS_COMPONENT_DECLARE(JceSkyboxComponent);
static ECS_COMPONENT_DECLARE(JceSpriteRendererComponent);
static ECS_COMPONENT_DECLARE(JceSpriteAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceSkeletalAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceConstraintComponent);
static ECS_COMPONENT_DECLARE(JceBoxColliderComponent);
static ECS_COMPONENT_DECLARE(JceSphereColliderComponent);
static ECS_COMPONENT_DECLARE(JceCharacterControllerComponent);
static ECS_COMPONENT_DECLARE(JceAudioSourceComponent);
static ECS_COMPONENT_DECLARE(JceScriptComponent);
static ECS_COMPONENT_DECLARE(JceEditorMeta);
static ECS_COMPONENT_DECLARE(JceTerrainComponent);
static ECS_COMPONENT_DECLARE(JceLodGroupComponent);
static ECS_COMPONENT_DECLARE(JceVirtualCameraComponent);
static ECS_COMPONENT_DECLARE(JceTriggerVolumeComponent);
static ECS_COMPONENT_DECLARE(JceCapsuleColliderComponent);
static ECS_COMPONENT_DECLARE(JceMeshColliderComponent);
static ECS_COMPONENT_DECLARE(JceCollider2DComponent);
static ECS_COMPONENT_DECLARE(JceTrailRendererComponent);
static ECS_COMPONENT_DECLARE(JceLineRendererComponent);
static ECS_COMPONENT_DECLARE(JceReflectionProbeComponent);
static ECS_COMPONENT_DECLARE(JceDecalComponent);
static ECS_COMPONENT_DECLARE(JceLightProbeGroupComponent);
static ECS_COMPONENT_DECLARE(JceAudioListenerComponent);
static ECS_COMPONENT_DECLARE(JceAudioReverbZoneComponent);
static ECS_COMPONENT_DECLARE(JceAudioOcclusionComponent);
static ECS_COMPONENT_DECLARE(JceSpawnManagerComponent);
static ECS_COMPONENT_DECLARE(JceWeaponComponent);
static ECS_COMPONENT_DECLARE(JceSavePointComponent);
static ECS_COMPONENT_DECLARE(JceWheelColliderComponent);
static ECS_COMPONENT_DECLARE(JceConstantForceComponent);
static ECS_COMPONENT_DECLARE(JceConfigurableJointComponent);
static ECS_COMPONENT_DECLARE(JceJoint2DComponent);
static ECS_COMPONENT_DECLARE(JceBillboardRendererComponent);
static ECS_COMPONENT_DECLARE(JceCanvasComponent);
static ECS_COMPONENT_DECLARE(JceCanvasGroupComponent);
static ECS_COMPONENT_DECLARE(JceLayoutGroupComponent);
static ECS_COMPONENT_DECLARE(JceUIImageComponent);
static ECS_COMPONENT_DECLARE(JceUITextComponent);
static ECS_COMPONENT_DECLARE(JceUIButtonComponent);
static ECS_COMPONENT_DECLARE(JceAudioBusRouteComponent);
static ECS_COMPONENT_DECLARE(JceAnimationLayerStateComponent);
static ECS_COMPONENT_DECLARE(JceBlendShapeWeightsComponent);

/* ── Scene struct ──────────────────────────────────────────────────── */

struct JceScene {
    ecs_world_t *world;
};

/* ── Create / destroy ──────────────────────────────────────────────── */

JceScene *jce_scene_create(void)
{
    JceScene *s = (JceScene *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;

    s->world = ecs_init();
    if (!s->world) {
        JCE_FREE(s);
    }

    /* Register components. */
    ECS_COMPONENT_DEFINE(s->world, JceTransform);
    ECS_COMPONENT_DEFINE(s->world, JceMeshRenderer);
    ECS_COMPONENT_DEFINE(s->world, JceCameraComponent);
    ECS_COMPONENT_DEFINE(s->world, JceDirectionalLight);
    ECS_COMPONENT_DEFINE(s->world, JcePointLight);
    ECS_COMPONENT_DEFINE(s->world, JceSpotLight);
    ECS_COMPONENT_DEFINE(s->world, JceTagActive);
    ECS_COMPONENT_DEFINE(s->world, JceRigidBodyComponent);
    ECS_COMPONENT_DEFINE(s->world, JceRigidBody2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceParticleEmitterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBehaviorTree);
    ECS_COMPONENT_DEFINE(s->world, JceSkyboxComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSpriteRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSpriteAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSkeletalAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceConstraintComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBoxColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSphereColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCharacterControllerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioSourceComponent);
    ECS_COMPONENT_DEFINE(s->world, JceScriptComponent);
    ECS_COMPONENT_DEFINE(s->world, JceEditorMeta);
    ECS_COMPONENT_DEFINE(s->world, JceTerrainComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLodGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVirtualCameraComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTriggerVolumeComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCapsuleColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceMeshColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCollider2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTrailRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLineRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceReflectionProbeComponent);
    ECS_COMPONENT_DEFINE(s->world, JceDecalComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLightProbeGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioListenerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioReverbZoneComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioOcclusionComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSpawnManagerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceWeaponComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSavePointComponent);
    ECS_COMPONENT_DEFINE(s->world, JceWheelColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceConstantForceComponent);
    ECS_COMPONENT_DEFINE(s->world, JceConfigurableJointComponent);
    ECS_COMPONENT_DEFINE(s->world, JceJoint2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBillboardRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCanvasComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCanvasGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLayoutGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIImageComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUITextComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIButtonComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioBusRouteComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAnimationLayerStateComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBlendShapeWeightsComponent);

    LOG_SUCCESS(LOG_TAG, "scene created");
    return s;
}

void jce_scene_destroy(JceScene *s)
{
    if (!s) return;
    if (s->world) ecs_fini(s->world);
    JCE_FREE(s);
    LOG_INFO(LOG_TAG, "scene destroyed");
}

/* ── Entity management ─────────────────────────────────────────────── */

JceEntity jce_scene_create_entity(JceScene *s, const char *name)
{
    if (!s) return JCE_ENTITY_INVALID;

    ecs_entity_t e = ecs_new(s->world);
    if (name && name[0])
        ecs_set_name(s->world, e, name);

    /* Default transform. */
    JceTransform t;
    t.position = jce_v3(0, 0, 0);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1, 1, 1);
    ecs_set_ptr(s->world, e, JceTransform, &t);

    /* Active by default. */
    ecs_add(s->world, e, JceTagActive);

    return (JceEntity)e;
}

void jce_scene_destroy_entity(JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    ecs_delete(s->world, (ecs_entity_t)e);
}

const char *jce_scene_entity_name(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return "(none)";
    const char *name = ecs_get_name(s->world, (ecs_entity_t)e);
    return name ? name : "(unnamed)";
}

const char *jce_scene_entity_registered_name(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return NULL;
    return ecs_get_name(s->world, (ecs_entity_t)e);
}

void jce_scene_set_entity_name(JceScene *s, JceEntity e, const char *name)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    ecs_set_name(s->world, (ecs_entity_t)e, name);
}

/* ── Parent / child hierarchy ──────────────────────────────────────── */

void jce_scene_set_parent(JceScene *s, JceEntity child, JceEntity parent)
{
    if (!s || child == JCE_ENTITY_INVALID) return;
    if (parent == JCE_ENTITY_INVALID) {
        /* Remove parent (make root entity). */
        ecs_entity_t cur = ecs_get_parent(s->world, (ecs_entity_t)child);
        if (cur) ecs_remove_pair(s->world, (ecs_entity_t)child, EcsChildOf, cur);
    } else {
        ecs_add_pair(s->world, (ecs_entity_t)child,
                     EcsChildOf, (ecs_entity_t)parent);
    }
}

JceEntity jce_scene_get_parent(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return JCE_ENTITY_INVALID;
    ecs_entity_t p = ecs_get_parent(s->world, (ecs_entity_t)e);
    return (JceEntity)p;
}

int jce_scene_get_children(const JceScene *s, JceEntity parent,
                           JceEntity *out, int max_out)
{
    if (!s || parent == JCE_ENTITY_INVALID || !out || max_out <= 0) return 0;

    ecs_query_t *q = ecs_query(s->world, {
        .terms = {{ .id = ecs_pair(EcsChildOf, (ecs_entity_t)parent) }},
    });

    int count = 0;
    ecs_iter_t it = ecs_query_iter(s->world, q);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count && count < max_out; i++) {
            out[count++] = (JceEntity)it.entities[i];
        }
    }
    ecs_query_fini(q);
    return count;
}

int jce_scene_get_child_count(const JceScene *s, JceEntity parent)
{
    if (!s || parent == JCE_ENTITY_INVALID) return 0;

    ecs_query_t *q = ecs_query(s->world, {
        .terms = {{ .id = ecs_pair(EcsChildOf, (ecs_entity_t)parent) }},
    });

    int count = 0;
    ecs_iter_t it = ecs_query_iter(s->world, q);
    while (ecs_query_next(&it)) {
        count += it.count;
    }
    ecs_query_fini(q);
    return count;
}

/* ── Component setters / getters / has / remove (macro-generated) ─── */

#define JCE_COMP_IMPL(TYPE, NAME)                                       \
void jce_scene_set_##NAME(JceScene *s, JceEntity e, const TYPE *v)      \
{                                                                       \
    if (!s || !v) return;                                               \
    ecs_set_ptr(s->world, (ecs_entity_t)e, TYPE, v);                    \
}                                                                       \
                                                                        \
TYPE *jce_scene_get_##NAME(JceScene *s, JceEntity e)                    \
{                                                                       \
    if (!s) return NULL;                                                \
    return (TYPE *)ecs_get_mut(s->world, (ecs_entity_t)e, TYPE);        \
}                                                                       \
                                                                        \
bool jce_scene_has_##NAME(const JceScene *s, JceEntity e)               \
{                                                                       \
    if (!s) return false;                                               \
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return false;         \
    return ecs_has(s->world, (ecs_entity_t)e, TYPE);                    \
}                                                                       \
                                                                        \
void jce_scene_remove_##NAME(JceScene *s, JceEntity e)                  \
{                                                                       \
    if (!s) return;                                                     \
    ecs_remove(s->world, (ecs_entity_t)e, TYPE);                        \
}

JCE_COMP_IMPL(JceTransform,                   transform)
JCE_COMP_IMPL(JceMeshRenderer,                mesh_renderer)
JCE_COMP_IMPL(JceCameraComponent,             camera)
JCE_COMP_IMPL(JceDirectionalLight,            dir_light)
JCE_COMP_IMPL(JcePointLight,                  point_light)
JCE_COMP_IMPL(JceSpotLight,                   spot_light)
JCE_COMP_IMPL(JceSkyboxComponent,             skybox)
JCE_COMP_IMPL(JceSpriteRendererComponent,     sprite_renderer)
JCE_COMP_IMPL(JceSpriteAnimatorComponent,     sprite_animator)
JCE_COMP_IMPL(JceAnimatorComponent,           animator)
JCE_COMP_IMPL(JceSkeletalAnimatorComponent,   skeletal_animator)
JCE_COMP_IMPL(JceConstraintComponent,         constraint)
JCE_COMP_IMPL(JceRigidBodyComponent,          rigidbody)
JCE_COMP_IMPL(JceRigidBody2DComponent,        rigidbody2d)
JCE_COMP_IMPL(JceBoxColliderComponent,        box_collider)
JCE_COMP_IMPL(JceSphereColliderComponent,     sphere_collider)
JCE_COMP_IMPL(JceCharacterControllerComponent,character_controller)
JCE_COMP_IMPL(JceAudioSourceComponent,        audio_source)
JCE_COMP_IMPL(JceScriptComponent,             script)
JCE_COMP_IMPL(JceParticleEmitterComponent,    particle_emitter)
JCE_COMP_IMPL(JceBehaviorTree,                behavior_tree)
JCE_COMP_IMPL(JceEditorMeta,                  editor_meta)
JCE_COMP_IMPL(JceTerrainComponent,            terrain)
JCE_COMP_IMPL(JceLodGroupComponent,           lod_group)
JCE_COMP_IMPL(JceVirtualCameraComponent,      virtual_camera)
JCE_COMP_IMPL(JceTriggerVolumeComponent,      trigger_volume)
JCE_COMP_IMPL(JceCapsuleColliderComponent,    capsule_collider)
JCE_COMP_IMPL(JceMeshColliderComponent,       mesh_collider)
JCE_COMP_IMPL(JceCollider2DComponent,         collider2d)
JCE_COMP_IMPL(JceTrailRendererComponent,      trail_renderer)
JCE_COMP_IMPL(JceLineRendererComponent,       line_renderer)
JCE_COMP_IMPL(JceReflectionProbeComponent,    reflection_probe)
JCE_COMP_IMPL(JceDecalComponent,              decal)
JCE_COMP_IMPL(JceLightProbeGroupComponent,    light_probe_group)
JCE_COMP_IMPL(JceAudioListenerComponent,      audio_listener)
JCE_COMP_IMPL(JceAudioReverbZoneComponent,    audio_reverb_zone)
JCE_COMP_IMPL(JceAudioOcclusionComponent,     audio_occlusion)
JCE_COMP_IMPL(JceSpawnManagerComponent,       spawn_manager)
JCE_COMP_IMPL(JceWeaponComponent,             weapon)
JCE_COMP_IMPL(JceSavePointComponent,          save_point)
JCE_COMP_IMPL(JceWheelColliderComponent,      wheel_collider)
JCE_COMP_IMPL(JceConstantForceComponent,      constant_force)
JCE_COMP_IMPL(JceConfigurableJointComponent,  configurable_joint)
JCE_COMP_IMPL(JceJoint2DComponent,            joint2d)
JCE_COMP_IMPL(JceBillboardRendererComponent,  billboard_renderer)
JCE_COMP_IMPL(JceCanvasComponent,             canvas)
JCE_COMP_IMPL(JceCanvasGroupComponent,        canvas_group)
JCE_COMP_IMPL(JceLayoutGroupComponent,        layout_group)
JCE_COMP_IMPL(JceUIImageComponent,            ui_image)
JCE_COMP_IMPL(JceUITextComponent,             ui_text)
JCE_COMP_IMPL(JceUIButtonComponent,           ui_button)
JCE_COMP_IMPL(JceAudioBusRouteComponent,      audio_bus_route)
JCE_COMP_IMPL(JceAnimationLayerStateComponent, animation_layer_state)
JCE_COMP_IMPL(JceBlendShapeWeightsComponent,   blend_shape_weights)

#undef JCE_COMP_IMPL

/* ── Component enumeration ─────────────────────────────────────────── */

uint64_t jce_scene_get_component_flags(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return 0;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return 0;
    uint64_t flags = 0;

    if (ecs_has(s->world, ent, JceTransform))                   flags |= JCE_COMP_FLAG_TRANSFORM;
    if (ecs_has(s->world, ent, JceMeshRenderer))                flags |= JCE_COMP_FLAG_MESH_RENDERER;
    if (ecs_has(s->world, ent, JceCameraComponent))             flags |= JCE_COMP_FLAG_CAMERA;
    if (ecs_has(s->world, ent, JceDirectionalLight))            flags |= JCE_COMP_FLAG_DIR_LIGHT;
    if (ecs_has(s->world, ent, JcePointLight))                  flags |= JCE_COMP_FLAG_POINT_LIGHT;
    if (ecs_has(s->world, ent, JceSpotLight))                   flags |= JCE_COMP_FLAG_SPOT_LIGHT;
    if (ecs_has(s->world, ent, JceSkyboxComponent))             flags |= JCE_COMP_FLAG_SKYBOX;
    if (ecs_has(s->world, ent, JceSpriteRendererComponent))     flags |= JCE_COMP_FLAG_SPRITE_RENDERER;
    if (ecs_has(s->world, ent, JceSpriteAnimatorComponent))     flags |= JCE_COMP_FLAG_SPRITE_ANIMATOR;
    if (ecs_has(s->world, ent, JceAnimatorComponent))           flags |= JCE_COMP_FLAG_ANIMATOR;
    if (ecs_has(s->world, ent, JceSkeletalAnimatorComponent))   flags |= JCE_COMP_FLAG_SKELETAL_ANIMATOR;
    if (ecs_has(s->world, ent, JceConstraintComponent))         flags |= JCE_COMP_FLAG_CONSTRAINT;
    if (ecs_has(s->world, ent, JceRigidBodyComponent))          flags |= JCE_COMP_FLAG_RIGIDBODY;
    if (ecs_has(s->world, ent, JceRigidBody2DComponent))        flags |= JCE_COMP_FLAG_RIGIDBODY_2D;
    if (ecs_has(s->world, ent, JceBoxColliderComponent))        flags |= JCE_COMP_FLAG_BOX_COLLIDER;
    if (ecs_has(s->world, ent, JceSphereColliderComponent))     flags |= JCE_COMP_FLAG_SPHERE_COLLIDER;
    if (ecs_has(s->world, ent, JceCharacterControllerComponent))flags |= JCE_COMP_FLAG_CHARACTER_CONTROLLER;
    if (ecs_has(s->world, ent, JceAudioSourceComponent))        flags |= JCE_COMP_FLAG_AUDIO_SOURCE;
    if (ecs_has(s->world, ent, JceScriptComponent))             flags |= JCE_COMP_FLAG_SCRIPT;
    if (ecs_has(s->world, ent, JceParticleEmitterComponent))    flags |= JCE_COMP_FLAG_PARTICLE_EMITTER;
    if (ecs_has(s->world, ent, JceBehaviorTree))                flags |= JCE_COMP_FLAG_BEHAVIOR_TREE;
    if (ecs_has(s->world, ent, JceEditorMeta))                  flags |= JCE_COMP_FLAG_EDITOR_META;
    if (ecs_has(s->world, ent, JceTerrainComponent))            flags |= JCE_COMP_FLAG_TERRAIN;
    if (ecs_has(s->world, ent, JceLodGroupComponent))           flags |= JCE_COMP_FLAG_LOD_GROUP;
    if (ecs_has(s->world, ent, JceVirtualCameraComponent))      flags |= JCE_COMP_FLAG_VIRTUAL_CAMERA;
    if (ecs_has(s->world, ent, JceTriggerVolumeComponent))      flags |= JCE_COMP_FLAG_TRIGGER_VOLUME;
    if (ecs_has(s->world, ent, JceCapsuleColliderComponent))    flags |= JCE_COMP_FLAG_CAPSULE_COLLIDER;
    if (ecs_has(s->world, ent, JceMeshColliderComponent))       flags |= JCE_COMP_FLAG_MESH_COLLIDER;
    if (ecs_has(s->world, ent, JceCollider2DComponent))         flags |= JCE_COMP_FLAG_COLLIDER_2D;
    if (ecs_has(s->world, ent, JceTrailRendererComponent))      flags |= JCE_COMP_FLAG_TRAIL_RENDERER;
    if (ecs_has(s->world, ent, JceLineRendererComponent))       flags |= JCE_COMP_FLAG_LINE_RENDERER;
    if (ecs_has(s->world, ent, JceReflectionProbeComponent))    flags |= JCE_COMP_FLAG_REFLECTION_PROBE;
    if (ecs_has(s->world, ent, JceDecalComponent))              flags |= JCE_COMP_FLAG_DECAL;
    if (ecs_has(s->world, ent, JceLightProbeGroupComponent))    flags |= JCE_COMP_FLAG_LIGHT_PROBE_GROUP;
    if (ecs_has(s->world, ent, JceAudioListenerComponent))      flags |= JCE_COMP_FLAG_AUDIO_LISTENER;
    if (ecs_has(s->world, ent, JceAudioReverbZoneComponent))    flags |= JCE_COMP_FLAG_AUDIO_REVERB_ZONE;
    if (ecs_has(s->world, ent, JceAudioOcclusionComponent))     flags |= JCE_COMP_FLAG_AUDIO_OCCLUSION;
    if (ecs_has(s->world, ent, JceSpawnManagerComponent))       flags |= JCE_COMP_FLAG_SPAWN_MANAGER;
    if (ecs_has(s->world, ent, JceWeaponComponent))             flags |= JCE_COMP_FLAG_WEAPON;
    if (ecs_has(s->world, ent, JceSavePointComponent))          flags |= JCE_COMP_FLAG_SAVE_POINT;
    if (ecs_has(s->world, ent, JceWheelColliderComponent))      flags |= JCE_COMP_FLAG_WHEEL_COLLIDER;
    if (ecs_has(s->world, ent, JceConstantForceComponent))      flags |= JCE_COMP_FLAG_CONSTANT_FORCE;
    if (ecs_has(s->world, ent, JceConfigurableJointComponent))  flags |= JCE_COMP_FLAG_CONFIGURABLE_JOINT;
    if (ecs_has(s->world, ent, JceJoint2DComponent))            flags |= JCE_COMP_FLAG_JOINT_2D;
    if (ecs_has(s->world, ent, JceBillboardRendererComponent))  flags |= JCE_COMP_FLAG_BILLBOARD_RENDERER;
    if (ecs_has(s->world, ent, JceCanvasComponent))             flags |= JCE_COMP_FLAG_CANVAS;
    if (ecs_has(s->world, ent, JceCanvasGroupComponent))        flags |= JCE_COMP_FLAG_CANVAS_GROUP;
    if (ecs_has(s->world, ent, JceLayoutGroupComponent))        flags |= JCE_COMP_FLAG_LAYOUT_GROUP;
    if (ecs_has(s->world, ent, JceUIImageComponent))            flags |= JCE_COMP_FLAG_UI_IMAGE;
    if (ecs_has(s->world, ent, JceUITextComponent))             flags |= JCE_COMP_FLAG_UI_TEXT;
    if (ecs_has(s->world, ent, JceUIButtonComponent))           flags |= JCE_COMP_FLAG_UI_BUTTON;
    if (ecs_has(s->world, ent, JceAudioBusRouteComponent))      flags |= JCE_COMP_FLAG_AUDIO_BUS_ROUTE;
    if (ecs_has(s->world, ent, JceAnimationLayerStateComponent)) flags |= JCE_COMP_FLAG_ANIMATION_LAYER_STATE;
    if (ecs_has(s->world, ent, JceBlendShapeWeightsComponent))  flags |= JCE_COMP_FLAG_BLEND_SHAPE_WEIGHTS;

    return flags;
}

/* ── Iteration ─────────────────────────────────────────────────────── */

typedef struct {
    JceScene          *scene;
    JceEntityCallback  cb;
    void              *user_data;
} IterCtx;

static void entity_iter_cb(ecs_iter_t *it)
{
    IterCtx *ctx = (IterCtx *)it->ctx;
    for (int i = 0; i < it->count; i++) {
        ctx->cb(ctx->scene, (JceEntity)it->entities[i], ctx->user_data);
    }
}

void jce_scene_each_entity(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !cb) return;

    IterCtx ctx = { s, cb, user_data };

    ecs_query_t *q = ecs_query(s->world, {
        .terms = {{ .id = ecs_id(JceTransform) }},
    });

    ecs_iter_t it = ecs_query_iter(s->world, q);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; i++) {
            cb(s, (JceEntity)it.entities[i], user_data);
        }
    }

    ecs_query_fini(q);
}

/* ── Discovery (Unity-style GameObject.Find / FindWithTag) ────────── */

/* Resolve display name: EditorMeta.name if present and non-empty,
 * otherwise the flecs registered name. */
static const char *display_name(const JceScene *s, JceEntity e)
{
    JceEditorMeta *meta = jce_scene_get_editor_meta((JceScene *)s, e);
    if (meta && meta->name[0]) return meta->name;
    return jce_scene_entity_registered_name(s, e);
}

JceEntity jce_scene_find_by_name(const JceScene *s, const char *name)
{
    if (!s || !name) return JCE_ENTITY_INVALID;
    JceEntity found = JCE_ENTITY_INVALID;

    ecs_query_t *q = ecs_query(((JceScene *)s)->world, {
        .terms = {{ .id = ecs_id(JceTransform) }},
    });
    ecs_iter_t it = ecs_query_iter(((JceScene *)s)->world, q);
    while (ecs_query_next(&it) && !found) {
        for (int i = 0; i < it.count; i++) {
            JceEntity e = (JceEntity)it.entities[i];
            const char *dn = display_name(s, e);
            if (dn && strcmp(dn, name) == 0) { found = e; break; }
        }
    }
    ecs_query_fini(q);
    return found;
}

JceEntity jce_scene_find_by_tag(const JceScene *s, const char *tag)
{
    if (!s || !tag) return JCE_ENTITY_INVALID;
    JceEntity found = JCE_ENTITY_INVALID;

    ecs_query_t *q = ecs_query(((JceScene *)s)->world, {
        .terms = {{ .id = ecs_id(JceTransform) }},
    });
    ecs_iter_t it = ecs_query_iter(((JceScene *)s)->world, q);
    while (ecs_query_next(&it) && !found) {
        for (int i = 0; i < it.count; i++) {
            JceEntity e = (JceEntity)it.entities[i];
            JceEditorMeta *m = jce_scene_get_editor_meta((JceScene *)s, e);
            if (m && strcmp(m->tag, tag) == 0) { found = e; break; }
        }
    }
    ecs_query_fini(q);
    return found;
}

uint32_t jce_scene_find_all_by_tag(const JceScene *s, const char *tag,
                                   JceEntity *out, uint32_t max_out)
{
    if (!s || !tag || !out || max_out == 0) return 0;
    uint32_t n = 0;

    ecs_query_t *q = ecs_query(((JceScene *)s)->world, {
        .terms = {{ .id = ecs_id(JceTransform) }},
    });
    ecs_iter_t it = ecs_query_iter(((JceScene *)s)->world, q);
    while (ecs_query_next(&it) && n < max_out) {
        for (int i = 0; i < it.count && n < max_out; i++) {
            JceEntity e = (JceEntity)it.entities[i];
            JceEditorMeta *m = jce_scene_get_editor_meta((JceScene *)s, e);
            if (m && strcmp(m->tag, tag) == 0) out[n++] = e;
        }
    }
    ecs_query_fini(q);
    return n;
}

uint32_t jce_scene_find_all_with_components(const JceScene *s,
                                            uint64_t component_flags,
                                            JceEntity *out, uint32_t max_out)
{
    if (!s || !out || max_out == 0 || component_flags == 0) return 0;
    uint32_t n = 0;

    ecs_query_t *q = ecs_query(((JceScene *)s)->world, {
        .terms = {{ .id = ecs_id(JceTransform) }},
    });
    ecs_iter_t it = ecs_query_iter(((JceScene *)s)->world, q);
    while (ecs_query_next(&it) && n < max_out) {
        for (int i = 0; i < it.count && n < max_out; i++) {
            JceEntity e = (JceEntity)it.entities[i];
            uint64_t flags = jce_scene_get_component_flags(s, e);
            if ((flags & component_flags) == component_flags) out[n++] = e;
        }
    }
    ecs_query_fini(q);
    return n;
}

void *jce_scene_get_world(JceScene *s)
{
    return s ? s->world : NULL;
}

void jce_scene_update(JceScene *s, float dt)
{
    JCE_PROFILE_ZONE_N("Scene::Update");
    if (!s) { JCE_PROFILE_ZONE_END; return; }
    ecs_progress(s->world, dt);

#if defined(JCE_PROFILER_ENABLED)
    {
        const ecs_world_info_t *info = ecs_get_world_info(s->world);
        if (info) {
            JCE_PROFILE_PLOT_I("scene.entities", (int64_t)info->entity_count);
        }
    }
#endif

    JCE_PROFILE_ZONE_END;
}
