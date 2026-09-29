/*
 * jce_scene_portal.c  Occlusion-portal operations on a scene.
 *
 * Deliberately NOT in jce_scene_components_render.c.  That file is a
 * SERIALIZER, and check_component_field_consumed.py excludes every
 * jce_scene_components_*.c from its scan on purpose: a field that only its own
 * parser and writer touch is round-tripped, not consumed.  Putting a real
 * operation in a serializer file would have hidden it from that gate --
 * measured, not guessed: with the function there, portal_id stayed on the
 * unconsumed list.
 */

#include <jce/middleware/scene/jce_scene.h>

typedef struct { int32_t id; bool open; int changed; } PortalGroupSet;

static void portal_group_cb(JceScene *s, JceEntity e, void *ud)
{
    PortalGroupSet *g = (PortalGroupSet *)ud;
    JceOcclusionPortalComponent *op = jce_scene_get_occlusion_portal(s, e);
    if (!op || !g || op->portal_id != g->id || op->open == g->open) return;
    op->open = g->open;
    ++g->changed;
}

/* JceOcclusionPortalComponent.portal_id is documented as "user-assigned ID for
 * pairing portals" and was read by nothing -- a pairing key with no operation
 * that pairs.  This is that operation: one call opens or closes the whole
 * group, which is how a double door or a row of shutters is actually authored.
 * Returns the number of portals changed, so a caller can tell "no portal has
 * that id" from "they were already in that state". */
int jce_scene_occlusion_portals_set_open(JceScene *s, int32_t portal_id, bool open)
{
    if (!s) return 0;
    PortalGroupSet g = { portal_id, open, 0 };
    jce_scene_each_entity(s, portal_group_cb, &g);
    return g.changed;
}
