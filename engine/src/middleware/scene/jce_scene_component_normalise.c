/* See jce_scene_component_normalise.h for why this exists and why it is a
 * file of its own. */

#include "jce_scene_component_normalise.h"

void jce_scene_normalise_mesh_renderer(JceScene *s, JceMeshRenderer *mr)
{
    if (!s || !mr) return;

    /* Intern once, and only if something actually needs it: the common case
     * is a component the engine built, where every field is already a pool
     * pointer and this walks seven non-NULL checks and returns. */
    if (mr->mesh_path && mr->material_path && mr->albedo_tex && mr->mr_tex &&
        mr->normal_tex && mr->ao_tex && mr->emissive_tex)
        return;

    const char *empty = jce_scene_intern(s, "");
    if (!mr->mesh_path)     mr->mesh_path     = empty;
    if (!mr->material_path) mr->material_path = empty;
    if (!mr->albedo_tex)    mr->albedo_tex    = empty;
    if (!mr->mr_tex)        mr->mr_tex        = empty;
    if (!mr->normal_tex)    mr->normal_tex    = empty;
    if (!mr->ao_tex)        mr->ao_tex        = empty;
    if (!mr->emissive_tex)  mr->emissive_tex  = empty;
}
