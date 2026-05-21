/*
 * jce_scene_systems.h  Introspection of flecs ECS systems (P3-B.5).
 *
 * Lets the editor's Systems panel enumerate every system registered in
 * the *active* scene (as published via the game-module bridge), without
 * pulling <flecs.h> into editor code (per AGENTS.md: "flecs stays
 * private to scene impl").
 *
 * Layer: L4 (middleware/scene).  Consumed via <jce/api_scene.h>.
 *
 * Threading: main thread only.  The struct passed to the callback is a
 * snapshot — do not retain pointers past callback return.
 */

#ifndef JCE_SCENE_SYSTEMS_H
#define JCE_SCENE_SYSTEMS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceEcsSystemInfo {
    const char *name;              /* system display name (never NULL)        */
    const char *group;             /* phase / group e.g. "EcsOnUpdate"        */
    double      last_ms;           /* flecs time_spent in milliseconds        */
    uint64_t    matched_entities;  /* result of ecs_query_count(.entities)    */
    bool        enabled;           /* false if EcsDisabled tag present        */
    uint64_t    system_id;         /* opaque ecs_entity_t                     */
} JceEcsSystemInfo;

typedef void (*JceEcsSystemIterFn)(const JceEcsSystemInfo *info, void *user);

/* Iterate every system in the active scene's flecs world.  No-op when
 * no scene is active. */
JCE_API void JCE_CALL
jce_scene_iterate_systems(JceEcsSystemIterFn cb, void *user);

/* Toggle a system at runtime.  Wraps `ecs_enable` on the underlying
 * flecs entity.  No-op when no scene is active or `system_id` is 0. */
JCE_API void JCE_CALL
jce_scene_set_system_enabled(uint64_t system_id, bool enabled);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_SYSTEMS_H */
