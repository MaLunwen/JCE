/*
 * jce_panel_inspector_gameplay.cpp
 * Gameplay component inspector drawers: behavior tree, spawn manager, weapon,
 * save point, trigger volume, terrain, particle emitter, script.
 */

extern "C" struct JceTerrain *jce_terrain_panel_get_terrain(void);
extern "C" void jce_terrain_panel_set_brush_armed(bool armed);
extern "C" bool jce_terrain_panel_open_asset(const char *path);
#include <vector>
#include "io/jce_editor_file_util.h"
#include <jce/middleware/scene/jce_foliage.h>
#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_ui_state.h"

extern "C" {
#include <jce/renderer/jce_renderer_caps.h>    /* compute caps gate (GPU particles) */
#include <jce/renderer/jce_render_pipeline.h>  /* gpu_particles feature flag hint   */
#include <jce/resource/jce_asset_format.h>     /* script-extension authority        */
#include <jce/middleware/script/jce_script_vm.h> /* which VMs THIS build linked     */
#include "core/jce_assetdb.h"                  /* the picker's own attachability   */
#include <jce/middleware/scene/jce_script_exports.h> /* what the SCRIPT declares */
#include <jce/os/core/jce_filesystem.h>            /* reading it to find out    */
}

void draw_comp_behavior_tree(JceBehaviorTree *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.bt.active", "bt"), &bt->active))
        insp_undo_bool(&bt->active);

    /* Behavior-tree asset (BehaviorTree.CPP XML).  The runtime loads this
     * file into its JceBtContext at Play and ticks it each gameplay frame.
     * jce_draw_path_input_asset owns the JCE_DND_ASSET_PATH drop target on
     * the text field — no accept_asset_drop() needed here or below. */
    ImGui::TextUnformatted(jce_editor_i18n("inspector.bt.treePath"));
    if (jce_draw_path_input_asset("##bt_tree", bt->tree_path,
                                  sizeof(bt->tree_path), JCE_ASSET_KIND_DATA))
        insp_track_edit();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("inspector.bt.clearTree", "bt")))
        insp_undo_set(&bt->tree_path[0], '\0');

    ImGui::DragFloat(jce_editor_i18n_id("inspector.bt.tickHz", "bt"),
                     &bt->tick_hz, 0.5f, 0.0f, 240.0f, "%.1f");
    insp_track_edit();
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bt.tickHzHint"));

    /* Perception sensing params, read by the runtime perception pass each
     * gameplay frame (0 ⇒ engine default).  The sight cone is authored in
     * degrees and stored as a half-angle in radians. */
    ImGui::DragFloat(jce_editor_i18n_id("inspector.bt.sightRange", "bt"),
                     &bt->sight_range, 0.5f, 0.0f, 200.0f, "%.1f m");
    insp_track_edit();
    {
        float deg = bt->sight_half_angle * (180.0f / 3.14159265f);
        if (deg <= 0.0f) deg = 60.0f;   /* effective default while unauthored */
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.bt.sightAngle", "bt"),
                             &deg, 0.5f, 1.0f, 180.0f, "%.0f deg")) {
            bt->sight_half_angle = deg * (3.14159265f / 180.0f);
        }
        insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.bt.hearingRange", "bt"),
                     &bt->hearing_range, 0.5f, 0.0f, 200.0f, "%.1f m");
    insp_track_edit();

    if (bt->tree_path[0] == '\0')
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("inspector.bt.noTree"));

    /* Read-only BT Visualizer panel: tree structure in edit mode, live
     * node statuses during Play. */
    if (ImGui::Button(jce_editor_i18n_id("inspector.bt.openInVisualizer", "bt"))) {
        bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_BT_VISUALIZER);
        if (vis) *vis = true;
    }
}

void draw_comp_spawn_manager(JceSpawnManagerComponent *m)
{
    if (!m) return;
    bool en = m->enabled != 0;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sm.enabled", "sm"), &en))
        insp_undo_set(&m->enabled, en ? 1 : 0);
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sm.maxPeds", "sm"),          &m->max_peds,         1.0f, 0, 1024); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sm.maxVehicles", "sm"),      &m->max_vehicles,     1.0f, 0, 1024); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.minSpawnRadius", "sm"),  &m->min_spawn_radius, 0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.maxSpawnRadius", "sm"),  &m->max_spawn_radius, 0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.despawnPad", "sm"),       &m->despawn_pad,      0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.spawnInterval", "sm"),&m->spawn_interval,   0.05f, 0.0f, 60.0f,    "%.2f"); insp_track_edit();
    /* WHAT to spawn.  The engine gates on this and the editor never let a
     * designer set it: jce_runtime.c:192 reads
     *   if (!smc || !smc->ped_prefab_path[0]) return 0;  // nothing authored
     * so every SpawnManager placed from the inspector spawned nothing, with
     * max_peds, the radii and the interval all tunable above it.  The mirror
     * of the usual defect -- the engine reads a field the editor has no
     * control for, so the capability exists and cannot be reached. */
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.sm.pedPrefab", "sm"),
                              m->ped_prefab_path, sizeof m->ped_prefab_path,
                              JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(m->ped_prefab_path, sizeof m->ped_prefab_path);
    if (m->max_spawn_radius < m->min_spawn_radius) m->max_spawn_radius = m->min_spawn_radius;

    int pn = m->ped_archetype_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.pedArchetypes", "sm"), &pn, 1.0f, 0, 8)) {
        if (pn < 0) pn = 0; if (pn > 8) pn = 8;
        m->ped_archetype_count = pn;
    }
    char lbl[32];
    for (int i = 0; i < m->ped_archetype_count; ++i) {
        snprintf(lbl, sizeof lbl, "Ped[%d] id##sm%d", i, i);
        int v = (int)m->ped_archetypes[i];
        if (ImGui::DragInt(lbl, &v, 1.0f, 0, INT_MAX)) {
            m->ped_archetypes[i] = (uint32_t)(v < 0 ? 0 : v);
        }
        insp_track_edit();
    }

    int vn = m->vehicle_archetype_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.vehicleArchetypes", "sm"), &vn, 1.0f, 0, 8)) {
        if (vn < 0) vn = 0; if (vn > 8) vn = 8;
        m->vehicle_archetype_count = vn;
    }
    for (int i = 0; i < m->vehicle_archetype_count; ++i) {
        snprintf(lbl, sizeof lbl, "Vehicle[%d] id##smv%d", i, i);
        int v = (int)m->vehicle_archetypes[i];
        if (ImGui::DragInt(lbl, &v, 1.0f, 0, INT_MAX)) {
            m->vehicle_archetypes[i] = (uint32_t)(v < 0 ? 0 : v);
        }
        insp_track_edit();
    }

    /* RNG seed (display as two 32-bit halves to avoid ImGui int64 absence). */
    uint32_t lo = (uint32_t)(m->rng_seed & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(m->rng_seed >> 32);
    int lo_i = (int)lo, hi_i = (int)hi;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.rngSeedLo", "sm"), &lo_i, 1.0f)) {
        m->rng_seed = ((uint64_t)(uint32_t)hi_i << 32) | (uint32_t)lo_i;
    }
    insp_track_edit();
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.rngSeedHi", "sm"), &hi_i, 1.0f)) {
        m->rng_seed = ((uint64_t)(uint32_t)hi_i << 32) | (uint32_t)lo_i;
    }
    insp_track_edit();
    ImGui::TextDisabled(jce_editor_i18n("inspector.sm.roadNetworkNote"));
}

void draw_comp_weapon(JceWeaponComponent *w)
{
    if (!w) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.wp.name", "wp"), w->name, sizeof w->name); insp_track_edit();
    const char *kinds[] = { jce_editor_i18n("inspector.wp.kind.hitscan"), jce_editor_i18n("inspector.wp.kind.projectile") };
    int k = w->kind; if (k < 0 || k > 1) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.wp.kind", "wp"), &k, kinds, 2))
        insp_undo_set(&w->kind, k);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.damage", "wp"),         &w->damage,          0.1f, 0.0f, 100000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.range", "wp"),      &w->range,           0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.rpm", "wp"),            &w->rpm,             1.0f, 0.0f, 10000.0f,  "%.0f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.wp.clipSize", "wp"),      &w->clip_size,       1.0f, 0, 10000); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.wp.reserveMax", "wp"),    &w->reserve_max,     1.0f, 0, 1000000); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.reload", "wp"),     &w->reload_seconds,  0.05f, 0.0f, 60.0f,    "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.spread", "wp"),   &w->spread_deg,      0.05f, 0.0f, 90.0f,    "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.recoilShot", "wp"),    &w->recoil_per_shot, 0.05f, 0.0f, 90.0f,    "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.recoilRecovery", "wp"),&w->recoil_recovery, 0.1f, 0.0f, 360.0f,    "%.2f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.wp.pellets", "wp"),        &w->pellets,         1.0f, 1, 64); insp_track_edit();
    if (w->kind == JCE_WEAPON_COMP_PROJECTILE) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.projectileSpeed", "wp"), &w->projectile_speed, 1.0f, 0.0f, 10000.0f, "%.1f");
        insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.wp.fullAuto", "wp"), &w->full_auto)) insp_undo_bool(&w->full_auto);
}

void draw_comp_save_point(JceSavePointComponent *sp)
{
    if (!sp) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.sv.saveId", "sv"),     sp->save_id,      sizeof sp->save_id);      insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.sv.displayName", "sv"),sp->display_name, sizeof sp->display_name); insp_track_edit();
    const char *kinds[] = { jce_editor_i18n("inspector.sv.kind.manual"), jce_editor_i18n("inspector.sv.kind.auto"), jce_editor_i18n("inspector.sv.kind.checkpoint") };
    int k = sp->kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.sv.kind", "sv"), &k, kinds, 3))
        insp_undo_set(&sp->kind, k);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sv.radius", "sv"), &sp->radius, 0.05f, 0.0f, 1000.0f, "%.2f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sv.slot", "sv"),   &sp->slot,   1.0f, -1, 256);              insp_track_edit();
    insp_unwired_field_badge();   /* display_name, kind, slot */
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sv.oneShot", "sv"),          &sp->one_shot))         insp_undo_bool(&sp->one_shot);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sv.requireInteract", "sv"),  &sp->require_interact)) insp_undo_bool(&sp->require_interact);
}

void draw_comp_trigger_volume(JceTriggerVolumeComponent *tv)
{
    if (!tv) return;
    const char *shapes[] = { jce_editor_i18n("inspector.trig.shape.aabb"), jce_editor_i18n("inspector.trig.shape.sphere"), jce_editor_i18n("inspector.trig.shape.obb") };
    int sh = tv->shape; if (sh < 0 || sh > 2) sh = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.trig.shape", "trig"), &sh, shapes, 3))
        insp_undo_set(&tv->shape, sh);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.enabled", "trig"), &tv->enabled))
        insp_undo_bool(&tv->enabled);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.fireStayEvents", "trig"), &tv->fire_stay))
        insp_undo_bool(&tv->fire_stay);
    ImGui::InputText(jce_editor_i18n_id("inspector.trig.tag", "trig"), tv->tag, sizeof tv->tag);
    insp_unwired_field_badge();   /* fire_stay, tag */
    /* Commit the tag when the field loses focus (click away / select another
     * object) or on Enter — not every frame — so switching objects mid-edit
     * still records + saves the change. */
    insp_track_edit();

    ImGui::Separator();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.center", "trig"), tv->center, 0.1f);
    insp_track_edit();
    if (tv->shape == 1) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.trig.radius", "trig"), &tv->half_extents[0], 0.05f, 0.0f, 10000.0f, "%.2f");
        insp_track_edit();
    } else {
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.halfExtents", "trig"), tv->half_extents, 0.05f, 0.0f, 10000.0f, "%.2f");
        insp_track_edit();
    }
    if (tv->shape == 2) {
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("inspector.trig.obbAxes"));
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.axisX", "trig"), tv->axis_x, 0.01f);
        insp_track_edit();
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.axisY", "trig"), tv->axis_y, 0.01f);
        insp_track_edit();
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.axisZ", "trig"), tv->axis_z, 0.01f);
        insp_track_edit();
    }
}

void draw_comp_terrain(JceTerrainComponent *tc)
{
    if (!tc) return;
    jce_draw_path_input_asset(jce_editor_i18n("inspector.terrain.path"), tc->terrain_path, sizeof tc->terrain_path, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.dropHint"));

    if (ImGui::Checkbox(jce_editor_i18n("inspector.terrain.visible"), &tc->visible))
        insp_undo_bool(&tc->visible);

    ImGui::ColorEdit3(jce_editor_i18n("inspector.terrain.tint"), tc->tint);
    insp_unwired_field_badge();   /* tint */
    insp_track_edit();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.terrain.splatHeader"));
    if (ImGui::Checkbox(jce_editor_i18n("inspector.terrain.splatEnable"), &tc->splat_enabled))
        insp_undo_bool(&tc->splat_enabled);
    if (tc->tile_scale <= 0.0f) tc->tile_scale = 10.0f;
    ImGui::DragFloat(jce_editor_i18n("inspector.terrain.layerTile"), &tc->tile_scale, 0.1f, 0.1f, 256.0f, "%.2f");
    insp_track_edit();
    ImGui::SliderFloat(jce_editor_i18n("inspector.terrain.heightBlend"),
                       &tc->height_blend, 0.0f, 1.0f, "%.2f");
    insp_track_edit();

    for (int i = 0; i < 4; i++) {
        char label[32];
        snprintf(label, sizeof label, jce_editor_i18n("inspector.terrain.layerFmt"), i);
        ImGui::PushID(i);
        jce_draw_path_input_asset(label, tc->layer_albedo_path[i],
                         sizeof tc->layer_albedo_path[i], JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.normal"),
                         tc->layer_normal_path[i],
                         sizeof tc->layer_normal_path[i], JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        jce_draw_path_input_asset(jce_editor_i18n("inspector.terrain.maskMap"),
                         tc->layer_mask_path[i],
                         sizeof tc->layer_mask_path[i], JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::SliderFloat(jce_editor_i18n("inspector.terrain.normalScale"),
                           &tc->layer_normal_scale[i], 0.0f, 2.0f, "%.2f");
        insp_track_edit();
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.splatChannels"));
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.maskChannels"));

    /* Show summary if a terrain file is bound. Using load_file is heavy;
     * for the inspector we just print the path and let the Terrain panel
     * handle authoring. */
    if (tc->terrain_path[0]) {
        ImGui::Separator();
        if (ImGui::Button(jce_editor_i18n("inspector.terrain.edit"),
                          ImVec2(-1.0f, 0.0f)))
            (void)jce_terrain_panel_open_asset(tc->terrain_path);
        ImGui::TextWrapped("%s", jce_editor_i18n("inspector.terrain.useTerrainPanel"));
    } else {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("inspector.terrain.notBound"));
    }
}

/* ── Foliage density paint brush (large-world #8a) ───────────────────────────
 * Armed from the VegetationScatter inspector; the scene view raycasts the terrain
 * (reusing the terrain-brush plumbing) and calls jce_foliage_brush_apply_world,
 * which carves the FOCUSED entity's density_paint grid with a radial falloff.
 * The grid feeds the scatter (sr_draw_foliage) so painted-sparse areas thin out. */
static struct {
    bool  armed;
    float radius;       /* world units */
    float strength;     /* 0..1 per stroke-frame */
    bool  erase;        /* lower density (true) vs raise (false) */
    bool  stroke_open;
    bool  stroke_has_point;
    float stroke_x;
    float stroke_z;
} g_veg_brush = { false, 8.0f, 0.5f, true, false };

static void foliage_brush_disarm(void)
{
    if (g_veg_brush.stroke_open)
        jce_state_end_batch_edit();
    g_veg_brush.armed = false;
    g_veg_brush.stroke_open = false;
    g_veg_brush.stroke_has_point = false;
}

extern "C" bool jce_foliage_brush_armed(void)
{
    if (!g_veg_brush.armed) return false;
    JceScene *scene = jce_state_get_scene();
    const uint32_t focused = jce_state_get_focused();
    if (!scene || !focused) {
        foliage_brush_disarm();
        return false;
    }
    JceVegetationScatterComponent *scatter =
        jce_scene_get_vegetation_scatter(
            scene, jce_state_to_ecs_entity(focused));
    if (scatter && scatter->density_paint_active) return true;

    foliage_brush_disarm();
    return false;
}

extern "C" void jce_foliage_brush_set_armed(bool armed)
{
    if (!armed) {
        foliage_brush_disarm();
        return;
    }
    g_veg_brush.armed = armed;
}

extern "C" float jce_foliage_brush_radius(void)
{
    return g_veg_brush.radius;
}

extern "C" void jce_foliage_brush_end_stroke(void)
{
    if (g_veg_brush.stroke_open) {
        jce_state_end_batch_edit();
        g_veg_brush.stroke_open = false;
    }
    g_veg_brush.stroke_has_point = false;
}

static void foliage_brush_stamp(JceVegetationScatterComponent *vs,
                                float origin_x, float origin_z,
                                float wx, float wz, float dt)
{
    float ax = vs->area_x > 0.01f ? vs->area_x : 1.0f;
    float az = vs->area_z > 0.01f ? vs->area_z : 1.0f;
    const int DIM = JCE_VEG_PAINT_DIM;
    float cu  = ((wx - origin_x) / ax + 0.5f) * (float)DIM;
    float cv  = ((wz - origin_z) / az + 0.5f) * (float)DIM;
    float rcx = (g_veg_brush.radius / ax) * (float)DIM;
    float rcz = (g_veg_brush.radius / az) * (float)DIM;
    if (rcx < 0.5f) rcx = 0.5f;
    if (rcz < 0.5f) rcz = 0.5f;

    float per = g_veg_brush.strength * 255.0f *
                (dt > 0.0f ? (dt * 6.0f < 1.0f ? dt * 6.0f : 1.0f) : 1.0f);
    int x0 = (int)floorf(cu - rcx), x1 = (int)ceilf(cu + rcx);
    int z0 = (int)floorf(cv - rcz), z1 = (int)ceilf(cv + rcz);
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > DIM - 1) x1 = DIM - 1;
    if (z1 > DIM - 1) z1 = DIM - 1;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const float dx = ((x + 0.5f) - cu) * ax / (float)DIM;
            const float dz = ((z + 0.5f) - cv) * az / (float)DIM;
            const float distance = sqrtf(dx * dx + dz * dz);
            if (distance > g_veg_brush.radius) continue;
            int delta = (int)(per *
                (1.0f - distance / g_veg_brush.radius));
            int v = (int)vs->density_paint[z * DIM + x] +
                    (g_veg_brush.erase ? -delta : delta);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            vs->density_paint[z * DIM + x] = (uint8_t)v;
        }
}

extern "C" void jce_foliage_brush_apply_world(float wx, float wz, float dt)
{
    JceScene *scene = jce_state_get_scene();
    uint32_t  fid   = jce_state_get_focused();
    if (!scene || !fid) return;
    JceEntity e = jce_state_to_ecs_entity(fid);
    JceVegetationScatterComponent *vs = jce_scene_get_vegetation_scatter(scene, e);
    if (!vs || !vs->density_paint_active) return;

    if (!g_veg_brush.stroke_open) {
        jce_state_begin_batch_edit();
        g_veg_brush.stroke_open = true;
    }

    jce_mat4 world = jce_scene_get_world_matrix(scene, e);
    const float origin_x = world.col[3].x;
    const float origin_z = world.col[3].z;
    const float dx = g_veg_brush.stroke_has_point
        ? wx - g_veg_brush.stroke_x : 0.0f;
    const float dz = g_veg_brush.stroke_has_point
        ? wz - g_veg_brush.stroke_z : 0.0f;
    const float distance = sqrtf(dx * dx + dz * dz);
    const float spacing = g_veg_brush.radius * 0.25f > 0.25f
        ? g_veg_brush.radius * 0.25f : 0.25f;
    int stamp_count = g_veg_brush.stroke_has_point
        ? (int)ceilf(distance / spacing) : 1;
    if (stamp_count < 1) stamp_count = 1;
    if (stamp_count > 256) stamp_count = 256;
    const float stamp_dt = dt > 0.0f ? dt / (float)stamp_count : dt;

    for (int i = 1; i <= stamp_count; ++i) {
        const float t = g_veg_brush.stroke_has_point
            ? (float)i / (float)stamp_count : 1.0f;
        const float stamp_x = g_veg_brush.stroke_has_point
            ? g_veg_brush.stroke_x + dx * t : wx;
        const float stamp_z = g_veg_brush.stroke_has_point
            ? g_veg_brush.stroke_z + dz * t : wz;
        foliage_brush_stamp(vs, origin_x, origin_z,
                            stamp_x, stamp_z, stamp_dt);
    }
    g_veg_brush.stroke_x = wx;
    g_veg_brush.stroke_z = wz;
    g_veg_brush.stroke_has_point = true;
    jce_state_mark_scene_modified();
}

void draw_comp_vegetation_scatter(JceVegetationScatterComponent *vs,
                                  JceScene *scene, JceEntity entity)
{
    if (!vs) return;
    bool ch = false;

    ImGui::TextUnformatted(jce_editor_i18n("inspector.vegetationScatter.mesh"));
    if (jce_draw_path_input_asset("##veg_mesh", vs->mesh_path,
                                  sizeof vs->mesh_path, JCE_ASSET_KIND_MODEL))
        ch = true;

    /* No mesh asset => scatter a built-in PRIMITIVE (the instanced-primitive ISM
     * path: one GPU-instanced submit for N shapes, Unity-ISM / UE-HISM level).
     * Pick the shape here; order matches JceVegetationScatterComponent.mesh_shape
     * (0=cube,1=sphere,2=plane,3=capsule,4=cylinder). */
    if (vs->mesh_path[0] == '\0') {
        static const char *const kShapes[] = { "Cube", "Sphere", "Plane", "Capsule", "Cylinder" };
        int shp = (vs->mesh_shape < 0 || vs->mesh_shape > 4) ? 0 : vs->mesh_shape;
        ImGui::TextUnformatted(jce_editor_i18n_id("inspector.vegetationScatter.primitive",
                                                  "Primitive (no mesh)"));
        if (ImGui::Combo("##veg_primitive", &shp, kShapes, 5)) {
            vs->mesh_shape = shp; ch = true;
        }
    }

    ImGui::TextUnformatted(jce_editor_i18n("inspector.vegetationScatter.albedo"));
    if (jce_draw_path_input_asset("##veg_albedo", vs->albedo_path,
                                  sizeof vs->albedo_path, JCE_ASSET_KIND_TEXTURE))
        ch = true;

    /* Density mask (large-world #8a): a grayscale texture whose R channel over
     * the area rect modulates per-instance keep-probability — sparse where dark,
     * dense where bright.  Empty = uniform density. */
    ImGui::TextUnformatted(jce_editor_i18n_id("inspector.vegetationScatter.densityMask",
                                              "Density Mask (grayscale)"));
    if (jce_draw_path_input_asset("##veg_density_mask", vs->density_mask_path,
                                  sizeof vs->density_mask_path, JCE_ASSET_KIND_TEXTURE))
        ch = true;

    /* In-editor density paint brush (large-world #8a): paint the density grid
     * directly on the terrain in Scene View (supersedes the mask asset). */
    ImGui::Separator();
    bool was_paint = vs->density_paint_active;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vegetationScatter.densityPaint",
                                           "Density Paint (in-editor)"),
                        &vs->density_paint_active)) {
        ch = true;
        if (vs->density_paint_active && !was_paint)
            memset(vs->density_paint, 255, sizeof vs->density_paint);  /* full on enable */
        if (!vs->density_paint_active)
            jce_foliage_brush_set_armed(false);
    }
    if (vs->density_paint_active) {
        /* One-time restore of the persisted brush knobs (user-global, so
         * the brush feel carries across projects and restarts). */
        static bool s_brush_loaded = false;
        if (!s_brush_loaded) {
            s_brush_loaded = true;
            g_veg_brush.radius   = jce_editor_ui_state_load_float(
                "brush.foliage.radius", g_veg_brush.radius, 0.5f, 128.0f);
            g_veg_brush.strength = jce_editor_ui_state_load_float(
                "brush.foliage.strength", g_veg_brush.strength, 0.0f, 1.0f);
            g_veg_brush.erase    = jce_editor_ui_state_load_int(
                "brush.foliage.erase", g_veg_brush.erase ? 1 : 0, 0, 1) != 0;
        }

        bool armed = g_veg_brush.armed;
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vegetationScatter.paintInScene",
                                               "Paint in Scene"), &armed)) {
            jce_foliage_brush_set_armed(armed);
            if (armed) jce_terrain_panel_set_brush_armed(false);
        }
        if (ImGui::SliderFloat(jce_editor_i18n_id("inspector.vegetationScatter.brushRadius",
                                                  "Brush Radius"),
                               &g_veg_brush.radius, 0.5f, 128.0f, "%.1f"))
            jce_editor_ui_state_save_float("brush.foliage.radius", g_veg_brush.radius);
        if (ImGui::SliderFloat(jce_editor_i18n_id("inspector.vegetationScatter.brushStrength",
                                                  "Brush Strength"),
                               &g_veg_brush.strength, 0.0f, 1.0f, "%.2f"))
            jce_editor_ui_state_save_float("brush.foliage.strength", g_veg_brush.strength);
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vegetationScatter.brushErase",
                                               "Erase (lower density)"), &g_veg_brush.erase))
            jce_editor_ui_state_save_int("brush.foliage.erase", g_veg_brush.erase ? 1 : 0);
        if (ImGui::Button(jce_editor_i18n_id("inspector.vegetationScatter.fillFull", "Fill Full"))) {
            memset(vs->density_paint, 255, sizeof vs->density_paint); ch = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n_id("inspector.vegetationScatter.clearEmpty", "Clear"))) {
            memset(vs->density_paint, 0, sizeof vs->density_paint); ch = true;
        }
        ImGui::TextDisabled("(%s)", jce_editor_i18n_id("inspector.vegetationScatter.paintNote",
            "Arm 'Paint in Scene' + drag on the terrain to carve density."));
    }

    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.vegetationScatter.density"),
                           &vs->density, 0.05f, 0.0f, 1000.0f, "%.2f");

    int seed = (int)vs->seed;
    if (ImGui::DragInt(jce_editor_i18n("inspector.vegetationScatter.seed"),
                       &seed, 1.0f, 0, 0x7FFFFFFF)) {
        vs->seed = (uint32_t)(seed < 0 ? 0 : seed);
        ch = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inspector.vegetationScatter.reseed"))) {
        vs->seed = vs->seed * 1664525u + 1013904223u;   /* LCG step */
        ch = true;
    }

    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.vegetationScatter.areaX"),
                           &vs->area_x, 0.5f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.vegetationScatter.areaZ"),
                           &vs->area_z, 0.5f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.vegetationScatter.maxSlope"),
                           &vs->max_slope_deg, 0.5f, 0.0f, 90.0f, "%.1f");
    ch |= ImGui::DragFloatRange2(jce_editor_i18n("inspector.vegetationScatter.scaleRange"),
                                 &vs->scale_min, &vs->scale_max, 0.01f, 0.01f, 100.0f, "%.2f");
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.vegetationScatter.tint"), vs->tint);
    insp_unwired_field_badge();   /* tint */
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.vegetationScatter.alignNormal"),
                          &vs->align_to_normal);
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.vegetationScatter.castShadow"),
                          &vs->cast_shadow);
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.vegetationScatter.visible"),
                          &vs->visible);

    long est = (long)((double)vs->density * (double)vs->area_x * (double)vs->area_z);
    ImGui::TextDisabled("%s: %ld",
                        jce_editor_i18n("inspector.vegetationScatter.estimate"), est);

    if (ch) insp_track_edit();
    /* ── Bake placement ────────────────────────────────────────────────
     *
     * Scatter is deterministic, so a level can compute its instances at load
     * time or ship them precomputed.  Baking makes the cost stop scaling with
     * instance count and -- more usefully -- lets an artist bake, inspect, and
     * know that what ships is what was reviewed.
     *
     * This is only sound because the surface rules consume no randomness: a
     * cooked list and a live scatter at the same seed agree instance for
     * instance.  See jce_foliage.h. */
    ImGui::Separator();
    ImGui::SeparatorText(jce_editor_i18n("inspector.vegetationScatter.bake"));
    {
        static int  s_baked = -1;      /* -1 = not attempted this session */
        static JceEntity s_bake_entity = 0;

        ImGui::TextUnformatted(
            jce_editor_i18n("inspector.vegetationScatter.bake"));
        if (jce_draw_path_input_asset("##veg_baked_placement",
                                      vs->baked_placement_path,
                                      sizeof vs->baked_placement_path,
                                      JCE_ASSET_KIND_DATA))
            ch = true;
        insp_track_edit();
        /* TRUE TODAY, AND MEANT TO BE DELETED: nothing under engine/src reads
         * baked_placement_path yet, so this control is inert.  The field is
         * in-flight work; when its reader lands,
         * check_authored_path_consumed.py fails on the now-stale exemption in
         * authored_path_exempt.txt, and this badge goes with that line. */
        insp_unwired_field_badge();
        if (vs->baked_placement_path[0]) {
            ImGui::SameLine();
            if (ImGui::SmallButton(
                    jce_editor_i18n("inspector.vegetationScatter.clearEmpty"))) {
                jce_state_begin_batch_edit();
                vs->baked_placement_path[0] = '\0';
                jce_state_end_batch_edit();
                s_baked = -1;
                s_bake_entity = 0;
            }
        }

        if (ImGui::Button(jce_editor_i18n("inspector.vegetationScatter.bakeNow"))) {
            s_baked = -1;
            s_bake_entity = entity;

            JceFoliageScatterParams fp;
            memset(&fp, 0, sizeof fp);
            fp.seed          = vs->seed;
            fp.density       = vs->density;
            fp.area_x        = vs->area_x;
            fp.area_z        = vs->area_z;
            fp.max_slope_deg = vs->max_slope_deg;
            fp.scale_min     = vs->scale_min;
            fp.scale_max     = vs->scale_max;
            fp.want_normals  = vs->align_to_normal;

            /* The entity's own world position is the scatter origin, exactly
             * as the renderer uses it -- baking around a different origin
             * would place a forest that does not match what is on screen. */
            jce_vec3 origin = jce_v3(0.0f, 0.0f, 0.0f);
            if (scene && entity) {
                const jce_mat4 m = jce_scene_get_world_matrix(scene, entity);
                origin = jce_v3(m.col[3].x, m.col[3].y, m.col[3].z);
            }

            std::vector<JceFoliageInstance> inst(JCE_FOLIAGE_MAX_INSTANCES);
            const uint32_t n = jce_foliage_scatter(
                &fp, jce_terrain_panel_get_terrain(), &origin,
                inst.data(), (uint32_t)inst.size());

            std::vector<unsigned char> blob(jce_foliage_cook_size(n));
            if (jce_foliage_cook(inst.data(), n, fp.seed,
                                 blob.data(), blob.size(), nullptr)) {
                char rel[256];
                snprintf(rel, sizeof rel,
                         "generated/foliage/vegetation_%u_%u.foliage.bin",
                         (unsigned)entity, vs->seed);
                char abs_path[512];
                if (jce_editor_resolve_asset_path(rel, abs_path, sizeof abs_path) &&
                    jce_fs_host_write_all_atomic(abs_path, blob.data(),
                                                 (uint64_t)blob.size())) {
                    jce_state_begin_batch_edit();
                    snprintf(vs->baked_placement_path,
                             sizeof vs->baked_placement_path, "%s", rel);
                    jce_state_end_batch_edit();
                    s_baked = (int)n;
                } else {
                    s_baked = -2;      /* wrote nothing: report, never pretend */
                }
            } else {
                s_baked = -2;
            }
        }

        if (s_bake_entity == entity && s_baked >= 0)
            ImGui::Text(jce_editor_i18n("inspector.vegetationScatter.bakeOk"),
                        s_baked, vs->baked_placement_path);
        else if (s_bake_entity == entity && s_baked == -2)
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.vegetationScatter.bakeFail"));
    }

}

void draw_comp_foliage_cluster(JceFoliageClusterComponent *fc)
{
    if (!fc) return;
    bool ch = false;

    ch |= ImGui::DragInt(jce_editor_i18n("inspector.foliageCluster.leafCount"),
                         &fc->leaf_count, 1, 1, 256);
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.foliageCluster.radius"),
                           &fc->radius, 0.05f, 0.05f, 50.0f, "%.2f");
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.foliageCluster.squashY"),
                             &fc->squash_y, 0.1f, 1.5f, "%.2f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.foliageCluster.leafScale"),
                           &fc->leaf_scale, 0.02f, 0.05f, 20.0f, "%.2f");
    uint32_t seed_min = 0, seed_max = 0xFFFFFFFFu;
    ch |= ImGui::DragScalar(jce_editor_i18n("inspector.foliageCluster.seed"),
                            ImGuiDataType_U32, &fc->seed, 1.0f, &seed_min, &seed_max);

    ImGui::Separator();
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.foliageCluster.shadow"),    fc->shadow_color);
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.foliageCluster.mid"),       fc->mid_color);
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.foliageCluster.highlight"), fc->highlight_color);
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.foliageCluster.multiplier"), fc->color_multiplier);

    ImGui::Separator();
    /* Leaf alpha map: full asset input (browse dialog + drag-drop from the
     * Asset Browser), matching every other texture-path field — this was a
     * bare InputText, the ES bush workflow's only hand-typed path. */
    ImGui::TextUnformatted(jce_editor_i18n("inspector.foliageCluster.alphaTex"));
    if (jce_draw_path_input_asset("##fc_alpha_tex", fc->alpha_tex,
                                  sizeof fc->alpha_tex, JCE_ASSET_KIND_TEXTURE))
        ch = true;
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.visible"), &fc->visible);

    if (ch) insp_track_edit();
}

void draw_comp_grass_field(JceGrassFieldComponent *g)
{
    if (!g) return;
    bool ch = false;

    /* --- Scatter --- */
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.density"),
                           &g->density, 0.05f, 0.001f, 10000.0f, "%.3f");

    uint32_t seed_step = 1;
    uint32_t seed_min  = 0;
    uint32_t seed_max  = 0xFFFFFFFFu;
    if (ImGui::DragScalar(jce_editor_i18n("inspector.grassField.seed"),
                          ImGuiDataType_U32, &g->seed, 1.0f, &seed_min, &seed_max))
        ch = true;
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inspector.grassField.reseed"))) {
        g->seed = g->seed * 1664525u + 1013904223u;
        ch = true;
    }

    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.areaX"),
                           &g->area_x, 0.5f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.areaZ"),
                           &g->area_z, 0.5f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.grassField.maxSlope"),
                             &g->max_slope_deg, 0.0f, 90.0f, "%.1f");

    /* --- Blade geometry --- */
    ImGui::Separator();
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.scaleMin"),
                           &g->scale_min, 0.01f, 0.0f, 100.0f, "%.3f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.scaleMax"),
                           &g->scale_max, 0.01f, 0.0f, 100.0f, "%.3f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.bladeHeight"),
                           &g->blade_height, 0.01f, 0.0f, 100.0f, "%.3f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.bladeWidth"),
                           &g->blade_width, 0.001f, 0.0f, 10.0f, "%.4f");
    ch |= ImGui::SliderInt(jce_editor_i18n("inspector.grassField.cards"),
                           &g->cards, 3, 6);

    /* --- Color --- */
    ImGui::Separator();
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.grassField.rootColor"), g->root_color);
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.grassField.tipColor"),  g->tip_color);
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.grassField.hueJitter"),
                             &g->hue_jitter, 0.0f, 1.0f, "%.3f");

    /* --- Wind --- */
    ImGui::Separator();
    ch |= ImGui::DragFloat2(jce_editor_i18n("inspector.grassField.windDir"),
                            g->wind_dir, 0.01f, -1.0f, 1.0f, "%.3f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.windSpeed"),
                           &g->wind_speed, 0.01f, 0.0f, 100.0f, "%.3f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.windAmplitude"),
                           &g->wind_amplitude, 0.005f, 0.0f, 10.0f, "%.4f");

    /* --- Fade --- */
    ImGui::Separator();
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.fadeStart"),
                           &g->fade_start, 1.0f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.grassField.fadeEnd"),
                           &g->fade_end, 1.0f, 0.0f, 100000.0f, "%.1f");

    /* --- Density mask (paths / ponds read through the grass) ---
     * Serialized + consumed by the renderer (the ES pond scene carves its
     * walkways with it) but previously had no inspector UI — the only way
     * to author it was hand-editing the scene JSON. */
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or("inspector.grassField.densityMask",
                                              "Density Mask (G channel gates blades)"));
    if (jce_draw_path_input_asset("##gf_density_mask", g->density_mask_path,
                                  sizeof g->density_mask_path,
                                  JCE_ASSET_KIND_TEXTURE))
        ch = true;
    if (g->density_mask_path[0]) {
        ch |= ImGui::SliderFloat(jce_editor_i18n_id("inspector.grassField.densityThreshold",
                                                    "Mask Threshold"),
                                 &g->density_threshold, 0.0f, 1.0f, "%.2f");
        ch |= ImGui::DragFloat(jce_editor_i18n_id("inspector.grassField.maskWorldSize",
                                                  "Mask World Size"),
                               &g->mask_world_size, 0.5f, 0.0f, 100000.0f, "%.1f");
        ImGui::TextDisabled("%s", jce_editor_i18n_or("inspector.grassField.maskHint",
            "Blades survive where mask.g >= threshold; 0 world size = area rect."));
    }

    /* --- Flags --- */
    ImGui::Separator();
    {
        /* Live now: blades join the CSM depth pass through the same
         * instanced shadow program the vegetation scatter already drives.
         * The BeginDisabled(true), the unwired badge and the "(v1: reserved)"
         * label are all gone together -- a control that works while still
         * wearing three signs saying it does not is the same lie backwards. */
        ch |= ImGui::Checkbox(jce_editor_i18n("inspector.grassField.castShadow"), &g->cast_shadow);
    }
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.grassField.visible"), &g->visible);

    /* Estimate blade count */
    long est = (long)((double)g->density * (double)g->area_x * (double)g->area_z);
    ImGui::TextDisabled("%s: %ld", jce_editor_i18n("inspector.grassField.estimate"), est);

    if (ch) insp_track_edit();
}

void draw_comp_water(JceWaterComponent *w)
{
    if (!w) return;
    bool ch = false;

    /* Simulation mode: Gerstner (sum-of-sines, the default) or FFT ocean
     * (Tessendorf, consumed by jce_water_fft.c).  The mode-specific UI below
     * follows the selection; the inactive set's fields stay serialized so a
     * round-trip is lossless. */
    const char *mode_names[] = {
        jce_editor_i18n("inspector.water.mode.gerstner"),
        jce_editor_i18n("inspector.water.mode.fft"),
        jce_editor_i18n("inspector.water.mode.stylized"),
    };
    if (ImGui::Combo(jce_editor_i18n("inspector.water.mode"),
                     &w->water_mode, mode_names, 3)) {
        if (w->water_mode < JCE_WATER_MODE_GERSTNER) w->water_mode = JCE_WATER_MODE_GERSTNER;
        if (w->water_mode > JCE_WATER_MODE_STYLIZED) w->water_mode = JCE_WATER_MODE_STYLIZED;
        ch = true;
    }

    /* The engine explicitly defers this one to the scene -- jce_sr_environment.c
     * at the water draw: "Authored, default off ... Without a depth value at
     * water pixels, SSR / DOF / aerial fog all read whatever lies BEHIND the
     * surface instead of the surface itself; with one, translucent geometry
     * further away is depth-rejected.  Neither is universally right, so the
     * scene decides rather than the engine."
     *
     * The scene had no way to decide: the field was serialised and read every
     * frame, and no control existed anywhere in the editor. */
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.water.depthWrite"),
                          &w->depth_write);
    insp_track_edit();

    /* PLANAR REFLECTION.  Off by default and next to a note about its cost:
     * it is a SECOND scene render, which is a different order of expense from
     * every other checkbox on this component. */
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.water.planarReflection"),
                          &w->planar_reflection);
    insp_track_edit();
    ImGui::BeginDisabled(!w->planar_reflection);
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.planarIntensity"),
                           &w->planar_intensity, 0.01f, 0.0f, 2.0f, "%.2f");
    insp_track_edit();
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.water.planarNote"));

    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.sizeX"),
                           &w->size_x, 1.0f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.sizeZ"),
                           &w->size_z, 1.0f, 0.0f, 100000.0f, "%.1f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.baseHeight"),
                           &w->base_height, 0.05f, -100000.0f, 100000.0f, "%.2f");

    if (w->water_mode == JCE_WATER_MODE_GERSTNER) {
        if (ImGui::SliderInt(jce_editor_i18n("inspector.water.waveCount"),
                             &w->wave_count, 0, JCE_WATER_COMP_MAX_WAVES)) {
            if (w->wave_count < 0) w->wave_count = 0;
            if (w->wave_count > JCE_WATER_COMP_MAX_WAVES) w->wave_count = JCE_WATER_COMP_MAX_WAVES;
            ch = true;
        }

        for (int i = 0; i < w->wave_count; ++i) {
            ImGui::PushID(i);
            if (ImGui::TreeNodeEx("##wave", ImGuiTreeNodeFlags_DefaultOpen, "%s %d",
                                  jce_editor_i18n("inspector.water.wave"), i)) {
                JceWaterWave *wv = &w->waves[i];
                ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.amplitude"),
                                       &wv->amplitude, 0.01f, 0.0f, 1000.0f, "%.3f");
                ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.wavelength"),
                                       &wv->wavelength, 0.1f, 0.001f, 100000.0f, "%.2f");
                ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.speed"),
                                       &wv->speed, 0.05f, -1000.0f, 1000.0f, "%.2f");
                float dir[2] = { wv->dir_x, wv->dir_z };
                if (ImGui::DragFloat2(jce_editor_i18n("inspector.water.direction"),
                                      dir, 0.02f, -1.0f, 1.0f, "%.2f")) {
                    wv->dir_x = dir[0]; wv->dir_z = dir[1]; ch = true;
                }
                ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.water.steepness"),
                                         &wv->steepness, 0.0f, 1.0f, "%.2f");
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    } else if (w->water_mode == JCE_WATER_MODE_STYLIZED) {
        /* Hand-painted ripple overlay: the body color is in the ground, this
         * plane draws only shore strokes / splashes / ice from a data map. */
        ImGui::SeparatorText(jce_editor_i18n("inspector.water.stylized.header"));
        ch |= jce_draw_path_input_asset(jce_editor_i18n("inspector.water.dataTex"),
                  w->data_tex, sizeof w->data_tex, JCE_ASSET_KIND_TEXTURE);
        ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.water.splashRatio"),
                                 &w->splash_ratio, 0.0f, 1.0f, "%.2f");
    } else { /* JCE_WATER_MODE_FFT */
        ImGui::SeparatorText(jce_editor_i18n("inspector.water.fft.header"));
        /* Open-ocean geometry.  Off for a pond, where a uniform grid is the
         * better mesh, not merely the legacy one. */
        ch |= ImGui::Checkbox(jce_editor_i18n("inspector.water.ocean"),
                              &w->ocean);
        ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.fft.patchSize"),
                               &w->fft_patch_size, 1.0f, 1.0f, 100000.0f, "%.1f");
        ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.fft.windSpeed"),
                               &w->fft_wind_speed, 0.1f, 0.0f, 1000.0f, "%.2f");
        float wind[2] = { w->fft_wind_dir_x, w->fft_wind_dir_z };
        if (ImGui::DragFloat2(jce_editor_i18n("inspector.water.fft.windDir"),
                              wind, 0.02f, -1.0f, 1.0f, "%.2f")) {
            w->fft_wind_dir_x = wind[0]; w->fft_wind_dir_z = wind[1]; ch = true;
        }
        ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.fft.amplitude"),
                               &w->fft_amplitude, 0.0005f, 0.0f, 1000.0f, "%.4f");
        /* Fetch: 0 keeps raw Phillips.  Left at 0 by default ON PURPOSE --
         * switching spectra changes every height value, so it is a deliberate
         * choice by the author rather than something that drifts in. */
        ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.fft.fetch"),
                               &w->fft_fetch, 100.0f, 0.0f, 1000000.0f, "%.0f");
        if (w->fft_fetch <= 0.0f)
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.water.fft.fetchOff"));
        else
            ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.fft.swell"),
                                   &w->fft_swell, 0.01f, 0.0f, 1.0f, "%.2f");
        const char *res_names[] = { "32", "64", "128", "256" };
        const int   res_values[] = { 32, 64, 128, 256 };
        int res_idx = 1; /* default to 64 if unset/out-of-range */
        for (int i = 0; i < 4; ++i)
            if (w->fft_resolution == res_values[i]) { res_idx = i; break; }
        if (ImGui::Combo(jce_editor_i18n("inspector.water.fft.resolution"),
                         &res_idx, res_names, 4)) {
            w->fft_resolution = res_values[res_idx];
            ch = true;
        }
    }

    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.water.colorShallow"), w->color_shallow);
    ch |= ImGui::ColorEdit3(jce_editor_i18n("inspector.water.colorDeep"), w->color_deep);
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.water.transparency"),
                             &w->transparency, 0.0f, 1.0f, "%.2f");
    /* Beer-Lambert clarity: 0 = off.  Off is the default because absorption
     * changes how every existing water body looks, and a scene tuned against
     * the old view-angle gradient must keep rendering as authored. */
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.clarity"),
                           &w->clarity, 0.1f, 0.0f, 200.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.water.clarityTip"));
    /* Caustics are finite-differenced from the FFT displacement map, so
     * GERSTNER and STYLIZED have no input to differentiate and the term is
     * inert there.  The slider used to look identical in all three modes: a
     * designer dragging it on a pond got no picture and no reason.  Disabled
     * (not hidden) and NOT reset, matching this panel's own rule that the
     * inactive mode's fields stay serialized so a round-trip is lossless --
     * switching back to FFT must return the value the author set.
     * The disable+EndDisabled+TextDisabled shape is the one this file already
     * uses for an inert control -- see inspector.pe.gpuUnsupported in
     * draw_comp_particle_emitter below. */
    const bool caustics_ok = (w->water_mode == JCE_WATER_MODE_FFT);
    if (!caustics_ok) ImGui::BeginDisabled();
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.water.caustics"),
                             &w->caustics, 0.0f, 1.0f, "%.2f");
    if (!caustics_ok) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s",
            jce_editor_i18n("inspector.water.causticsFftOnly"));
    }
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.shoreFoam"),
                           &w->shore_foam_m, 0.05f, 0.0f, 50.0f, "%.2f m");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.shoreSurge"),
                           &w->shore_surge_s, 0.1f, 0.0f, 60.0f, "%.1f s");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.sunSpecular"),
                           &w->sun_specular, 0.02f, 0.0f, 100.0f, "%.2f");
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.water.shoreRipple"),
                             &w->shore_ripple, 0.0f, 1.0f, "%.2f");
    ch |= ImGui::SliderFloat(jce_editor_i18n("inspector.water.iceRatio"),
                             &w->ice_ratio, 0.0f, 1.0f, "%.2f");
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.water.visible"), &w->visible);

    if (ch) insp_track_edit();
}

void draw_comp_buoyancy(JceBuoyancyComponent *b)
{
    if (!b) return;
    bool ch = false;

    ImGui::TextWrapped("%s", jce_editor_i18n("inspector.buoyancy.hint"));
    ch |= ImGui::Checkbox(jce_editor_i18n("inspector.buoyancy.enabled"), &b->enabled);
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.buoyancy.strength"),
                           &b->buoyancy_strength, 0.5f, 0.0f, 100000.0f, "%.2f");
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.buoyancy.drag"),
                           &b->drag, 0.05f, 0.0f, 1000.0f, "%.2f");

    if (ch) insp_track_edit();
}

void draw_comp_particle_emitter(JceParticleEmitterComponent *pe)
{
    if (!pe) return;

    /* Authored asset (*.particles.json) drives the runtime emitter.  When
     * set, the legacy quick-tune fields below are ignored at runtime. */
    ImGui::TextUnformatted(jce_editor_i18n("inspector.pe.asset"));
    if (jce_draw_path_input_asset("##pe_asset", pe->asset_path,
                                  sizeof(pe->asset_path), JCE_ASSET_KIND_PARTICLE))
        insp_track_edit();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("inspector.pe.clearAsset", "pe")))
        insp_undo_set(&pe->asset_path[0], '\0');

    const bool has_asset = pe->asset_path[0] != '\0';
    if (has_asset) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.pe.assetDrives"));
        ImGui::BeginDisabled();
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.pe.legacyHint"));
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.emitRate", "pe"), &pe->emit_rate, 0.5f, 0.0f, 10000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.lifetimeMin", "pe"), &pe->lifetime_min, 0.05f, 0.0f, 1000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.lifetimeMax", "pe"), &pe->lifetime_max, 0.05f, 0.0f, 1000.0f);
    insp_track_edit();
    if (pe->lifetime_max < pe->lifetime_min) pe->lifetime_max = pe->lifetime_min;
    if (has_asset) ImGui::EndDisabled();

    /* GPU simulation request (persisted; silently falls back to CPU when
     * unsupported).  The GPU path is always world-space with the procedural
     * sprite — the authored texture / worldSpace=false stay CPU-only. */
    const bool compute_ok =
        (jce_renderer_get_caps() & JCE_CAP_COMPUTE) != 0;
    if (!compute_ok) ImGui::BeginDisabled();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.pe.gpu", "pe"), &pe->gpu))
        insp_undo_bool(&pe->gpu);
    if (!compute_ok) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.pe.gpuUnsupported"));
    } else if (pe->gpu &&
               !jce_render_pipeline_is_feature_enabled("gpu_particles")) {
        /* The checkbox stays editable — it is a persisted request; only the
         * pipeline gate is currently off. */
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.pe.gpuPipelineOff"));
    }

    ImGui::TextDisabled(jce_editor_i18n("inspector.pe.useParticleSystemPanel"));
}

/* The language / backend diagnostics, split out of draw_comp_script so the
 * parameter table can follow them.  It keeps its own three early returns --
 * they end the DIAGNOSTIC, and a script whose extension nothing claims still
 * has authored parameters worth editing. */
static void draw_script_language_note(JceScriptComponent *scr)
{
    /* TWO INDEPENDENT QUESTIONS, and therefore two messages that never share
     * a string, because they have two different fixes:
     *
     *   1. does ANY language claim this extension?  Answered offline by the
     *      engine's script-extension authority, so it reads the same in an
     *      editor built with no backend at all.  Fix: author the file in a
     *      language the engine ships.
     *   2. did THIS executable link a VM for that language?  Answered by the
     *      registry each backend populates from its own register().  Fix:
     *      configure the build with that backend ON.
     *
     * Collapsing them is how "turret.py does nothing" became a silent no-op
     * at runtime instead of a sentence in the editor.
     *
     * A path with no extension is question 1's NULL too, and correctly so:
     * nothing can be promised about a bare name from the path alone.  A C++
     * script is NOT that case — it names its class through ".jcecpp", which
     * the catalog carries — so a working C++ script no longer reads amber
     * here, which it did for as long as the cpp backend claimed nothing and a
     * project had to invent its own extension at runtime. */
    /* Same call the picker filters on, so "offered" and "accepted" are one
     * set by construction: a path this returns NULL for is a path the picker
     * will not list, and vice versa. */
    const char *lang = jce_assetdb_script_language(scr->script_path);
    if (!lang) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("inspector.script.noLanguage"));
        return;
    }

    ImGui::TextDisabled("%s: %s",
                        jce_editor_i18n("inspector.script.language"), lang);
    if (jce_script_vm_find(lang))
        return;

    /* Worded about the EDITOR, not about "this build", and that distinction
     * is the whole point of the line.
     *
     * A backend registers itself from its own register() (jce_script_vm.h).
     * This process calls several — jce_editor_register_script_backends()
     * registers the cpp VM plus Python and Java when the editor was built
     * with them — so the set below is real and it is THIS PROCESS'S.  It is not the game's.  The
     * game links its own backends and makes its own register() calls, and a
     * project can additionally claim a private extension at runtime
     * (jce_script_vm_register_extension), which no editor has loaded.
     *
     * So "not registered here" is never "will not run there".  Saying "the
     * script will never run" would be a verdict on a packaged executable this
     * UI cannot see, and would be false for a game that links the backend
     * correctly — which is the normal case for Java, whose build option
     * defaults OFF (scripting/java/CMakeLists.txt) while a shipped game turns
     * it on.  Informational, not a defect, so it is dimmed rather than amber
     * — unlike the unclaimed-extension case above, which nothing anywhere
     * can run.
     *
     * (Before a7a2dbfc the editor registered nothing and this comment said
     * so.  Kept current deliberately: a reader who believes the editor
     * registers no backend concludes Play cannot run Python, which is now
     * the opposite of true.) */
    ImGui::TextDisabled("%s",
                        jce_editor_i18n("inspector.script.backendMissing"));

    /* Name what IS registered, for the same reason rt_script_create_vm() logs
     * it: "not registered" without the registered set leaves the reader
     * guessing whether the backend, the claim, or the spelling is wrong. */
    char have[192];
    size_t used = 0;
    have[0] = '\0';
    const int count = jce_script_vm_count();
    for (int i = 0; i < count && used + 1 < sizeof(have); ++i) {
        const char *name = jce_script_vm_language_at(i);
        if (!name)
            continue;
        const int written = snprintf(have + used, sizeof(have) - used,
                                     "%s%s", used ? ", " : "", name);
        if (written <= 0)
            break;
        used += (size_t)written;
        if (used >= sizeof(have)) {
            used = sizeof(have) - 1;
            break;
        }
    }
    ImGui::TextDisabled("%s %s",
                        jce_editor_i18n("inspector.script.backendsPresent"),
                        have);
}

/* One row's kind, spelled for a human.  Read every frame rather than cached:
 * the locale can change without the scene changing, and a cache would keep
 * showing the old language until something else marked the panel dirty. */
static const char *script_param_kind_label(int kind)
{
    switch (kind) {
    case JCE_SCRIPT_PARAM_BOOL:   return jce_editor_i18n("inspector.script.kindBool");
    case JCE_SCRIPT_PARAM_TEXT:   return jce_editor_i18n("inspector.script.kindText");
    case JCE_SCRIPT_PARAM_ENTITY: return jce_editor_i18n("inspector.script.kindEntity");
    default:                      return jce_editor_i18n("inspector.script.kindNumber");
    }
}

/* Read the script file and return what it DECLARES via @export.
 *
 * Project-relative like every other asset path on a component; an empty or
 * unreadable path yields zero declarations, which the caller renders as "this
 * script declares nothing" rather than as an error -- a script with no
 * @export lines is the normal case and must not look broken.
 *
 * Read on demand rather than cached.  This runs only while a Script component
 * is selected AND its section is open, the files are a few KB, and a cache
 * would need invalidating on every external edit -- the editor already has a
 * file watcher for scripts, and a stale parameter list is exactly the kind of
 * wrong answer that reads as correct. */
static int read_script_declarations(const char *script_path,
                                    JceScriptParam *out, int max_out)
{
    if (!script_path || !script_path[0] || !out || max_out <= 0) return 0;

    char        abs[1024];
    const char *root = jce_assetdb_get_root();
    if (root && root[0])
        snprintf(abs, sizeof abs, "%s/%s", root, script_path);
    else
        snprintf(abs, sizeof abs, "%s", script_path);

    uint64_t sz  = 0;
    void    *buf = jce_fs_host_read_all(abs, &sz);
    if (!buf) return 0;
    /* A script far larger than any real one is still cheap to scan, but the
     * bound keeps a mis-pointed path (a model, a pak) from being walked. */
    if (sz > (4u << 20)) { jce_fs_buffer_free(buf); return 0; }

    const int n = jce_script_exports_scan((const char *)buf, (size_t)sz,
                                          out, max_out);
    jce_fs_buffer_free(buf);
    return n < 0 ? 0 : n;
}

/* Parameters -- Unity's [SerializeField], Godot's @export, UE's
 * UPROPERTY(EditAnywhere).
 *
 * THE COMMENT HERE USED TO SAY JCE "CANNOT" read the exposed set out of the
 * script's own declarations -- "seven backends, no uniform reflection, and a
 * Lua chunk has no declared fields at all".  That was the sentence that kept
 * both parity rows behind, because it reads as settled and so nobody looked
 * again.  The absence of uniform reflection is the ARGUMENT FOR a comment
 * marker rather than against declarations: `@export number speed = 5` inside
 * a line comment is identical in all seven languages precisely because none
 * of them can reflect, and one reader cannot drift between backends the way
 * seven parsers would.  See jce_script_exports.h.
 *
 * SO THERE ARE TWO SOURCES NOW and the table shows which is which: what the
 * SCRIPT declares, and what the AUTHOR added by hand.  Both remain editable
 * -- a declaration fixes a parameter's NAME and KIND, never its value, since
 * the whole point is that two turrets differing only in range share one
 * script.
 *
 * SYNC IS EXPLICIT, not automatic on draw.  The merge is non-destructive by
 * construction, so running it every frame would be safe for the data -- but
 * it would silently dirty the scene of anyone who merely SELECTED an entity,
 * and an undo stack that fills up with edits nobody made is worse than a
 * button.
 *
 * Shape follows the Animator parameter table (jce_panel_animator_sm.cpp): an
 * author-defined list of typed parameters is the same problem, and the two
 * should not read differently. */
static void draw_script_params(JceScriptComponent *scr)
{
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.script.params"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.script.paramsHint"));

    /* WHAT THE SCRIPT DECLARES, read before the table so every row below can
     * say whether the script still asks for it. */
    JceScriptParam decls[JCE_SCRIPT_PARAM_MAX];
    memset(decls, 0, sizeof decls);
    const int decl_n = read_script_declarations(scr->script_path, decls,
                                                JCE_SCRIPT_PARAM_MAX);

    /* How many declarations are not on the component yet -- the number that
     * tells an author whether pressing Sync will do anything. */
    int missing = 0;
    for (int i = 0; i < decl_n; ++i) {
        bool have = false;
        for (int j = 0; j < scr->param_count && j < JCE_SCRIPT_PARAM_MAX; ++j)
            if (strcmp(scr->params[j].name, decls[i].name) == 0) { have = true; break; }
        if (!have) ++missing;
    }

    if (decl_n > 0) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n_id("inspector.script.declaredBy",
                               "declared by the script:"));
        ImGui::SameLine();
        ImGui::Text("%d", decl_n);
        if (missing > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%d %s)", missing,
                jce_editor_i18n_id("inspector.script.notAdded", "not added"));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(missing == 0);
        if (ImGui::Button(jce_editor_i18n_id("inspector.script.sync",
                                             "Sync from script"))) {
            /* BATCH EDIT, not insp_track_edit().  A Button is DISCRETE:
             * its `if` body runs on neither the activation nor the
             * deactivation frame, so insp_track_edit() there records
             * nothing -- check_inspector_undo_scope named this exact line
             * and it was right.  The Add Parameter button below mutates the
             * same array the same way and already uses this idiom. */
            const int before = scr->param_count;
            jce_state_begin_batch_edit();
            const int after  = jce_script_exports_merge(
                decls, decl_n, scr->params, scr->param_count,
                JCE_SCRIPT_PARAM_MAX);
            if (after >= 0 && after != before)
                scr->param_count = after;
            jce_state_end_batch_edit();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n_id(
                "inspector.script.syncHint",
                "Adds every @export the script declares, with its default. "
                "Values you have already set are kept, and parameters the "
                "script no longer declares are left alone rather than "
                "deleted."));
    }

    /* CLAMPED BEFORE THE LOOP, and written back.  A hand-edited scene can
     * carry any int32 here; the parser clamps on load, but the component is
     * also reachable through the Automation API and through a script, so the
     * drawer does not get to assume the value it reads came from the parser. */
    int n = scr->param_count;
    if (n < 0) n = 0;
    if (n > JCE_SCRIPT_PARAM_MAX) n = JCE_SCRIPT_PARAM_MAX;
    if (n != scr->param_count)
        scr->param_count = n;

    if (n == 0)
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.script.paramsEmpty"));

    int remove_at = -1;
    if (n > 0 && ImGui::BeginTable("##script_params", 4,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(jce_editor_i18n("inspector.script.paramName"));
        ImGui::TableSetupColumn(jce_editor_i18n("inspector.script.paramKind"),
                                ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(jce_editor_i18n("inspector.script.paramValue"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; ++i) {
            JceScriptParam *pm = &scr->params[i];
            /* A row the script does not declare is either hand-added or left
             * over from a rename.  MARKED, never removed automatically: the
             * value cannot be recovered from the script file, so deleting it
             * would destroy work during a routine redraw.  Only meaningful
             * once the script declares something -- a script with no @export
             * at all must not paint every row as stale. */
            const bool undeclared =
                decl_n > 0 && pm->name[0] &&
                !jce_script_exports_declares(decls, decl_n, pm->name);
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            /* OUTSIDE the `if`, always: insp_track_edit() keys on
             * IsItemActivated / IsItemDeactivated and the body of the `if`
             * runs on the CHANGE frame, which is neither.  Same reasoning as
             * the combo below, opposite fix. */
            ImGui::InputText("##nm", pm->name, sizeof(pm->name));
            insp_track_edit();
            /* The script does not ask for this one.  Said here, next to
             * the name, because "which of these is left over from a
             * rename" is a question an author cannot answer any other
             * way once a script has changed under a scene. */
            if (undeclared) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", jce_editor_i18n_id(
                    "inspector.script.undeclared", "(not declared)"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", jce_editor_i18n_id(
                        "inspector.script.undeclaredHint",
                        "The script no longer declares an @export with "
                        "this name. The value is kept -- remove the row "
                        "yourself if it is no longer wanted."));
            }
            if (pm->name[0] == '\0') {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                                   jce_editor_i18n("inspector.script.paramUnnamed"));
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##kd", script_param_kind_label((int)pm->kind))) {
                for (int k = JCE_SCRIPT_PARAM_NUMBER; k <= JCE_SCRIPT_PARAM_ENTITY; ++k) {
                    const bool sel = ((int)pm->kind == k);
                    /* insp_undo_set, not insp_track_edit -- see the file
                     * comment and jce_panel_inspector_common.h:129.  It also
                     * leaves number/entity/text ALONE, which is deliberate and
                     * matches the serialiser: flipping the kind must not throw
                     * away what the author typed under the other one. */
                    if (ImGui::Selectable(script_param_kind_label(k), sel))
                        insp_undo_set(&pm->kind, k);
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            switch ((JceScriptParamKind)pm->kind) {
            case JCE_SCRIPT_PARAM_BOOL: {
                bool b = (pm->number != 0.0f);
                if (ImGui::Checkbox("##vl", &b))
                    insp_undo_set(&pm->number, b ? 1.0f : 0.0f);
                break;
            }
            case JCE_SCRIPT_PARAM_TEXT:
                ImGui::InputText("##vl", pm->text, sizeof(pm->text));
                insp_track_edit();
                break;
            case JCE_SCRIPT_PARAM_ENTITY: {
                int ent = (int)pm->entity;
                if (ImGui::DragInt("##vl", &ent, 1.0f, 0, 1 << 30))
                    pm->entity = (uint64_t)(ent < 0 ? 0 : ent);
                insp_track_edit();
                break;
            }
            case JCE_SCRIPT_PARAM_NUMBER:
            default:
                ImGui::DragFloat("##vl", &pm->number, 0.05f);
                insp_track_edit();
                break;
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("x"))
                remove_at = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", jce_editor_i18n("inspector.script.removeParam"));

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /* Deferred out of the loop: erasing the row being drawn invalidates the
     * ImGui IDs of every row after it in the same frame. */
    if (remove_at >= 0) {
        jce_state_begin_batch_edit();          /* snapshot is the pre-edit state */
        for (int i = remove_at; i + 1 < n; ++i)
            scr->params[i] = scr->params[i + 1];
        memset(&scr->params[n - 1], 0, sizeof(scr->params[n - 1]));
        scr->param_count = n - 1;
        jce_state_end_batch_edit();
        return;
    }

    if (n >= JCE_SCRIPT_PARAM_MAX) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.script.paramsFull"));
        return;
    }
    if (ImGui::Button(jce_editor_i18n("inspector.script.addParam"))) {
        jce_state_begin_batch_edit();
        memset(&scr->params[n], 0, sizeof(scr->params[n]));
        scr->params[n].kind = (uint32_t)JCE_SCRIPT_PARAM_NUMBER;
        scr->param_count = n + 1;
        jce_state_end_batch_edit();
    }
}

void draw_comp_script(JceScriptComponent *scr)
{
    if (!scr) return;

    /* No "unwired" badge: the runtime DOES consume scripts (jce_runtime drives
     * on_start/on_update/on_fixed_update/on_collision/on_trigger/on_message
     * through the VM registered for the script's language). */
    jce_draw_path_input_asset("##script_path", scr->script_path, 128, JCE_ASSET_KIND_SCRIPT);
    insp_track_edit();

    if (scr->script_path[0] == '\0')
        return;

    draw_script_language_note(scr);
    draw_script_params(scr);
}

void draw_comp_nav_agent(JceNavAgentComponent *na)
{
    if (!na) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.nav.enabled", "nav"), &na->enabled))
        insp_undo_bool(&na->enabled);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.radius", "nav"),         &na->radius,          0.05f, 0.0f, 100.0f,  "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.height", "nav"),         &na->height,          0.05f, 0.0f, 100.0f,  "%.2f"); insp_track_edit();
    insp_unwired_field_badge();   /* height */
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.maxSpeed", "nav"),       &na->max_speed,       0.05f, 0.0f, 1000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.maxAccel", "nav"),       &na->max_accel,       0.05f, 0.0f, 1000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.arriveRadius", "nav"),   &na->arrive_radius,   0.05f, 0.0f, 100.0f,  "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.waypointRadius", "nav"), &na->waypoint_radius, 0.05f, 0.0f, 100.0f,  "%.2f"); insp_track_edit();
    ImGui::Separator();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.nav.target", "nav"), na->target, 0.1f);
    insp_track_edit();
    int target_ent = (int)na->target_entity;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.nav.targetEntity", "nav"), &target_ent, 1.0f, 0, 1<<30)) {
        na->target_entity = (uint64_t)(target_ent < 0 ? 0 : target_ent);
    }
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.nav.autoRepath", "nav"), &na->auto_repath))
        insp_undo_bool(&na->auto_repath);
}

void draw_comp_sim_lod(JceSimLodComponent *sl)
{
    if (!sl) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.simlod.enabled", "simlod"),
                        &sl->enabled))
        insp_undo_bool(&sl->enabled);
    if (!sl->enabled) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.simlod.disabledNote"));
        return;
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.simlod.nearRadius", "simlod"),
                     &sl->near_radius, 0.5f, 0.0f, 100000.0f, "%.1f m");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.simlod.midRadius", "simlod"),
                     &sl->mid_radius, 0.5f, 0.0f, 100000.0f, "%.1f m");
    insp_track_edit();
    /* Keep mid >= near so the bands never invert. */
    if (sl->mid_radius < sl->near_radius) sl->mid_radius = sl->near_radius;
    ImGui::Separator();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.simlod.nearHz", "simlod"),
                     &sl->near_hz, 0.5f, 0.0f, 240.0f, "%.1f Hz");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.simlod.midHz", "simlod"),
                     &sl->mid_hz, 0.5f, 0.0f, 240.0f, "%.1f Hz");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.simlod.farHz", "simlod"),
                     &sl->far_hz, 0.25f, -1.0f, 240.0f, "%.2f Hz");
    insp_track_edit();
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.simlod.hzNote"));

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.simlod.gates"));
    uint32_t mask = sl->gate_mask ? sl->gate_mask : (uint32_t)JCE_SIMLOD_GATE_ALL;
    bool g_script = (mask & JCE_SIMLOD_GATE_SCRIPT) != 0;
    bool g_nav    = (mask & JCE_SIMLOD_GATE_NAV)    != 0;
    bool g_bt     = (mask & JCE_SIMLOD_GATE_BT)     != 0;
    bool changed = false;
    changed |= ImGui::Checkbox(jce_editor_i18n_id("inspector.simlod.gateScript", "simlod"), &g_script);
    changed |= ImGui::Checkbox(jce_editor_i18n_id("inspector.simlod.gateNav", "simlod"),    &g_nav);
    changed |= ImGui::Checkbox(jce_editor_i18n_id("inspector.simlod.gateBt", "simlod"),     &g_bt);
    if (changed) {
        mask = (g_script ? JCE_SIMLOD_GATE_SCRIPT : 0u)
             | (g_nav    ? JCE_SIMLOD_GATE_NAV    : 0u)
             | (g_bt     ? JCE_SIMLOD_GATE_BT     : 0u);
        sl->gate_mask = mask;
        insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.simlod.gateAnimFar", "simlod"),
                        &sl->gate_anim_far))
        insp_undo_bool(&sl->gate_anim_far);
}

void draw_comp_sequence_player(JceSequencePlayerComponent *sp)
{
    if (!sp) return;

    ImGui::TextUnformatted(jce_editor_i18n("inspector.seqplayer.path"));
    if (jce_draw_path_input_asset("##seqplayer_path", sp->seq_path,
                                  sizeof(sp->seq_path), JCE_ASSET_KIND_DATA))
        insp_track_edit();

    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.seqplayer.playOnAwake", "seqp"),
                        &sp->play_on_awake))
        insp_undo_bool(&sp->play_on_awake);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.seqplayer.overrideLoop", "seqp"),
                        &sp->override_loop))
        insp_undo_bool(&sp->override_loop);
    if (sp->override_loop) {
        ImGui::SameLine();
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.seqplayer.loopValue", "seqp"),
                            &sp->loop_override))
            insp_undo_bool(&sp->loop_override);
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.seqplayer.speed", "seqp"),
                     &sp->speed, 0.01f, 0.0f, 100.0f, "%.2f");
    insp_track_edit();

    /* Read-only bindings list (authored via the Sequencer panel's
     * "Sync to Player"). */
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.seqplayer.bindings"));
    int n = sp->binding_count;
    if (n < 0) n = 0;
    if (n > JCE_SEQ_PLAYER_MAX_BINDINGS) n = JCE_SEQ_PLAYER_MAX_BINDINGS;
    if (n == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.seqplayer.noBindings"));
    } else {
        JceScene *scene = jce_state_get_scene();
        for (int i = 0; i < n; ++i) {
            uint64_t id = sp->bindings[i];
            if (id == 0) {
                ImGui::TextDisabled("#%d: —", i);
            } else {
                const char *nm = scene
                    ? jce_scene_entity_name(scene, (JceEntity)id) : NULL;
                ImGui::TextDisabled("#%d: %s (%llu)", i,
                                    (nm && nm[0]) ? nm : "?",
                                    (unsigned long long)id);
            }
        }
    }

    if (ImGui::Button(jce_editor_i18n_id("inspector.seqplayer.openInSequencer", "seqp"))) {
        bool *ae_vis = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR);
        if (ae_vis) *ae_vis = true;
        jce_panel_animation_editor_request_tab(3);
        /* Bind the panel to THIS entity's sequence (select → operate): load its
         * .seq so its tracks/keys/bindings appear, instead of just opening the
         * panel on whatever was there before. */
        if (sp->seq_path[0])
            jce_panel_sequencer_load_path(sp->seq_path);
    }
}

/* Gameplay Ability System authoring card: an attribute table (name/base/min/max)
 * and an ability table (name/id/cost-attr/cost/cooldown).  This authors the
 * SETUP only; the live runtime system (active effects, cooldowns) is F12-only.
 * The runtime inits a JceGameplayAbilitySystem from these tables at Play and
 * scripts drive it via jce.gas_activate / jce.gas_get / jce.gas_apply. */
void draw_comp_gas(JceGameplayAbilitySystemComponent *gas)
{
    if (!gas) return;

    /* ── Attributes ── */
    ImGui::TextUnformatted(jce_editor_i18n("inspector.gas.attributes"));
    if (gas->attribute_count < 0) gas->attribute_count = 0;
    if (gas->attribute_count > JCE_GAS_AUTHOR_MAX_ATTRIBUTES)
        gas->attribute_count = JCE_GAS_AUTHOR_MAX_ATTRIBUTES;

    int remove_attr = -1;
    for (int i = 0; i < gas->attribute_count; i++) {
        JceGasAttributeAuthor *a = &gas->attributes[i];
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputText(jce_editor_i18n_id("inspector.gas.attrName", "gas"),
                         a->name, sizeof a->name);
        insp_track_edit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat(jce_editor_i18n_id("inspector.gas.attrBase", "gas"),
                         &a->base, 0.5f); insp_track_edit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat(jce_editor_i18n_id("inspector.gas.attrMin", "gas"),
                         &a->min, 0.5f); insp_track_edit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat(jce_editor_i18n_id("inspector.gas.attrMax", "gas"),
                         &a->max, 0.5f); insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.gas.remove", "gas")))
            remove_attr = i;
        ImGui::PopID();
    }
    if (remove_attr >= 0) {
        jce_state_begin_batch_edit();
        for (int j = remove_attr; j + 1 < gas->attribute_count; j++)
            gas->attributes[j] = gas->attributes[j + 1];
        gas->attribute_count--;
        memset(&gas->attributes[gas->attribute_count], 0,
               sizeof gas->attributes[gas->attribute_count]);
        jce_state_end_batch_edit();
    }
    if (gas->attribute_count < JCE_GAS_AUTHOR_MAX_ATTRIBUTES &&
        ImGui::Button(jce_editor_i18n_id("inspector.gas.addAttr", "gas"))) {
        jce_state_begin_batch_edit();
        JceGasAttributeAuthor *a = &gas->attributes[gas->attribute_count];
        memset(a, 0, sizeof *a);
        snprintf(a->name, sizeof a->name, "%s", "Attribute");
        a->base = 0.0f; a->min = 0.0f; a->max = 100.0f;
        gas->attribute_count++;
        jce_state_end_batch_edit();
    }

    ImGui::Separator();

    /* ── Abilities ── */
    ImGui::TextUnformatted(jce_editor_i18n("inspector.gas.abilities"));
    if (gas->ability_count < 0) gas->ability_count = 0;
    if (gas->ability_count > JCE_GAS_AUTHOR_MAX_ABILITIES)
        gas->ability_count = JCE_GAS_AUTHOR_MAX_ABILITIES;

    int remove_abil = -1;
    for (int i = 0; i < gas->ability_count; i++) {
        JceGasAbilityAuthor *b = &gas->abilities[i];
        ImGui::PushID(1000 + i);
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputText(jce_editor_i18n_id("inspector.gas.abilName", "gas"),
                         b->name, sizeof b->name);
        insp_track_edit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        int id = (int)b->id;
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.gas.abilId", "gas"),
                           &id, 1.0f, 0, 1000000)) {
            jce_state_begin_batch_edit();
            b->id = (uint32_t)(id < 0 ? 0 : id);
            jce_state_end_batch_edit();
        }
        insp_track_edit();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragInt(jce_editor_i18n_id("inspector.gas.abilCostAttr", "gas"),
                       &b->cost_attr_idx, 1.0f, -1,
                       JCE_GAS_AUTHOR_MAX_ATTRIBUTES - 1);
        insp_track_edit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat(jce_editor_i18n_id("inspector.gas.abilCost", "gas"),
                         &b->cost_magnitude, 0.5f); insp_track_edit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat(jce_editor_i18n_id("inspector.gas.abilCooldown", "gas"),
                         &b->cooldown_seconds, 0.05f, 0.0f, 3600.0f, "%.2f");
        insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.gas.remove", "gas")))
            remove_abil = i;
        ImGui::PopID();
    }
    if (remove_abil >= 0) {
        jce_state_begin_batch_edit();
        for (int j = remove_abil; j + 1 < gas->ability_count; j++)
            gas->abilities[j] = gas->abilities[j + 1];
        gas->ability_count--;
        memset(&gas->abilities[gas->ability_count], 0,
               sizeof gas->abilities[gas->ability_count]);
        jce_state_end_batch_edit();
    }
    if (gas->ability_count < JCE_GAS_AUTHOR_MAX_ABILITIES &&
        ImGui::Button(jce_editor_i18n_id("inspector.gas.addAbil", "gas"))) {
        jce_state_begin_batch_edit();
        JceGasAbilityAuthor *b = &gas->abilities[gas->ability_count];
        memset(b, 0, sizeof *b);
        snprintf(b->name, sizeof b->name, "%s", "Ability");
        b->id = (uint32_t)(gas->ability_count + 1);
        b->cost_attr_idx = -1;
        gas->ability_count++;
        jce_state_end_batch_edit();
    }

    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.gas.note"));
}
