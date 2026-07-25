/*
 * jce_world_ecs.c — implementation. See jce_world_ecs.h.
 */

#include <jce/middleware/world/jce_world_ecs.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include <flecs.h>

#define LOG_TAG "world_ecs"

ECS_COMPONENT_DECLARE(JceTriggerVolumeEcs);
ECS_COMPONENT_DECLARE(JceTriggerObserverEcs);

struct JceWorldEcs {
    ecs_world_t     *world;
    JceTriggerWorld *tw;
    ecs_query_t     *q_volumes;
    ecs_query_t     *q_observers;
};

JceWorldEcs *jce_world_ecs_create(void *w_opaque, JceTriggerWorld *tw)
{
    ecs_world_t *w = (ecs_world_t *)w_opaque;
    if (!w) return NULL;

    JceWorldEcs *we = (JceWorldEcs *)ecs_os_calloc(sizeof(*we));
    if (!we) return NULL;
    we->world = w;
    we->tw    = tw;

    ECS_COMPONENT_DEFINE(w, JceTriggerVolumeEcs);
    ECS_COMPONENT_DEFINE(w, JceTriggerObserverEcs);

    we->q_volumes = ecs_query(w, {
        .terms = {{ ecs_id(JceTriggerVolumeEcs) }}
    });
    we->q_observers = ecs_query(w, {
        .terms = {{ ecs_id(JceTriggerObserverEcs) }}
    });

    LOG_INFO(LOG_TAG, "world ECS adapter ready (tw=%s)",
                 tw ? "bound" : "null");
    return we;
}

void jce_world_ecs_destroy(JceWorldEcs *we)
{
    if (!we) return;
    if (we->q_volumes)   ecs_query_fini(we->q_volumes);
    if (we->q_observers) ecs_query_fini(we->q_observers);
    ecs_os_free(we);
}

void jce_world_ecs_tick_triggers(JceWorldEcs *we)
{
    if (!we || !we->tw) return;

    JCE_PROFILE_ZONE_N("world_ecs.tick_triggers");

    /* Sync trigger volumes (assign handles, push desc updates). */
    ecs_iter_t it = ecs_query_iter(we->world, we->q_volumes);
    while (ecs_query_next(&it)) {
        JceTriggerVolumeEcs *arr = ecs_field(&it, JceTriggerVolumeEcs, 0);
        for (int i = 0; i < it.count; ++i) {
            JceTriggerVolumeEcs *v = &arr[i];
            if (!jce_trigger_valid(v->handle)) {
                v->handle = jce_trigger_add(we->tw, &v->desc, v->user);
                jce_trigger_set_enabled(we->tw, v->handle, v->enabled);
                v->desc_dirty = false;
            } else if (v->desc_dirty) {
                jce_trigger_set_desc(we->tw, v->handle, &v->desc);
                v->desc_dirty = false;
            }
        }
    }

    /* Sync observers (assign handles, push positions). */
    it = ecs_query_iter(we->world, we->q_observers);
    while (ecs_query_next(&it)) {
        JceTriggerObserverEcs *arr = ecs_field(&it, JceTriggerObserverEcs, 0);
        for (int i = 0; i < it.count; ++i) {
            JceTriggerObserverEcs *o = &arr[i];
            if (!jce_observer_valid(o->handle)) {
                o->handle = jce_observer_add(we->tw, o->position, o->user);
            } else {
                jce_observer_set_position(we->tw, o->handle, o->position);
            }
        }
    }

    jce_trigger_world_update(we->tw);

    JCE_PROFILE_ZONE_END;
}
