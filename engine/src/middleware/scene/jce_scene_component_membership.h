/* Private ECS mutation bridge: table membership invalidates renderer metadata. */
#ifndef JCE_SCENE_COMPONENT_MEMBERSHIP_H
#define JCE_SCENE_COMPONENT_MEMBERSHIP_H

#include <jce/middleware/scene/jce_scene.h>
#include <flecs.h>

static inline void scene_component_set(JceScene *scene, ecs_world_t *world,
                                       ecs_entity_t entity, ecs_id_t type,
                                       size_t size, const void *value)
{
    const bool present = ecs_has_id(world, entity, type);
    ecs_set_id(world, entity, type, size, value);
    if (!present) jce_scene_invalidate_world_cache(scene);
}

static inline void scene_component_remove(JceScene *scene, ecs_world_t *world,
                                          ecs_entity_t entity, ecs_id_t type)
{
    if (!ecs_has_id(world, entity, type)) return;
    ecs_remove_id(world, entity, type);
    jce_scene_invalidate_world_cache(scene);
}

#define JCE_SCENE_COMPONENT_SET(s, e, TYPE, value) \
    scene_component_set((s), (s)->world, (e), ecs_id(TYPE), sizeof(TYPE), (value))
#define JCE_SCENE_COMPONENT_REMOVE(s, e, TYPE) \
    scene_component_remove((s), (s)->world, (e), ecs_id(TYPE))

#endif
