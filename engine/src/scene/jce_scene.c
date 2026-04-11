/*
 * jce_scene.c  ECS scene implementation (flecs backend).
 */

#include <jce/scene/jce_scene.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"

#include <flecs.h>
#include <string.h>

#define LOG_TAG "scene"

/* ── Component IDs (registered once per world) ─────────────────────── */

static ECS_COMPONENT_DECLARE(JceTransform);
static ECS_COMPONENT_DECLARE(JceMeshRenderer);
static ECS_COMPONENT_DECLARE(JceCameraComponent);
static ECS_COMPONENT_DECLARE(JceDirectionalLight);
static ECS_COMPONENT_DECLARE(JceTagActive);
static ECS_COMPONENT_DECLARE(JceRigidBodyComponent);
static ECS_COMPONENT_DECLARE(JceRigidBody2DComponent);
static ECS_COMPONENT_DECLARE(JceParticleEmitterComponent);
static ECS_COMPONENT_DECLARE(JceBehaviorTree);

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
    ECS_COMPONENT_DEFINE(s->world, JceTagActive);
    ECS_COMPONENT_DEFINE(s->world, JceRigidBodyComponent);
    ECS_COMPONENT_DEFINE(s->world, JceRigidBody2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceParticleEmitterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBehaviorTree);

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

/* ── Component setters / getters ───────────────────────────────────── */

void jce_scene_set_transform(JceScene *s, JceEntity e, const JceTransform *t)
{
    if (!s || !t) return;
    ecs_set_ptr(s->world, (ecs_entity_t)e, JceTransform, t);
}

JceTransform *jce_scene_get_transform(JceScene *s, JceEntity e)
{
    if (!s) return NULL;
    return (JceTransform *)ecs_get_mut(s->world, (ecs_entity_t)e, JceTransform);
}

void jce_scene_set_mesh_renderer(JceScene *s, JceEntity e, const JceMeshRenderer *mr)
{
    if (!s || !mr) return;
    ecs_set_ptr(s->world, (ecs_entity_t)e, JceMeshRenderer, mr);
}

JceMeshRenderer *jce_scene_get_mesh_renderer(JceScene *s, JceEntity e)
{
    if (!s) return NULL;
    return (JceMeshRenderer *)ecs_get_mut(s->world, (ecs_entity_t)e, JceMeshRenderer);
}

void jce_scene_set_camera(JceScene *s, JceEntity e, const JceCameraComponent *c)
{
    if (!s || !c) return;
    ecs_set_ptr(s->world, (ecs_entity_t)e, JceCameraComponent, c);
}

JceCameraComponent *jce_scene_get_camera(JceScene *s, JceEntity e)
{
    if (!s) return NULL;
    return (JceCameraComponent *)ecs_get_mut(s->world, (ecs_entity_t)e, JceCameraComponent);
}

void jce_scene_set_dir_light(JceScene *s, JceEntity e, const JceDirectionalLight *l)
{
    if (!s || !l) return;
    ecs_set_ptr(s->world, (ecs_entity_t)e, JceDirectionalLight, l);
}

JceDirectionalLight *jce_scene_get_dir_light(JceScene *s, JceEntity e)
{
    if (!s) return NULL;
    return (JceDirectionalLight *)ecs_get_mut(s->world, (ecs_entity_t)e, JceDirectionalLight);
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
    if (!s) return;
    ecs_progress(s->world, dt);
}
