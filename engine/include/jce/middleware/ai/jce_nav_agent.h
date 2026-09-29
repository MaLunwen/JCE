/*
 * jce_nav_agent.h -- NavMesh agent + steering runtime.
 *
 * Lightweight per-agent state machine layered on top of jce_navmesh.
 * Each agent owns: a position, velocity, max-speed/accel/radius, a
 * destination, and a cached path of waypoints obtained from the
 * underlying NavMesh.  jce_nav_agent_update() advances the agent
 * along its path using a seek-and-arrive steering rule.
 *
 * Agents are pure XZ-plane actors — Y is ignored (callers may sample
 * terrain height separately).  No obstacle avoidance yet; pure path
 * following.  RVO/avoidance is a future module.
 *
 * Layer: AI / Navigation (Layer 3) — public.
 */

#ifndef JCE_NAV_AGENT_H
#define JCE_NAV_AGENT_H

#include <jce/middleware/ai/jce_navmesh.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceNavAgentSet JceNavAgentSet;

typedef struct { uint32_t idx; } JceNavAgentHandle;
#define JCE_NAV_AGENT_INVALID ((JceNavAgentHandle){ UINT32_MAX })

static inline bool jce_nav_agent_valid(JceNavAgentHandle h) {
    return h.idx != UINT32_MAX;
}

typedef struct {
    float pos_x, pos_z;        /* world-space spawn */
    float radius;              /* agent radius (m) */
    float max_speed;           /* m/s */
    float max_accel;           /* m/s² (acceleration cap) */
    float arrive_radius;       /* slow-down radius around final goal */
    float waypoint_radius;     /* distance to consider a waypoint reached */
    /* APPENDED.  The agent's standing height in metres, compared against the
     * clearance the navmesh was carved for (jce_navmesh_agent_height).
     *
     * <= 0 means "do not check", which is what every desc written before this
     * field existed holds -- so no existing agent starts being refused.
     *
     * The steering itself is 2D and stays 2D: this does not make an agent
     * duck.  What it does is catch the case a 2D steerer cannot see at all --
     * an agent on a mesh built for someone shorter, walking under geometry it
     * does not fit under, with nothing about the motion looking wrong. */
    float height;
} JceNavAgentDesc;

typedef enum {
    JCE_NAV_AGENT_IDLE      = 0,  /* no destination set */
    JCE_NAV_AGENT_MOVING    = 1,  /* path active, following waypoints */
    JCE_NAV_AGENT_ARRIVED   = 2,  /* reached destination this frame */
    JCE_NAV_AGENT_NO_PATH   = 3   /* destination set but unreachable */
} JceNavAgentStatus;

/* -- Agent set (collection that shares a navmesh) ----------------- */

JCE_API JceNavAgentSet *jce_nav_agent_set_create(JceNavMesh *navmesh,
                                                  uint32_t max_agents);
JCE_API void            jce_nav_agent_set_destroy(JceNavAgentSet *set);

/* Replace the bound navmesh (e.g. when streaming swaps a region).
 * Existing agents keep their position but their cached paths are
 * invalidated and re-computed on the next destination set. */
JCE_API void jce_nav_agent_set_navmesh(JceNavAgentSet *set, JceNavMesh *nm);

/* -- Agent lifecycle --------------------------------------------- */

/* Does this agent fit the mesh it is about to walk?
 *
 * false only when BOTH numbers are known and the agent is taller.  An unknown
 * clearance (a mesh baked before it was recorded) and an unset agent height
 * both mean "no opinion", because a check that fires on missing data rejects
 * every scene authored before it existed. */
JCE_API bool jce_nav_agent_fits(float agent_height, float mesh_clearance);

/* jce_nav_agent_fits, and SAY SO once if it does not.
 *
 * Reported rather than refused, deliberately: refusing would strand the agent
 * where it stands, which is a worse failure than a warning, and the authored
 * intent is often a mesh that simply has not been re-baked yet.
 *
 * One function because there are two navmesh backends -- the grid mesh the
 * agent set can hold, and the Detour mesh the runtime drives through a path
 * function -- and two copies of this message is how they end up saying
 * different things about the same condition.  Returns what fits() returned. */
JCE_API bool jce_nav_agent_report_fit(float agent_height, float mesh_clearance);

JCE_API JceNavAgentHandle jce_nav_agent_add(JceNavAgentSet *set,
                                              const JceNavAgentDesc *desc);
JCE_API void              jce_nav_agent_remove(JceNavAgentSet *set,
                                                JceNavAgentHandle a);

/* Set / clear destination.  Setting a destination triggers an
 * immediate path-find via jce_navmesh_find_path. */
JCE_API bool jce_nav_agent_set_destination(JceNavAgentSet *set,
                                            JceNavAgentHandle a,
                                            float wx, float wz);
JCE_API void jce_nav_agent_stop(JceNavAgentSet *set, JceNavAgentHandle a);

/* -- Per-frame update -------------------------------------------- */

/* Step every active agent by dt seconds.  Internally:
 *   1. seek next waypoint (arrive behaviour near final goal),
 *   2. clamp acceleration to max_accel,
 *   3. integrate position by velocity,
 *   4. advance to next waypoint when within waypoint_radius. */
JCE_API void jce_nav_agent_set_update(JceNavAgentSet *set, float dt);

/* -- Per-agent queries ------------------------------------------- */

JCE_API void              jce_nav_agent_get_position(const JceNavAgentSet *set,
                                                      JceNavAgentHandle a,
                                                      float *out_x, float *out_z);
JCE_API void              jce_nav_agent_get_velocity(const JceNavAgentSet *set,
                                                      JceNavAgentHandle a,
                                                      float *out_vx, float *out_vz);
JCE_API JceNavAgentStatus jce_nav_agent_get_status  (const JceNavAgentSet *set,
                                                      JceNavAgentHandle a);

/* Number of remaining waypoints (including current target). */
JCE_API uint32_t jce_nav_agent_path_length(const JceNavAgentSet *set,
                                            JceNavAgentHandle a);

/* Force-teleport an agent (e.g. respawn).  Clears any active path. */
JCE_API void jce_nav_agent_warp(JceNavAgentSet *set, JceNavAgentHandle a,
                                  float wx, float wz);

/* Total number of live agents (for debug/profiler). */
JCE_API uint32_t jce_nav_agent_set_count(const JceNavAgentSet *set);

/* -- Crowd avoidance (sampling-RVO) ------------------------------ */

/*
 * Reciprocal velocity-obstacle avoidance for crowds.  When enabled,
 * each agent samples N candidate velocities around its desired
 * velocity each frame and picks the one that minimises a cost of
 * (deviation from desired) + (collision penalty against neighbours).
 * Neighbours are gathered with an O(N²) scan up to `sight_radius`,
 * fine for hundreds of agents — replace with a grid index for
 * thousands.
 *
 * Disabled by default to preserve deterministic single-agent path
 * follow.  When enabled, set time_horizon to roughly the time the
 * agent should plan ahead (1.5–3.0 s typical) and num_samples to
 * 8–32 (more = smoother but more expensive).
 */
typedef struct {
    bool     enabled;
    float    sight_radius;       /* m — neighbours within this contribute */
    float    time_horizon;       /* s — collision lookahead window */
    uint32_t num_samples;        /* candidate velocities per agent (≥ 1) */
    float    overlap_push_gain;  /* depen. push factor when overlapping
                                    (0 = off; 4-8 typical) */
} JceNavAvoidanceConfig;

JCE_API void jce_nav_agent_set_avoidance(JceNavAgentSet *set,
                                          const JceNavAvoidanceConfig *cfg);

/* -- Pluggable path-find backend --------------------------------- *
 *
 * By default an agent set finds paths via jce_navmesh_find_path on
 * the bound JceNavMesh (grid + A*).  To plug in an alternative
 * backend (e.g. the Recast/Detour navmesh in jce_navmesh_recast.h)
 * register a callback here.  The callback is invoked on each
 * jce_nav_agent_set_destination() and receives start/goal in world
 * XZ; it must write at most `max_pairs` (x,z) pairs into out_xz and
 * return the number of waypoints written (0 = no path).
 *
 * Setting fn = NULL restores the default JceNavMesh-based backend.
 */
typedef int (*JceNavAgentPathFn)(void *user,
                                  float start_x, float start_z,
                                  float goal_x,  float goal_z,
                                  float *out_xz, int max_pairs);

JCE_API void jce_nav_agent_set_path_fn(JceNavAgentSet *set,
                                        JceNavAgentPathFn fn,
                                        void *user);

JCE_EXTERN_C_END

#endif /* JCE_NAV_AGENT_H */
