#ifndef JCE_SCENE_STRESS_INTERNAL_H
#define JCE_SCENE_STRESS_INTERNAL_H

#include <jce/middleware/scene/jce_scene_stress.h>
#include <flecs.h>

typedef struct JceSceneStressState {
    ecs_query_t *query;
    JceEntity root;
    float phase;
    bool enabled;
} JceSceneStressState;

JceSceneStressState *jce_scene_stress_state(JceScene *scene);
void jce_scene_stress_update(JceScene *scene, ecs_world_t *world,
                             JceSceneStressState *state, float dt,
                             ecs_entity_t transform, ecs_entity_t camera,
                             ecs_entity_t character);

#endif /* JCE_SCENE_STRESS_INTERNAL_H */
