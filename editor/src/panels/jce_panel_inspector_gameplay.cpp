/*
 * jce_panel_inspector_gameplay.cpp
 * Gameplay component inspector drawers: behavior tree, spawn manager, weapon,
 * save point, trigger volume, terrain, particle emitter, script.
 */

#include "jce_panel_inspector_common.h"

extern "C" {
#include <jce/renderer/jce_renderer_caps.h>    /* compute caps gate (GPU particles) */
#include <jce/renderer/jce_render_pipeline.h>  /* gpu_particles feature flag hint   */
}

void draw_comp_behavior_tree(JceBehaviorTree *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.bt.active", "bt"), &bt->active))
        insp_undo_bool(&bt->active);

    /* Behavior-tree asset (BehaviorTree.CPP XML).  The runtime loads this
     * file into its JceBtContext at Play and ticks it each gameplay frame. */
    ImGui::TextUnformatted(jce_editor_i18n("inspector.bt.treePath"));
    if (jce_draw_path_input_asset("##bt_tree", bt->tree_path,
                                  sizeof(bt->tree_path), JCE_ASSET_KIND_DATA))
        insp_track_edit();
    accept_asset_drop(bt->tree_path, sizeof(bt->tree_path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("inspector.bt.clearTree", "bt")))
    { bt->tree_path[0] = '\0'; insp_track_edit(); }

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
            insp_track_edit();
        }
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
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sm.enabled", "sm"), &en)) { m->enabled = en ? 1 : 0; insp_track_edit(); }
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sm.maxPeds", "sm"),          &m->max_peds,         1.0f, 0, 1024); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sm.maxVehicles", "sm"),      &m->max_vehicles,     1.0f, 0, 1024); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.minSpawnRadius", "sm"),  &m->min_spawn_radius, 0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.maxSpawnRadius", "sm"),  &m->max_spawn_radius, 0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.despawnPad", "sm"),       &m->despawn_pad,      0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.spawnInterval", "sm"),&m->spawn_interval,   0.05f, 0.0f, 60.0f,    "%.2f"); insp_track_edit();
    if (m->max_spawn_radius < m->min_spawn_radius) m->max_spawn_radius = m->min_spawn_radius;

    int pn = m->ped_archetype_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.pedArchetypes", "sm"), &pn, 1.0f, 0, 8)) {
        if (pn < 0) pn = 0; if (pn > 8) pn = 8;
        m->ped_archetype_count = pn; insp_track_edit();
    }
    char lbl[32];
    for (int i = 0; i < m->ped_archetype_count; ++i) {
        snprintf(lbl, sizeof lbl, "Ped[%d] id##sm%d", i, i);
        int v = (int)m->ped_archetypes[i];
        if (ImGui::DragInt(lbl, &v, 1.0f, 0, INT_MAX)) {
            m->ped_archetypes[i] = (uint32_t)(v < 0 ? 0 : v);
            insp_track_edit();
        }
    }

    int vn = m->vehicle_archetype_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.vehicleArchetypes", "sm"), &vn, 1.0f, 0, 8)) {
        if (vn < 0) vn = 0; if (vn > 8) vn = 8;
        m->vehicle_archetype_count = vn; insp_track_edit();
    }
    for (int i = 0; i < m->vehicle_archetype_count; ++i) {
        snprintf(lbl, sizeof lbl, "Vehicle[%d] id##smv%d", i, i);
        int v = (int)m->vehicle_archetypes[i];
        if (ImGui::DragInt(lbl, &v, 1.0f, 0, INT_MAX)) {
            m->vehicle_archetypes[i] = (uint32_t)(v < 0 ? 0 : v);
            insp_track_edit();
        }
    }

    /* RNG seed (display as two 32-bit halves to avoid ImGui int64 absence). */
    uint32_t lo = (uint32_t)(m->rng_seed & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(m->rng_seed >> 32);
    int lo_i = (int)lo, hi_i = (int)hi;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.rngSeedLo", "sm"), &lo_i, 1.0f)) {
        m->rng_seed = ((uint64_t)(uint32_t)hi_i << 32) | (uint32_t)lo_i;
        insp_track_edit();
    }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.rngSeedHi", "sm"), &hi_i, 1.0f)) {
        m->rng_seed = ((uint64_t)(uint32_t)hi_i << 32) | (uint32_t)lo_i;
        insp_track_edit();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.sm.roadNetworkNote"));
}

void draw_comp_weapon(JceWeaponComponent *w)
{
    if (!w) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.wp.name", "wp"), w->name, sizeof w->name); insp_track_edit();
    const char *kinds[] = { jce_editor_i18n("inspector.wp.kind.hitscan"), jce_editor_i18n("inspector.wp.kind.projectile") };
    int k = w->kind; if (k < 0 || k > 1) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.wp.kind", "wp"), &k, kinds, 2)) { w->kind = k; insp_track_edit(); }
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
    if (ImGui::Combo(jce_editor_i18n_id("inspector.sv.kind", "sv"), &k, kinds, 3)) { sp->kind = k; insp_track_edit(); }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sv.radius", "sv"), &sp->radius, 0.05f, 0.0f, 1000.0f, "%.2f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sv.slot", "sv"),   &sp->slot,   1.0f, -1, 256);              insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sv.oneShot", "sv"),          &sp->one_shot))         insp_undo_bool(&sp->one_shot);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sv.requireInteract", "sv"),  &sp->require_interact)) insp_undo_bool(&sp->require_interact);
}

void draw_comp_trigger_volume(JceTriggerVolumeComponent *tv)
{
    if (!tv) return;
    const char *shapes[] = { jce_editor_i18n("inspector.trig.shape.aabb"), jce_editor_i18n("inspector.trig.shape.sphere"), jce_editor_i18n("inspector.trig.shape.obb") };
    int sh = tv->shape; if (sh < 0 || sh > 2) sh = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.trig.shape", "trig"), &sh, shapes, 3)) {
        tv->shape = sh; insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.enabled", "trig"), &tv->enabled))
        insp_undo_bool(&tv->enabled);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.fireStayEvents", "trig"), &tv->fire_stay))
        insp_undo_bool(&tv->fire_stay);
    ImGui::InputText(jce_editor_i18n_id("inspector.trig.tag", "trig"), tv->tag, sizeof tv->tag);
    /* Commit the tag when the field loses focus (click away / select another
     * object) or on Enter — not every frame — so switching objects mid-edit
     * still records + saves the change. */
    if (ImGui::IsItemDeactivatedAfterEdit())
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
    accept_asset_drop(tc->terrain_path, sizeof tc->terrain_path);
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.dropHint"));

    if (ImGui::Checkbox(jce_editor_i18n("inspector.terrain.visible"), &tc->visible))
        insp_undo_bool(&tc->visible);

    ImGui::ColorEdit3(jce_editor_i18n("inspector.terrain.tint"), tc->tint);
    insp_track_edit();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.terrain.splatHeader"));
    if (ImGui::Checkbox(jce_editor_i18n("inspector.terrain.splatEnable"), &tc->splat_enabled))
        insp_undo_bool(&tc->splat_enabled);
    if (tc->tile_scale <= 0.0f) tc->tile_scale = 10.0f;
    ImGui::DragFloat(jce_editor_i18n("inspector.terrain.layerTile"), &tc->tile_scale, 0.1f, 0.1f, 256.0f, "%.2f");
    insp_track_edit();

    for (int i = 0; i < 4; i++) {
        char label[32];
        snprintf(label, sizeof label, jce_editor_i18n("inspector.terrain.layerFmt"), i);
        ImGui::PushID(i);
        jce_draw_path_input_asset(label, tc->layer_albedo_path[i],
                         sizeof tc->layer_albedo_path[i], JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(tc->layer_albedo_path[i],
                          sizeof tc->layer_albedo_path[i]);
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.splatChannels"));

    /* Show summary if a terrain file is bound. Using load_file is heavy;
     * for the inspector we just print the path and let the Terrain panel
     * handle authoring. */
    if (tc->terrain_path[0]) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", jce_editor_i18n("inspector.terrain.useTerrainPanel"));
    } else {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("inspector.terrain.notBound"));
    }
}

void draw_comp_vegetation_scatter(JceVegetationScatterComponent *vs)
{
    if (!vs) return;
    bool ch = false;

    ImGui::TextUnformatted(jce_editor_i18n("inspector.vegetationScatter.mesh"));
    if (jce_draw_path_input_asset("##veg_mesh", vs->mesh_path,
                                  sizeof vs->mesh_path, JCE_ASSET_KIND_MODEL))
        ch = true;
    accept_asset_drop(vs->mesh_path, sizeof vs->mesh_path);

    ImGui::TextUnformatted(jce_editor_i18n("inspector.vegetationScatter.albedo"));
    if (jce_draw_path_input_asset("##veg_albedo", vs->albedo_path,
                                  sizeof vs->albedo_path, JCE_ASSET_KIND_TEXTURE))
        ch = true;
    accept_asset_drop(vs->albedo_path, sizeof vs->albedo_path);

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
    };
    if (ImGui::Combo(jce_editor_i18n("inspector.water.mode"),
                     &w->water_mode, mode_names, 2)) {
        if (w->water_mode < JCE_WATER_MODE_GERSTNER) w->water_mode = JCE_WATER_MODE_GERSTNER;
        if (w->water_mode > JCE_WATER_MODE_FFT)      w->water_mode = JCE_WATER_MODE_FFT;
        ch = true;
    }

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
    } else { /* JCE_WATER_MODE_FFT */
        ImGui::SeparatorText(jce_editor_i18n("inspector.water.fft.header"));
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
    ch |= ImGui::DragFloat(jce_editor_i18n("inspector.water.sunSpecular"),
                           &w->sun_specular, 0.02f, 0.0f, 100.0f, "%.2f");
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
    accept_asset_drop(pe->asset_path, sizeof(pe->asset_path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("inspector.pe.clearAsset", "pe")))
    { pe->asset_path[0] = '\0'; insp_track_edit(); }

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

void draw_comp_script(JceScriptComponent *scr)
{
    /* No "unwired" badge: the runtime DOES consume scripts (jce_runtime drives
     * on_start/on_update/on_collision/on_trigger/on_message via the Lua VM). */
    jce_draw_path_input_asset("##script_path", scr->script_path, 128, JCE_ASSET_KIND_SCRIPT);
    insp_track_edit();
    accept_asset_drop(scr->script_path, 128);
}

void draw_comp_nav_agent(JceNavAgentComponent *na)
{
    if (!na) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.nav.enabled", "nav"), &na->enabled))
        insp_undo_bool(&na->enabled);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.radius", "nav"),         &na->radius,          0.05f, 0.0f, 100.0f,  "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.nav.height", "nav"),         &na->height,          0.05f, 0.0f, 100.0f,  "%.2f"); insp_track_edit();
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
        insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.nav.autoRepath", "nav"), &na->auto_repath))
        insp_undo_bool(&na->auto_repath);
}

void draw_comp_sequence_player(JceSequencePlayerComponent *sp)
{
    if (!sp) return;

    ImGui::TextUnformatted(jce_editor_i18n("inspector.seqplayer.path"));
    if (jce_draw_path_input_asset("##seqplayer_path", sp->seq_path,
                                  sizeof(sp->seq_path), JCE_ASSET_KIND_DATA))
        insp_track_edit();
    accept_asset_drop(sp->seq_path, sizeof(sp->seq_path));

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
