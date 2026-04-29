/*
 * jce_ai_ecs.h -- flecs ECS adapter for AI middleware (steering + A*).
 *
 * Wraps:
 *   - jce_steering helpers — an entity-bound steering agent that the
 *     adapter integrates each tick with one of the built-in behaviors
 *     (seek / flee / arrive / wander / idle).  Composite behaviors
 *     remain available by calling the bare jce_steer_* helpers from
 *     game code; this adapter handles the common single-behavior case.
 *   - jce_graph_astar — request/response component pair so callers
 *     can submit pathfinding requests via ECS and read results back
 *     once the adapter processes them.
 *
 * The adapter does NOT own the JceGraphAstar context — caller passes
 * it in.  Steering is fully stateless; only requires the world.
 *
 * Generic — no game-specific types; works for crowds, vehicles,
 * fish, drones, anything with position+velocity in 3D.
 *
 * Thread-safety: a JceAiEcs handle is single-threaded.  The bound
 * JceGraphAstar context is shared, so all path requests are solved
 * sequentially in tick_paths().  For multi-thread pathfinding, give
 * each worker its own JceAiEcs+JceGraphAstar pair.
 *
 * Example:
 *   JceGraphAstar *astar = jce_graph_astar_create(8192);
 *   jce_graph_astar_set_callbacks(astar, my_neighbors, my_heuristic, ud);
 *   JceAiEcs *aiecs = jce_ai_ecs_create(world, astar);
 *
 *   // attach a wandering agent:
 *   JceSteerAgentEcs s = {0};
 *   s.position  = (jce_vec3){0,0,0};
 *   s.velocity  = (jce_vec3){0,0,0};
 *   s.max_speed = 5.0f;
 *   s.max_force = 8.0f;
 *   s.behavior  = JCE_STEER_BEHAVIOR_WANDER;
 *   s.wander_distance = 2.5f;
 *   s.wander_radius   = 1.5f;
 *   s.wander_angle_change_per_sec = 1.2f;
 *   ecs_set_ptr(world, e, JceSteerAgentEcs, &s);
 *
 *   // each frame:
 *   jce_ai_ecs_tick_steering(aiecs, dt);
 *   jce_ai_ecs_tick_paths(aiecs, 8);   // process up to 8 reqs/frame
 *
 *   jce_ai_ecs_destroy(aiecs);
 *   jce_graph_astar_destroy(astar);
 *
 * Layer: middleware (Layer 4) — public.
 */
#ifndef JCE_AI_ECS_H
#define JCE_AI_ECS_H

#include <jce/middleware/ai/jce_graph_astar.h>
#include <jce/middleware/ai/jce_steering.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct ecs_world_t ecs_world_t;

/* ------------------------------------------------------------------ *
 *  Steering agent component
 * ------------------------------------------------------------------ */

typedef enum {
    JCE_STEER_BEHAVIOR_IDLE   = 0,
    JCE_STEER_BEHAVIOR_SEEK,
    JCE_STEER_BEHAVIOR_FLEE,
    JCE_STEER_BEHAVIOR_ARRIVE,
    JCE_STEER_BEHAVIOR_WANDER
} JceSteerBehavior;

typedef struct JceSteerAgentEcs {
    /* state */
    jce_vec3 position;
    jce_vec3 velocity;
    /* limits */
    float    max_speed;
    float    max_force;
    /* behavior config */
    JceSteerBehavior behavior;
    jce_vec3 target;            /* used by SEEK/FLEE/ARRIVE */
    float    arrive_slowing_radius;
    /* wander state (used by WANDER) */
    JceSteerWander wander_state;
    float    wander_distance;
    float    wander_radius;
    float    wander_angle_change_per_sec;
} JceSteerAgentEcs;

/* ------------------------------------------------------------------ *
 *  A* path request / result component
 * ------------------------------------------------------------------ */

typedef enum {
    JCE_PATH_STATUS_PENDING = 0,
    JCE_PATH_STATUS_FOUND,
    JCE_PATH_STATUS_UNREACHABLE,
    JCE_PATH_STATUS_OVERFLOW
} JcePathStatus;

#define JCE_PATH_RESULT_MAX 256

typedef struct JcePathRequestEcs {
    uint32_t      start;
    uint32_t      goal;
    JcePathStatus status;
    uint32_t      path[JCE_PATH_RESULT_MAX];
    uint32_t      path_count;
} JcePathRequestEcs;

/* ------------------------------------------------------------------ *
 *  Lifetime
 * ------------------------------------------------------------------ */

typedef struct JceAiEcs JceAiEcs;

/* `astar` may be NULL — tick_paths becomes a no-op. */
JCE_API JceAiEcs *jce_ai_ecs_create(ecs_world_t *world, JceGraphAstar *astar);
JCE_API void JCE_CALL jce_ai_ecs_destroy(JceAiEcs *a);

/* ------------------------------------------------------------------ *
 *  Per-frame ticks
 * ------------------------------------------------------------------ */

/* Integrate every JceSteerAgentEcs once: compute steering force per
 * configured behavior, clamp, integrate velocity & position. */
JCE_API void JCE_CALL jce_ai_ecs_tick_steering(JceAiEcs *a, float dt);

/* Process up to `max_per_frame` PENDING path requests.  Sets status
 * to FOUND/UNREACHABLE/OVERFLOW and writes path[]/path_count. */
JCE_API void JCE_CALL jce_ai_ecs_tick_paths(JceAiEcs *a, uint32_t max_per_frame);

JCE_EXTERN_C_END

#endif /* JCE_AI_ECS_H */
