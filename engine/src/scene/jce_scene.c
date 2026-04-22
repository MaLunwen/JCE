/*
 * jce_scene.c  ECS scene implementation (flecs backend).
 */

#include <jce/scene/jce_scene.h>
#include <jce/core/jce_log.h>
#include <jce/core/jce_profiler.h>
#include "core/jce_memory.h"

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
    t.position = (jce_vec3){{0, 0, 0}};
    t.rotation = jce_q_identity();
    t.scale    = (jce_vec3){{1, 1, 1}};
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

#undef JCE_COMP_IMPL

/* ── Component enumeration ─────────────────────────────────────────── */

uint32_t jce_scene_get_component_flags(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return 0;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return 0;
    uint32_t flags = 0;

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

void *jce_scene_get_world(JceScene *s)
{
    return s ? s->world : NULL;
}

void jce_scene_update(JceScene *s, float dt)
{
    JCE_PROFILE_ZONE_N("Scene::Update");
    if (!s) { JCE_PROFILE_ZONE_END; return; }
    ecs_progress(s->world, dt);
    JCE_PROFILE_ZONE_END;
}
