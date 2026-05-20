/*
 * jce_nav_areas.c  Per-agent area overrides side-table.
 */

#include <jce/middleware/ai/jce_nav_areas.h>

#include <string.h>

static JceNavAreaOverride s_overrides[JCE_NAV_AREAS_MAX_AGENTS];

void jce_nav_areas_clear(void)
{
    memset(s_overrides, 0, sizeof(s_overrides));
}

static int find_slot(JceNavAgentHandle a, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < JCE_NAV_AREAS_MAX_AGENTS; ++i) {
        if (s_overrides[i].active && s_overrides[i].agent.idx == a.idx)
            return i;
        if (!s_overrides[i].active && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    s_overrides[free_slot].agent     = a;
    s_overrides[free_slot].area_mask = 0xFFFFFFFFu;
    for (int i = 0; i < JCE_NAV_AREA_COUNT; ++i)
        s_overrides[free_slot].area_cost[i] = 1.0f;
    s_overrides[free_slot].active    = true;
    return free_slot;
}

bool jce_nav_agent_set_area_mask(JceNavAgentHandle a, uint32_t mask)
{
    int s = find_slot(a, true);
    if (s < 0) return false;
    s_overrides[s].area_mask = mask;
    return true;
}

bool jce_nav_agent_set_area_cost(JceNavAgentHandle a, uint8_t area, float cost)
{
    if (area >= JCE_NAV_AREA_COUNT) return false;
    int s = find_slot(a, true);
    if (s < 0) return false;
    s_overrides[s].area_cost[area] = cost;
    return true;
}

uint32_t jce_nav_agent_get_area_mask(JceNavAgentHandle a)
{
    int s = find_slot(a, false);
    return (s >= 0) ? s_overrides[s].area_mask : 0xFFFFFFFFu;
}

float jce_nav_agent_area_cost(JceNavAgentHandle a, uint8_t area)
{
    if (area >= JCE_NAV_AREA_COUNT) return 1.0f;
    int s = find_slot(a, false);
    return (s >= 0) ? s_overrides[s].area_cost[area] : 1.0f;
}

bool jce_nav_agent_can_enter_area(JceNavAgentHandle a, uint8_t area)
{
    if (area >= JCE_NAV_AREA_COUNT) return false;
    int s = find_slot(a, false);
    if (s < 0) return true; /* default mask = all */
    return (s_overrides[s].area_mask & (1u << area)) != 0;
}
