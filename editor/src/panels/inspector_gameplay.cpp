/*
 * inspector_gameplay.cpp
 * Gameplay component inspector drawers: behavior tree, spawn manager, weapon,
 * save point, trigger volume, terrain, particle emitter, script.
 */

#include "jce_panel_inspector_common.h"

void draw_comp_behavior_tree(JceBehaviorTree *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.bt.active", "bt"), &bt->active))
        insp_undo_bool(&bt->active);
    ImGui::Text("%s: %u", jce_editor_i18n("inspector.bt.treeHandle"), (unsigned)bt->tree_handle_idx);
    ImGui::Text("%s: %u", jce_editor_i18n("inspector.bt.contextHandle"), (unsigned)bt->context_handle_idx);
    ImGui::TextDisabled(jce_editor_i18n("inspector.bt.editInBtEditor"));
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
    static const char *kinds[] = { "Hitscan", "Projectile" };
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
    static const char *kinds[] = { "Manual", "Auto", "Checkpoint" };
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
    static const char *shapes[] = { "AABB", "Sphere", "OBB" };
    int sh = tv->shape; if (sh < 0 || sh > 2) sh = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.trig.shape", "trig"), &sh, shapes, 3)) {
        tv->shape = sh; insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.enabled", "trig"), &tv->enabled))
        insp_undo_bool(&tv->enabled);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.fireStayEvents", "trig"), &tv->fire_stay))
        insp_undo_bool(&tv->fire_stay);
    ImGui::InputText(jce_editor_i18n_id("inspector.trig.tag", "trig"), tv->tag, sizeof tv->tag);
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

void draw_comp_particle_emitter(JceParticleEmitterComponent *pe)
{
    if (!pe) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.emitRate", "pe"), &pe->emit_rate, 0.5f, 0.0f, 10000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.lifetimeMin", "pe"), &pe->lifetime_min, 0.05f, 0.0f, 1000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.lifetimeMax", "pe"), &pe->lifetime_max, 0.05f, 0.0f, 1000.0f);
    insp_track_edit();
    if (pe->lifetime_max < pe->lifetime_min) pe->lifetime_max = pe->lifetime_min;
    ImGui::TextDisabled(jce_editor_i18n("inspector.pe.useParticleSystemPanel"));
}

void draw_comp_script(JceScriptComponent *scr)
{
    jce_draw_path_input_asset("##script_path", scr->script_path, 128, JCE_ASSET_KIND_SCRIPT);
    insp_track_edit();
    accept_asset_drop(scr->script_path, 128);
}
