/*
 * jce_scene_view_snap.cpp  Snap-to-ground scene-view operation.
 *
 * Drops every selected entity straight down so the bottom of its world AABB
 * rests on the first surface below it.  Surface candidates, in priority of
 * whichever is hit HIGHEST below the entity: other entities' world AABBs
 * (real mesh bounds — same helper the Unity-style asset drop uses), the
 * terrain heightfield, and finally the Y=0 ground plane (matching the asset
 * drop's fallback).  Selected entities never act as ground for each other,
 * so a stacked multi-selection doesn't land on itself mid-operation.
 * The whole selection becomes ONE undo entry.
 */

#include "jce_scene_view_internal.h"

extern "C" {
#include <jce/middleware/scene/jce_terrain.h>
}

/* Provided by jce_panel_terrain.cpp (NULL when the scene has no terrain). */
extern "C" struct JceTerrain *jce_terrain_panel_get_terrain(void);

/* Downward ray vs world AABB: the ray from (x, y0, z) straight down hits the
 * box's top face iff (x,z) lies inside the box footprint and the top sits at
 * or below the origin.  Returns the hit height. */
static bool down_ray_hit_aabb(float x, float y0, float z,
                              const float lo[3], const float hi[3],
                              float *out_y)
{
    if (x < lo[0] || x > hi[0] || z < lo[2] || z > hi[2]) return false;
    if (hi[1] > y0) return false;
    *out_y = hi[1];
    return true;
}

void jce_scene_view_snap_selection_to_ground(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    /* Copy the selection (mutating transforms must not invalidate it);
     * fall back to the focused entity when nothing is multi-selected. */
    uint32_t ids[JCE_MAX_SELECTED];
    int      n   = 0;
    {
        int sc = 0;
        const uint32_t *sel = jce_state_get_selection(&sc);
        for (int i = 0; i < sc && n < JCE_MAX_SELECTED; i++)
            if (sel[i] != 0) ids[n++] = sel[i];
        if (n == 0) {
            uint32_t f = jce_state_get_focused();
            if (f != 0) ids[n++] = f;
        }
    }
    if (n == 0) return;

    JceTerrain *terr  = jce_terrain_panel_get_terrain();
    const int   total = jce_state_get_entity_count();

    jce_state_begin_batch_edit();

    for (int i = 0; i < n; i++) {
        const uint32_t id = ids[i];
        if (!jce_state_entity_exists(id)) continue;

        float lo[3], hi[3];
        if (!jce_editor_scene_camera_get_entity_focus_bounds(id, lo, hi))
            continue;

        const float cx = (lo[0] + hi[0]) * 0.5f;
        const float cz = (lo[2] + hi[2]) * 0.5f;
        /* Start a hair above the bottom so a surface flush with the entity's
         * base still counts (snapping up by that epsilon is imperceptible). */
        const float y0 = lo[1] + 1e-3f;

        float best_y = 0.0f;   /* Y=0 ground plane = final fallback */
        bool  found  = false;

        for (int pi = 0; pi < total; pi++) {
            const uint32_t pid = jce_state_get_entity_id_by_index(pi);
            if (pid == 0 || pid == id) continue;
            bool in_sel = false;
            for (int k = 0; k < n; k++)
                if (ids[k] == pid) { in_sel = true; break; }
            if (in_sel) continue;
            if (!jce_state_entity_exists(pid) ||
                !jce_state_entity_enabled(pid))
                continue;

            float plo[3], phi[3];
            if (!jce_editor_scene_camera_get_entity_focus_bounds(pid, plo, phi))
                continue;
            float y;
            if (down_ray_hit_aabb(cx, y0, cz, plo, phi, &y) &&
                (!found || y > best_y)) {
                best_y = y;
                found  = true;
            }
        }

        if (terr) {
            const float ro[3] = { cx, y0, cz };
            const float rd[3] = { 0.0f, -1.0f, 0.0f };
            float hit[3];
            if (jce_terrain_raycast(terr, ro, rd, 1.0e6f, hit) &&
                hit[1] <= y0 && (!found || hit[1] > best_y)) {
                best_y = hit[1];
                found  = true;
            }
        }

        const float delta = best_y - lo[1];
        if (fabsf(delta) < 1e-6f) continue;

        JceTransform *t = jce_scene_get_transform(scene, (JceEntity)id);
        if (!t) continue;
        JceTransform nt = *t;
        nt.position.y += delta;
        jce_scene_set_transform(scene, (JceEntity)id, &nt);
    }

    jce_state_end_batch_edit();
    jce_editor_inspector_request_sync();
}
