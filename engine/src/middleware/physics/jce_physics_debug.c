/*
 * jce_physics_debug.c  Static snapshot store.
 */

#include <jce/middleware/physics/jce_physics_debug.h>

#include <string.h>

static JcePhysicsDebugStats s_stats;

const JcePhysicsDebugStats *jce_physics_debug_get(void)
{
    return &s_stats;
}

void jce_physics_debug_set(const JcePhysicsDebugStats *s)
{
    if (!s) return;
    /* Preserve top_contacts which is mutated separately. */
    JcePhysicsContact saved[JCE_PHYSICS_DEBUG_TOP_CONTACTS];
    uint32_t saved_count = s_stats.top_contact_count;
    memcpy(saved, s_stats.top_contacts, sizeof(saved));
    s_stats = *s;
    s_stats.top_contact_count = saved_count;
    memcpy(s_stats.top_contacts, saved, sizeof(saved));
}

void jce_physics_debug_push_contact(const JcePhysicsContact *c)
{
    if (!c) return;
    /* Insert into descending-impulse order, evict smallest. */
    if (s_stats.top_contact_count < JCE_PHYSICS_DEBUG_TOP_CONTACTS) {
        s_stats.top_contacts[s_stats.top_contact_count++] = *c;
        return;
    }
    uint32_t worst = 0;
    for (uint32_t i = 1; i < JCE_PHYSICS_DEBUG_TOP_CONTACTS; ++i)
        if (s_stats.top_contacts[i].impulse < s_stats.top_contacts[worst].impulse)
            worst = i;
    if (c->impulse > s_stats.top_contacts[worst].impulse)
        s_stats.top_contacts[worst] = *c;
}

void jce_physics_debug_clear_contacts(void)
{
    s_stats.top_contact_count = 0;
}
