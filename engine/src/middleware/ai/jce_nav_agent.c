/*
 * jce_nav_agent.c -- NavMesh agent + steering implementation.
 *
 * SoA pool of agents.  Each set holds a single shared NavMesh; when
 * the navmesh is swapped (region streaming), all agents are nudged
 * back into IDLE and must be re-issued a destination by the caller.
 */

#include <jce/middleware/ai/jce_nav_agent.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "nav-agent"

/* Per-agent waypoint cache.  16 hops covers ≈ 100 m at 6 m cells —
 * enough for street-level NPC routing.  Long routes get re-planned
 * once the agent gets close to its current goal. */
#define JCE_NAV_AGENT_MAX_WAYPOINTS 32

typedef struct {
    /* Static config. */
    float radius;
    float max_speed;
    float max_accel;
    float arrive_radius;
    float waypoint_radius;

    /* Kinematics (XZ only). */
    float pos_x, pos_z;
    float vel_x, vel_z;

    /* Desired velocity computed by the path-following pass; consumed
     * by the optional avoidance pass and then by integration. */
    float desired_vx, desired_vz;

    /* Final destination (last waypoint). */
    float goal_x, goal_z;

    /* Path cache (XZ pairs).  Path[0..n-1] are remaining waypoints;
     * path[0] is the active target. */
    float    path[JCE_NAV_AGENT_MAX_WAYPOINTS * 2];
    uint32_t path_len;
    uint32_t path_cursor;          /* index of current target waypoint */

    JceNavAgentStatus status;
    bool              alive;
} Agent;

struct JceNavAgentSet {
    JceNavMesh *navmesh;
    Agent      *agents;
    uint32_t    capacity;
    uint32_t    count;

    JceNavAvoidanceConfig avoidance;

    JceNavAgentPathFn path_fn;
    void             *path_user;
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static inline float v2_len(float x, float z) { return sqrtf(x * x + z * z); }

static bool find_free_slot(JceNavAgentSet *s, uint32_t *out_idx)
{
    for (uint32_t i = 0; i < s->capacity; ++i) {
        if (!s->agents[i].alive) { *out_idx = i; return true; }
    }
    return false;
}

static bool replan_path(JceNavAgentSet *s, Agent *a)
{
    int n = 0;
    if (s->path_fn) {
        n = s->path_fn(s->path_user,
                       a->pos_x, a->pos_z,
                       a->goal_x, a->goal_z,
                       a->path, JCE_NAV_AGENT_MAX_WAYPOINTS);
    } else {
        if (!s->navmesh) {
            a->status = JCE_NAV_AGENT_NO_PATH;
            a->path_len = 0;
            return false;
        }
        n = jce_navmesh_find_path(s->navmesh,
                                   a->pos_x, a->pos_z,
                                   a->goal_x, a->goal_z,
                                   a->path, JCE_NAV_AGENT_MAX_WAYPOINTS);
    }
    if (n <= 0) {
        a->path_len = 0;
        a->status   = JCE_NAV_AGENT_NO_PATH;
        return false;
    }
    a->path_len    = (uint32_t)n;
    a->path_cursor = 0;
    a->status      = JCE_NAV_AGENT_MOVING;
    return true;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

JceNavAgentSet *jce_nav_agent_set_create(JceNavMesh *navmesh,
                                          uint32_t max_agents)
{
    if (max_agents == 0) max_agents = 64;
    JceNavAgentSet *s = (JceNavAgentSet *)JCE_CALLOC(1, sizeof(JceNavAgentSet));
    if (!s) return NULL;
    s->agents = (Agent *)JCE_CALLOC(max_agents, sizeof(Agent));
    if (!s->agents) { JCE_FREE(s); return NULL; }
    s->capacity = max_agents;
    s->navmesh  = navmesh;
    return s;
}

void jce_nav_agent_set_destroy(JceNavAgentSet *s)
{
    if (!s) return;
    JCE_FREE(s->agents);
    JCE_FREE(s);
}

void jce_nav_agent_set_navmesh(JceNavAgentSet *s, JceNavMesh *nm)
{
    if (!s) return;
    s->navmesh = nm;
    /* Invalidate every cached path — cell coordinates may differ. */
    for (uint32_t i = 0; i < s->capacity; ++i) {
        if (!s->agents[i].alive) continue;
        s->agents[i].path_len    = 0;
        s->agents[i].path_cursor = 0;
        s->agents[i].vel_x = s->agents[i].vel_z = 0.0f;
        s->agents[i].status      = JCE_NAV_AGENT_IDLE;
    }
}

bool jce_nav_agent_report_fit(float agent_height, float mesh_clearance)
{
    if (jce_nav_agent_fits(agent_height, mesh_clearance)) return true;
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN(LOG_TAG,
                 "nav: agent height %.2f m exceeds the %.2f m clearance this "
                 "navmesh was baked for; it will walk under geometry it does "
                 "not fit under", (double)agent_height,
                 (double)mesh_clearance);
    }
    return false;
}

/* An agent joining a set that holds a GRID mesh is checked here; the Recast
 * path is checked by the runtime, which is the side that holds that mesh.
 * Both go through the one report above. */
static void nav_check_fit(const JceNavAgentSet *set, const JceNavAgentDesc *d)
{
    if (!set || !set->navmesh || !d) return;
    (void)jce_nav_agent_report_fit(d->height,
                                   jce_navmesh_agent_height(set->navmesh));
}

bool jce_nav_agent_fits(float agent_height, float mesh_clearance)
{
    if (agent_height  <= 0.0f) return true;   /* the agent has no opinion */
    if (mesh_clearance <= 0.0f) return true;  /* the mesh has none either */
    return agent_height <= mesh_clearance;
}

JceNavAgentHandle jce_nav_agent_add(JceNavAgentSet *s,
                                      const JceNavAgentDesc *d)
{
    if (!s || !d) return JCE_NAV_AGENT_INVALID;
    uint32_t idx = 0;
    if (!find_free_slot(s, &idx)) return JCE_NAV_AGENT_INVALID;

    /* Before the agent joins, not after: the point is to say WHY it will walk
     * through things, and a message that arrives once it is already steering
     * reads as a symptom rather than the cause. */
    nav_check_fit(s, d);

    Agent *a = &s->agents[idx];
    memset(a, 0, sizeof(*a));
    a->radius           = d->radius          > 0 ? d->radius          : 0.4f;
    a->max_speed        = d->max_speed       > 0 ? d->max_speed       : 3.0f;
    a->max_accel        = d->max_accel       > 0 ? d->max_accel       : 12.0f;
    a->arrive_radius    = d->arrive_radius   > 0 ? d->arrive_radius   : 1.5f;
    a->waypoint_radius  = d->waypoint_radius > 0 ? d->waypoint_radius : 0.5f;
    a->pos_x = d->pos_x;
    a->pos_z = d->pos_z;
    a->status = JCE_NAV_AGENT_IDLE;
    a->alive  = true;
    s->count++;
    return (JceNavAgentHandle){ idx };
}

void jce_nav_agent_remove(JceNavAgentSet *s, JceNavAgentHandle h)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return;
    if (!s->agents[h.idx].alive) return;
    s->agents[h.idx].alive = false;
    s->count--;
}

/* ── Destination ─────────────────────────────────────────────────── */

bool jce_nav_agent_set_destination(JceNavAgentSet *s, JceNavAgentHandle h,
                                     float wx, float wz)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return false;
    Agent *a = &s->agents[h.idx];
    if (!a->alive) return false;
    a->goal_x = wx;
    a->goal_z = wz;
    return replan_path(s, a);
}

void jce_nav_agent_stop(JceNavAgentSet *s, JceNavAgentHandle h)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return;
    Agent *a = &s->agents[h.idx];
    if (!a->alive) return;
    a->path_len = 0;
    a->path_cursor = 0;
    a->vel_x = a->vel_z = 0.0f;
    a->status = JCE_NAV_AGENT_IDLE;
}

void jce_nav_agent_warp(JceNavAgentSet *s, JceNavAgentHandle h,
                          float wx, float wz)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return;
    Agent *a = &s->agents[h.idx];
    if (!a->alive) return;
    a->pos_x = wx;
    a->pos_z = wz;
    a->vel_x = a->vel_z = 0.0f;
    a->path_len = 0;
    a->path_cursor = 0;
    a->status = JCE_NAV_AGENT_IDLE;
}

/* ── Steering update ─────────────────────────────────────────────── */

/* Compute desired velocity from path follow + arrive.  Stores result
 * in a->desired_vx/vz.  May advance the path cursor and transition
 * the agent into ARRIVED. */
static void compute_desired_velocity(Agent *a)
{
    a->desired_vx = 0.0f;
    a->desired_vz = 0.0f;

    if (a->status != JCE_NAV_AGENT_MOVING || a->path_cursor >= a->path_len)
        return;

    float wp_x = a->path[a->path_cursor * 2 + 0];
    float wp_z = a->path[a->path_cursor * 2 + 1];

    float to_wp_x = wp_x - a->pos_x;
    float to_wp_z = wp_z - a->pos_z;
    float dist_wp = v2_len(to_wp_x, to_wp_z);

    if (dist_wp <= a->waypoint_radius) {
        a->path_cursor++;
        if (a->path_cursor >= a->path_len) {
            a->status = JCE_NAV_AGENT_ARRIVED;
            return;
        }
        wp_x = a->path[a->path_cursor * 2 + 0];
        wp_z = a->path[a->path_cursor * 2 + 1];
        to_wp_x = wp_x - a->pos_x;
        to_wp_z = wp_z - a->pos_z;
        dist_wp = v2_len(to_wp_x, to_wp_z);
    }

    float desired_speed = a->max_speed;
    bool is_final = (a->path_cursor + 1 == a->path_len);
    if (is_final) {
        float to_goal_x = a->goal_x - a->pos_x;
        float to_goal_z = a->goal_z - a->pos_z;
        float dist_goal = v2_len(to_goal_x, to_goal_z);
        if (dist_goal < a->arrive_radius && a->arrive_radius > 1e-4f) {
            desired_speed = a->max_speed * (dist_goal / a->arrive_radius);
        }
    }

    if (dist_wp > 1e-4f) {
        a->desired_vx = (to_wp_x / dist_wp) * desired_speed;
        a->desired_vz = (to_wp_z / dist_wp) * desired_speed;
    }
}

/* Cost of velocity v for agent self against neighbours.  Lower is
 * better.  Penalty grows sharply for collisions inside time_horizon. */
static float avoidance_cost(const Agent *self,
                              float vx, float vz,
                              const Agent *agents, uint32_t cap,
                              uint32_t self_idx,
                              float sight_radius, float time_horizon)
{
    /* Deviation from desired velocity. */
    float dx = vx - self->desired_vx;
    float dz = vz - self->desired_vz;
    float cost = sqrtf(dx * dx + dz * dz);

    float sight2 = sight_radius * sight_radius;

    for (uint32_t j = 0; j < cap; ++j) {
        if (j == self_idx) continue;
        const Agent *other = &agents[j];
        if (!other->alive) continue;

        float rx = other->pos_x - self->pos_x;
        float rz = other->pos_z - self->pos_z;
        float dist2 = rx * rx + rz * rz;
        if (dist2 > sight2) continue;

        /* Relative velocity if self picked v.  Other keeps current vel
         * (reciprocal-RVO assumes both adjust; treating other as static
         * is a conservative stand-in that still produces good motion). */
        float rvx = vx - other->vel_x;
        float rvz = vz - other->vel_z;
        float radius_sum = self->radius + other->radius;
        float radius_sum2 = radius_sum * radius_sum;

        /* Solve |r + rv*t|² = (r1+r2)² for smallest positive t. */
        float a_q = rvx * rvx + rvz * rvz;
        float b_q = 2.0f * (rx * rvx + rz * rvz);
        float c_q = (rx * rx + rz * rz) - radius_sum2;

        if (c_q < 0.0f) {
            /* Already overlapping — heavy penalty. */
            cost += 100.0f;
            continue;
        }
        if (a_q < 1e-6f) continue;          /* parallel motion */
        float disc = b_q * b_q - 4.0f * a_q * c_q;
        if (disc < 0.0f) continue;          /* never collides */
        float t = (-b_q - sqrtf(disc)) / (2.0f * a_q);
        if (t < 0.0f || t > time_horizon) continue;

        /* Cost is inverse-time: imminent collisions cost more. */
        cost += (time_horizon - t) * 10.0f;
    }
    return cost;
}

/* Pick a velocity for self that minimises avoidance_cost.  Samples
 * the desired velocity plus N-1 perturbations around it on a circle. */
static void apply_avoidance(JceNavAgentSet *s, uint32_t self_idx)
{
    Agent *self = &s->agents[self_idx];
    const JceNavAvoidanceConfig *c = &s->avoidance;

    float best_vx   = self->desired_vx;
    float best_vz   = self->desired_vz;
    float best_cost = avoidance_cost(self, best_vx, best_vz,
                                      s->agents, s->capacity, self_idx,
                                      c->sight_radius, c->time_horizon);

    uint32_t n = c->num_samples;
    if (n < 1) n = 1;

    /* Sample on circle of radius max_speed around origin (so we can
     * sidestep or reverse if needed) plus a "stop" candidate. */
    for (uint32_t k = 0; k < n; ++k) {
        float theta = (2.0f * 3.14159265f * (float)k) / (float)n;
        /* Magnitude varies between 0.5 and 1.0 of max_speed for
         * smoother slow-down options. */
        float mag = self->max_speed * (0.5f + 0.5f * (float)((k * 7u) % 3u) / 2.0f);
        float vx = cosf(theta) * mag;
        float vz = sinf(theta) * mag;
        float cost = avoidance_cost(self, vx, vz, s->agents, s->capacity,
                                     self_idx, c->sight_radius, c->time_horizon);
        if (cost < best_cost) {
            best_cost = cost;
            best_vx   = vx;
            best_vz   = vz;
        }
    }

    /* Also try standing still — useful at choke-points. */
    float stop_cost = avoidance_cost(self, 0.0f, 0.0f, s->agents, s->capacity,
                                      self_idx, c->sight_radius, c->time_horizon);
    if (stop_cost < best_cost) {
        best_vx = 0.0f;
        best_vz = 0.0f;
    }

    self->desired_vx = best_vx;
    self->desired_vz = best_vz;
}

/* Hard depenetration: push overlapping agents apart symmetrically. */
static void apply_overlap_push(JceNavAgentSet *s, float dt)
{
    float gain = s->avoidance.overlap_push_gain;
    if (gain <= 0.0f) return;

    for (uint32_t i = 0; i < s->capacity; ++i) {
        Agent *a = &s->agents[i];
        if (!a->alive) continue;
        for (uint32_t j = i + 1; j < s->capacity; ++j) {
            Agent *b = &s->agents[j];
            if (!b->alive) continue;
            float dx = b->pos_x - a->pos_x;
            float dz = b->pos_z - a->pos_z;
            float d2 = dx * dx + dz * dz;
            float r  = a->radius + b->radius;
            if (d2 >= r * r || d2 < 1e-6f) continue;
            float d  = sqrtf(d2);
            float pen = (r - d) * 0.5f;
            float nx = dx / d;
            float nz = dz / d;
            float push = pen * gain * dt;
            a->pos_x -= nx * push;
            a->pos_z -= nz * push;
            b->pos_x += nx * push;
            b->pos_z += nz * push;
        }
    }
}

/* Steer current velocity toward desired (acceleration-limited) and
 * integrate position. */
static void integrate(Agent *a, float dt)
{
    float ax = a->desired_vx - a->vel_x;
    float az = a->desired_vz - a->vel_z;
    float a_mag = v2_len(ax, az);
    float a_cap = a->max_accel * dt;
    if (a_mag > a_cap && a_mag > 1e-6f) {
        float k = a_cap / a_mag;
        ax *= k;
        az *= k;
    }
    a->vel_x += ax;
    a->vel_z += az;

    float v_mag = v2_len(a->vel_x, a->vel_z);
    if (v_mag > a->max_speed && v_mag > 1e-6f) {
        float k = a->max_speed / v_mag;
        a->vel_x *= k;
        a->vel_z *= k;
    }

    a->pos_x += a->vel_x * dt;
    a->pos_z += a->vel_z * dt;
}

void jce_nav_agent_set_update(JceNavAgentSet *s, float dt)
{
    if (!s || dt <= 0.0f) return;

    /* Demote ARRIVED back to IDLE so callers see ARRIVED for exactly
     * one frame after the transition. */
    for (uint32_t i = 0; i < s->capacity; ++i) {
        Agent *a = &s->agents[i];
        if (!a->alive) continue;
        if (a->status == JCE_NAV_AGENT_ARRIVED) {
            a->status = JCE_NAV_AGENT_IDLE;
            a->path_len = 0;
            a->path_cursor = 0;
        }
    }

    /* Pass 1: compute desired velocity for each agent. */
    for (uint32_t i = 0; i < s->capacity; ++i) {
        if (!s->agents[i].alive) continue;
        compute_desired_velocity(&s->agents[i]);
    }

    /* Pass 2: optional crowd avoidance — modify desired in place. */
    if (s->avoidance.enabled && s->avoidance.num_samples > 0
        && s->avoidance.sight_radius > 0.0f
        && s->avoidance.time_horizon > 0.0f) {
        for (uint32_t i = 0; i < s->capacity; ++i) {
            if (!s->agents[i].alive) continue;
            if (s->agents[i].status != JCE_NAV_AGENT_MOVING) continue;
            apply_avoidance(s, i);
        }
    }

    /* Pass 3: steer + integrate. */
    for (uint32_t i = 0; i < s->capacity; ++i) {
        Agent *a = &s->agents[i];
        if (!a->alive) continue;
        if (a->status != JCE_NAV_AGENT_MOVING) {
            /* Decay residual velocity for idle/arrived agents. */
            a->desired_vx = a->vel_x * 0.5f;
            a->desired_vz = a->vel_z * 0.5f;
            a->vel_x *= 0.5f;
            a->vel_z *= 0.5f;
            a->pos_x += a->vel_x * dt;
            a->pos_z += a->vel_z * dt;
            continue;
        }
        integrate(a, dt);
    }

    /* Pass 4: hard depenetration if configured. */
    if (s->avoidance.enabled) apply_overlap_push(s, dt);
}

void jce_nav_agent_set_avoidance(JceNavAgentSet *s,
                                   const JceNavAvoidanceConfig *cfg)
{
    if (!s) return;
    if (!cfg) {
        memset(&s->avoidance, 0, sizeof(s->avoidance));
        return;
    }
    s->avoidance = *cfg;
    if (s->avoidance.num_samples == 0) s->avoidance.num_samples = 12;
    if (s->avoidance.sight_radius <= 0) s->avoidance.sight_radius = 6.0f;
    if (s->avoidance.time_horizon <= 0) s->avoidance.time_horizon = 2.0f;
}

void jce_nav_agent_set_path_fn(JceNavAgentSet *s,
                                JceNavAgentPathFn fn,
                                void *user)
{
    if (!s) return;
    s->path_fn   = fn;
    s->path_user = user;
}

/* ── Queries ─────────────────────────────────────────────────────── */

void jce_nav_agent_get_position(const JceNavAgentSet *s, JceNavAgentHandle h,
                                  float *out_x, float *out_z)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return;
    if (!s->agents[h.idx].alive) return;
    if (out_x) *out_x = s->agents[h.idx].pos_x;
    if (out_z) *out_z = s->agents[h.idx].pos_z;
}

void jce_nav_agent_get_velocity(const JceNavAgentSet *s, JceNavAgentHandle h,
                                  float *out_vx, float *out_vz)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return;
    if (!s->agents[h.idx].alive) return;
    if (out_vx) *out_vx = s->agents[h.idx].vel_x;
    if (out_vz) *out_vz = s->agents[h.idx].vel_z;
}

JceNavAgentStatus jce_nav_agent_get_status(const JceNavAgentSet *s,
                                             JceNavAgentHandle h)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity)
        return JCE_NAV_AGENT_IDLE;
    if (!s->agents[h.idx].alive) return JCE_NAV_AGENT_IDLE;
    return s->agents[h.idx].status;
}

uint32_t jce_nav_agent_path_length(const JceNavAgentSet *s,
                                     JceNavAgentHandle h)
{
    if (!s || !jce_nav_agent_valid(h) || h.idx >= s->capacity) return 0;
    if (!s->agents[h.idx].alive) return 0;
    const Agent *a = &s->agents[h.idx];
    if (a->path_cursor >= a->path_len) return 0;
    return a->path_len - a->path_cursor;
}

uint32_t jce_nav_agent_set_count(const JceNavAgentSet *s)
{
    return s ? s->count : 0u;
}
