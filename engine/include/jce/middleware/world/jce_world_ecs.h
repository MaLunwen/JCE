/*
 * jce_world_ecs.h — flecs ECS adapter for jce_trigger_volume.
 *
 * Bridges generic trigger-volume + observer state into a flecs world so
 * gameplay code can express zones and tracked points as entities. The
 * adapter never registers systems; the host calls _tick() each frame.
 *
 * Components:
 *   JceTriggerVolumeEcs   — desc + handle (assigned on first tick)
 *   JceTriggerObserverEcs — position + handle (assigned on first tick)
 *
 * Per-frame ticks:
 *   jce_world_ecs_tick_triggers() — sync new components ↔ trigger world,
 *                                   push observer positions, then call
 *                                   jce_trigger_world_update().
 *
 * Removal: deleting an entity does NOT auto-remove the underlying
 * trigger/observer (flecs doesn't tell us cheaply). Game code should
 * call jce_trigger_remove / jce_observer_remove explicitly OR rely on
 * the trigger world being torn down with the level.
 *
 * Thread-safety: single-threaded (matches JceTriggerWorld).
 *
 * Example:
 *   ecs_world_t *w = ecs_init();
 *   JceWorldEcs *we = jce_world_ecs_create(w, trigger_world);
 *   ecs_entity_t e = ecs_new(w);
 *   ecs_set_ptr(w, e, JceTriggerVolumeEcs, &(JceTriggerVolumeEcs){
 *       .desc = { .shape = JCE_TRIGGER_SPHERE, .center = {0,0,0},
 *                 .half_extents = {5,0,0} },
 *       .user = 42,
 *   });
 *   for (;;) jce_world_ecs_tick_triggers(we);
 *
 * Layer: world.
 */
#ifndef JCE_WORLD_ECS_H
#define JCE_WORLD_ECS_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/middleware/world/jce_trigger_volume.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* The ECS world is passed as an opaque void* (from jce_scene_get_world());
 * the concrete flecs type must not appear in the public ABI. */
typedef struct JceWorldEcs JceWorldEcs;

typedef struct {
    JceTriggerDesc   desc;
    uint64_t         user;
    bool             enabled;
    JceTriggerHandle handle;   /* filled by tick on first sync; INVALID until then */
    bool             desc_dirty; /* set true to push desc updates to engine */
} JceTriggerVolumeEcs;

typedef struct {
    jce_vec3          position;
    uint64_t          user;
    JceObserverHandle handle;  /* filled by tick on first sync */
} JceTriggerObserverEcs;

JCE_API JceWorldEcs *jce_world_ecs_create(void *w, JceTriggerWorld *tw);
JCE_API void         jce_world_ecs_destroy(JceWorldEcs *we);

/* Sync new entities → engine handles, push observer positions and any
 * dirty trigger descs, then advance the trigger world by one frame. */
JCE_API void         jce_world_ecs_tick_triggers(JceWorldEcs *we);

JCE_EXTERN_C_END

#endif /* JCE_WORLD_ECS_H */
