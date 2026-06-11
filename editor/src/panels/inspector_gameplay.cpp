/*
 * inspector_gameplay.cpp
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
    insp_unwired_badge();
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
    }
}
