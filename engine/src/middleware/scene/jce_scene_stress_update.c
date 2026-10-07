#include "jce_scene_stress_internal.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <math.h>
#include <stdlib.h>

void jce_scene_stress_motion(JceScene *scene, JceEntity root, bool enabled)
{
    JceSceneStressState *state = jce_scene_stress_state(scene);
    if (!state) return;
    state->enabled = enabled && root != JCE_ENTITY_INVALID &&
                     jce_scene_has_transform(scene, root);
    state->root = state->enabled ? root : JCE_ENTITY_INVALID;
    state->phase = 0;
}

static bool stress_descendant(JceScene *scene, JceEntity entity, JceEntity root)
{
    if (entity == root) return false;
    int depth;
    for (depth = 0; depth < 64 && entity != JCE_ENTITY_INVALID; ++depth) {
        JceEntity parent = jce_scene_get_parent(scene, entity);
        if (parent == root) return true;
        if (parent == entity) return false;
        entity = parent;
    }
    return false;
}

void jce_scene_stress_update(JceScene *scene, ecs_world_t *world,
                             JceSceneStressState *state, float dt,
                             ecs_entity_t transform, ecs_entity_t camera,
                             ecs_entity_t character)
{
    static int env_spin = -1;
    if (env_spin < 0) {
        const char *v = getenv("JCE_STRESS_SPIN");
        env_spin = v && v[0] && v[0] != '0';
    }
    if ((!env_spin && !state->enabled) || !isfinite(dt) || dt <= 0) return;
    const uint64_t start = jce_time_perf_counter();
    if (!state->query) {
        state->query = ecs_query(world, {
            .terms = {
                { .id = transform },
                { .id = camera, .oper = EcsNot },
                { .id = character, .oper = EcsNot },
            }
        });
    }
    if (state->query) {
        state->phase = fmodf(state->phase + dt, 6.2831853f);
        const float vx = sinf(state->phase), vy = cosf(state->phase);
        const jce_quat spin = jce_q_from_euler(0, dt, 0);
        ecs_iter_t it = ecs_query_iter(world, state->query);
        while (ecs_query_next(&it)) {
            JceTransform *xf = ecs_field(&it, JceTransform, 0);
            int i;
            for (i = 0; i < it.count; ++i) {
                const JceEntity e = (JceEntity)it.entities[i];
                if (state->enabled && !stress_descendant(scene, e, state->root))
                    continue;
                xf[i].position.x += vx * dt;
                xf[i].position.y += vy * dt * 0.5f;
                xf[i].rotation = jce_q_normalize(jce_q_multiply(spin, xf[i].rotation));
                /* Motion is a transform edit, not a structural edit. This also
                 * makes the env-driven workload safe with a real renderer. */
                jce_scene_invalidate_entity_world(scene, e);
            }
        }
    }
    jce_perf_phase_add("ecs_move", jce_time_perf_to_ms(start, jce_time_perf_counter()));
}
