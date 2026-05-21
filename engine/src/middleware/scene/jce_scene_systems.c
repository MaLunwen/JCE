/*
 * jce_scene_systems.c  Implementation of the system-introspection API
 *                      used by the editor's P3-B.5 Systems panel.
 *
 * Sources the active scene via the runtime game-module bridge
 * (`jce_game_module_active_scene`), grabs its flecs world through the
 * existing public accessor `jce_scene_get_world`, then walks every
 * (EcsSystem) instance and feeds a POD snapshot to the caller.
 */

#include <jce/middleware/scene/jce_scene_systems.h>
#include <jce/middleware/scene/jce_scene.h>

#include <flecs.h>

/* Forward decl — defined privately in jce_game_module.c (not part of
 * the public runtime header until the multi-scene story stabilizes). */
extern JceScene *jce_game_module_active_scene(void);

static ecs_world_t *active_world(void)
{
    JceScene *s = jce_game_module_active_scene();
    return s ? (ecs_world_t *)jce_scene_get_world(s) : NULL;
}

void JCE_CALL
jce_scene_iterate_systems(JceEcsSystemIterFn cb, void *user)
{
    if (!cb) return;
    ecs_world_t *world = active_world();
    if (!world) return;

    /* `ecs_query` with a single EcsSystem term enumerates every
     * registered system (built-in pipeline systems included).
     * EcsSystem is an entity tag, used directly as the term id. */
    ecs_query_t *q = ecs_query(world, {
        .terms = {{ .id = EcsSystem }},
    });
    if (!q) return;

    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; ++i) {
            const ecs_entity_t sys = it.entities[i];
            const ecs_system_t *sd = ecs_system_get(world, sys);

            JceEcsSystemInfo info;
            info.name = sd && sd->name ? sd->name
                                       : (ecs_get_name(world, sys)
                                              ? ecs_get_name(world, sys)
                                              : "<anonymous>");

            /* Phase = target of the (DependsOn, *) pair, if any. */
            const ecs_entity_t phase =
                ecs_get_target(world, sys, EcsDependsOn, 0);
            const char *phase_name =
                phase ? ecs_get_name(world, phase) : NULL;
            info.group = phase_name ? phase_name : "—";

            /* flecs populates time_spent only when stats collection is
             * enabled; otherwise the field stays 0 — still useful as a
             * "no data" marker for the panel. */
            info.last_ms = sd ? (double)sd->time_spent * 1000.0 : 0.0;

            info.matched_entities = 0u;
            if (sd && sd->query) {
                ecs_query_count_t c = ecs_query_count(sd->query);
                info.matched_entities = (uint64_t)c.entities;
            }

            info.enabled  = !ecs_has_id(world, sys, EcsDisabled);
            info.system_id = (uint64_t)sys;

            cb(&info, user);
        }
    }
    ecs_query_fini(q);
}

void JCE_CALL
jce_scene_set_system_enabled(uint64_t system_id, bool enabled)
{
    if (system_id == 0u) return;
    ecs_world_t *world = active_world();
    if (!world) return;
    ecs_enable(world, (ecs_entity_t)system_id, enabled);
}
