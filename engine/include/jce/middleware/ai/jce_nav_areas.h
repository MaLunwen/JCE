/*
 * jce_nav_areas.h  Per-agent NavMesh area mask + cost weights.
 *
 * Unity NavMesh.AreaCosts equivalent.  Each area id (0..31) carries
 * a cost multiplier; the agent's mask gates which areas it may
 * traverse (a 0 bit forbids the area; 1 allows with the configured
 * cost).  Default mask = 0xFFFFFFFFu (all areas), cost = 1.0 each.
 *
 * Storage is a side-table keyed by JceNavAgentHandle (from
 * jce_nav_agent.h) so we don't break the existing JceNavAgentDesc
 * ABI.  Recast integration reads these at path-find time via
 * jce_nav_agent_area_cost().
 *
 * Layer: ai (Layer 4) — public.
 */

#ifndef JCE_NAV_AREAS_H
#define JCE_NAV_AREAS_H

#include <jce/middleware/ai/jce_nav_agent.h>

JCE_EXTERN_C_BEGIN

#define JCE_NAV_AREA_COUNT 32
#define JCE_NAV_AREAS_MAX_AGENTS 256

typedef struct {
    JceNavAgentHandle agent;
    uint32_t          area_mask;
    float             area_cost[JCE_NAV_AREA_COUNT];
    bool              active;
} JceNavAreaOverride;

/* Clear registry. */
JCE_API void jce_nav_areas_clear(void);

/* Register area-cost overrides for an agent.  Calling without
 * registering returns defaults: mask = ALL, cost = 1.0 per area. */
JCE_API bool jce_nav_agent_set_area_mask(JceNavAgentHandle a, uint32_t mask);
JCE_API bool jce_nav_agent_set_area_cost(JceNavAgentHandle a, uint8_t area_id,
                                          float cost);

JCE_API uint32_t jce_nav_agent_get_area_mask(JceNavAgentHandle a);
JCE_API float    jce_nav_agent_area_cost   (JceNavAgentHandle a, uint8_t area_id);

/* Returns true when the agent is allowed to traverse `area_id`. */
JCE_API bool jce_nav_agent_can_enter_area(JceNavAgentHandle a, uint8_t area_id);

JCE_EXTERN_C_END

#endif /* JCE_NAV_AREAS_H */
