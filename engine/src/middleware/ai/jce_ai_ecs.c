/*
 * jce_ai_ecs.c -- flecs ECS adapter for steering + A* pathfinding.
 */

#include <jce/middleware/ai/jce_ai_ecs.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <flecs.h>
#include <math.h>
#include <string.h>

#define LOG_TAG "ai_ecs"

ECS_COMPONENT_DECLARE(JceSteerAgentEcs);
ECS_COMPONENT_DECLARE(JcePathRequestEcs);

struct JceAiEcs {
    ecs_world_t   *world;
    JceGraphAstar *astar;
    ecs_query_t   *q_steer;
    ecs_query_t   *q_path;
};

JceAiEcs *jce_ai_ecs_create(ecs_world_t *world, JceGraphAstar *astar)
{
    if (!world) return NULL;
    JceAiEcs *a = (JceAiEcs *)JCE_CALLOC(1, sizeof(*a));
    if (!a) return NULL;
    a->world = world;
    a->astar = astar;

    ECS_COMPONENT_DEFINE(world, JceSteerAgentEcs);
    ECS_COMPONENT_DEFINE(world, JcePathRequestEcs);

    a->q_steer = ecs_query(world, {
        .terms = {{ ecs_id(JceSteerAgentEcs) }}
    });
    a->q_path = ecs_query(world, {
        .terms = {{ ecs_id(JcePathRequestEcs) }}
    });
    LOG_SUCCESS(LOG_TAG, "AI ECS adapter created (astar=%p)", (void*)astar);
    return a;
}

void jce_ai_ecs_destroy(JceAiEcs *a)
{
    if (!a) return;
    if (a->q_steer) ecs_query_fini(a->q_steer);
    if (a->q_path)  ecs_query_fini(a->q_path);
    JCE_FREE(a);
}

/* ── Steering tick ─────────────────────────────────────────────────── */

static jce_vec3 v3_clamp_len(jce_vec3 v, float max_len)
{
    float l2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (l2 <= max_len * max_len) return v;
    float inv = max_len / sqrtf(l2);
    jce_vec3 r = { v.x * inv, v.y * inv, v.z * inv };
    return r;
}

void jce_ai_ecs_tick_steering(JceAiEcs *a, float dt)
{
    if (!a || !a->q_steer || dt <= 0.0f) return;
    JCE_PROFILE_ZONE_N("AiECS::tick_steering");

    ecs_iter_t it = ecs_query_iter(a->world, a->q_steer);
    while (ecs_query_next(&it)) {
        JceSteerAgentEcs *as = ecs_field(&it, JceSteerAgentEcs, 0);
        if (!as) continue;
        for (int i = 0; i < it.count; ++i) {
            JceSteerAgentEcs *g = &as[i];
            jce_vec3 force = {0,0,0};
            switch (g->behavior) {
            case JCE_STEER_BEHAVIOR_SEEK:
                force = jce_steer_seek(g->position, g->velocity,
                                       g->target, g->max_speed);
                break;
            case JCE_STEER_BEHAVIOR_FLEE:
                force = jce_steer_flee(g->position, g->velocity,
                                       g->target, g->max_speed);
                break;
            case JCE_STEER_BEHAVIOR_ARRIVE:
                force = jce_steer_arrive(g->position, g->velocity,
                                         g->target, g->max_speed,
                                         g->arrive_slowing_radius);
                break;
            case JCE_STEER_BEHAVIOR_WANDER:
                force = jce_steer_wander(g->position, g->velocity,
                                         &g->wander_state,
                                         g->wander_distance,
                                         g->wander_radius,
                                         g->wander_angle_change_per_sec,
                                         dt, g->max_speed);
                break;
            case JCE_STEER_BEHAVIOR_IDLE:
            default:
                continue;
            }
            force = v3_clamp_len(force, g->max_force);
            g->velocity.x += force.x * dt;
            g->velocity.y += force.y * dt;
            g->velocity.z += force.z * dt;
            g->velocity = jce_steer_truncate(g->velocity, g->max_speed);
            g->position.x += g->velocity.x * dt;
            g->position.y += g->velocity.y * dt;
            g->position.z += g->velocity.z * dt;
        }
    }
    JCE_PROFILE_ZONE_END;
}

/* ── Path-request tick ─────────────────────────────────────────────── */

void jce_ai_ecs_tick_paths(JceAiEcs *a, uint32_t max_per_frame)
{
    if (!a || !a->astar || !a->q_path || max_per_frame == 0) return;
    JCE_PROFILE_ZONE_N("AiECS::tick_paths");

    uint32_t budget = max_per_frame;
    ecs_iter_t it = ecs_query_iter(a->world, a->q_path);
    while (ecs_query_next(&it) && budget > 0) {
        JcePathRequestEcs *rs = ecs_field(&it, JcePathRequestEcs, 0);
        if (!rs) continue;
        for (int i = 0; i < it.count && budget > 0; ++i) {
            JcePathRequestEcs *r = &rs[i];
            if (r->status != JCE_PATH_STATUS_PENDING) continue;
            uint32_t count = 0;
            bool ok = jce_graph_astar_search(a->astar, r->start, r->goal,
                                             r->path, JCE_PATH_RESULT_MAX,
                                             &count);
            if (ok) {
                r->path_count = count;
                r->status     = JCE_PATH_STATUS_FOUND;
            } else if (count > JCE_PATH_RESULT_MAX) {
                r->path_count = 0;
                r->status     = JCE_PATH_STATUS_OVERFLOW;
            } else {
                r->path_count = 0;
                r->status     = JCE_PATH_STATUS_UNREACHABLE;
            }
            --budget;
        }
    }
    JCE_PROFILE_ZONE_END;
}
