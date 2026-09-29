/*
 * jce_scene_light_probe.c  Light-probe group operations on a scene.
 *
 * Why the ENGINE owns this and not the editor bake panel, which is where the
 * probe bake is actually driven from:
 *
 *   - check_component_field_consumed.py scans engine/src only.  A field read
 *     nowhere but an ImGui panel is, to that gate, still a field nothing
 *     consumes -- and it is right: policy that lives in a panel does not ship
 *     in a game.
 *   - the whole probe bake currently lives inside jce_panel_lightmap_bake.cpp,
 *     so a headless or SDK build cannot bake probes at all.  This is one piece
 *     of that moved to where it belongs.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_lightmapper.h>

bool jce_scene_light_probe_group_dering(JceScene *s, JceEntity e)
{
    JceLightProbeGroupComponent *g = jce_scene_get_light_probe_group(s, e);
    /* Only a BAKED group has coefficients to window, and only a group that
     * asked for it gets windowed: deringing an un-asked-for probe would
     * quietly soften every probe in the scene. */
    if (!g || !g->dering || !g->sh9_baked || g->probe_count <= 0) return false;
    jce_lightmapper_sh9_dering(g->sh9, g->probe_count);
    return true;
}
