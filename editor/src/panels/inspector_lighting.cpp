/*
 * inspector_lighting.cpp
 * Light, camera, virtual camera, reflection probe, and light probe group
 * component inspector drawers.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_tip.h"

#include <unordered_map>

extern "C" {
#include <jce/renderer/jce_reflection_probe_bake.h>
#include <jce/os/core/jce_math.h>
}

void draw_comp_light(JceScene *scene, JceEntity e, uint64_t flags)
{
    char lbl[256];

    int light_type = -1;
    if (flags & JCE_COMP_FLAG_DIR_LIGHT)        light_type = 0;
    else if (flags & JCE_COMP_FLAG_POINT_LIGHT) light_type = 1;
    else if (flags & JCE_COMP_FLAG_SPOT_LIGHT)  light_type = 2;
    if (light_type < 0) return;

    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    if (light_type == 0) {
        JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    } else if (light_type == 1) {
        JcePointLight *l = jce_scene_get_point_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    } else {
        JceSpotLight *l = jce_scene_get_spot_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    }

    snprintf(lbl, sizeof(lbl), "%s###Color", jce_editor_i18n("light.color"));
    if (ImGui::ColorEdit3(lbl, color)) {
        if (light_type == 0)      { JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);   l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else if (light_type == 1) { JcePointLight       *l = jce_scene_get_point_light(scene, e); l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else                      { JceSpotLight        *l = jce_scene_get_spot_light(scene, e);  l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
    }
    INSP_RESET_CTX("##rst_lightColor",
        if (light_type == 0)      { JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);   l->color = { 1.0f, 1.0f, 1.0f }; }
        else if (light_type == 1) { JcePointLight       *l = jce_scene_get_point_light(scene, e); l->color = { 1.0f, 1.0f, 1.0f }; }
        else                      { JceSpotLight        *l = jce_scene_get_spot_light(scene, e);  l->color = { 1.0f, 1.0f, 1.0f }; });
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
    if (ImGui::DragFloat(lbl, &intensity, 0.1f, 0.0f, 100.0f)) {
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = intensity; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = intensity; }
        else                      { jce_scene_get_spot_light(scene, e)->intensity  = intensity; }
    }
    INSP_RESET_CTX("##rst_lightIntensity",
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = 1.0f; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = 1.0f; }
        else                      { jce_scene_get_spot_light(scene, e)->intensity  = 1.0f; });
    insp_track_edit();

    const char *light_types[] = {
        jce_editor_i18n("light.directional"),
        jce_editor_i18n("light.point"),
        jce_editor_i18n("light.spot")
    };
    int new_type = light_type;
    snprintf(lbl, sizeof(lbl), "%s###Type", jce_editor_i18n("light.type"));
    if (ImGui::Combo(lbl, &new_type, light_types, 3) && new_type != light_type) {
        jce_state_begin_batch_edit();
        if (light_type == 0) jce_scene_remove_dir_light(scene, e);
        else if (light_type == 1) jce_scene_remove_point_light(scene, e);
        else jce_scene_remove_spot_light(scene, e);

        if (new_type == 0) {
            JceDirectionalLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.casts_shadow = false;
            jce_scene_set_dir_light(scene, e, &l);
        } else if (new_type == 1) {
            JcePointLight l = {};
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            jce_scene_set_point_light(scene, e, &l);
        } else {
            JceSpotLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            l.inner_cone_cos = cosf(25.0f * JCE_DEG2RAD);
            l.outer_cone_cos = cosf(35.0f * JCE_DEG2RAD);
            jce_scene_set_spot_light(scene, e, &l);
        }
        jce_state_end_batch_edit();
        light_type = new_type;
    }

    if (light_type == 1 || light_type == 2) {
        float radius = (light_type == 1)
            ? jce_scene_get_point_light(scene, e)->radius
            : jce_scene_get_spot_light(scene, e)->radius;
        snprintf(lbl, sizeof(lbl), "%s###Radius", jce_editor_i18n("collider.radius"));
        if (ImGui::DragFloat(lbl, &radius, 0.1f, 0.01f, 1000.0f)) {
            if (light_type == 1) jce_scene_get_point_light(scene, e)->radius = radius;
            else                 jce_scene_get_spot_light(scene, e)->radius  = radius;
        }
        insp_track_edit();
    }

    if (light_type == 2) {
        JceSpotLight *l = jce_scene_get_spot_light(scene, e);
        float inner_deg = acosf(l->inner_cone_cos) * JCE_RAD2DEG;
        float outer_deg = acosf(l->outer_cone_cos) * JCE_RAD2DEG;
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.innerCone", "InnerCone"), &inner_deg, 0.5f, 0.0f, 89.0f))
            l->inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
        insp_track_edit();
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.outerCone", "OuterCone"), &outer_deg, 0.5f, 0.0f, 90.0f))
            l->outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
        insp_track_edit();
        if (l->outer_cone_cos > l->inner_cone_cos)
            l->outer_cone_cos = l->inner_cone_cos;

        /* P3-E.5 — Light cookie + IES profile (spot lights). */
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("inspector.light.cookie"));
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.light.cookie.path", "CookiePath"),
                                  l->cookie_path, sizeof l->cookie_path,
                                  JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.light.cookie.clear", "CookieClear"))) {
            l->cookie_path[0] = '\0';
            l->cookie_texture.idx = UINT16_MAX;
            insp_track_edit();
        }
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.cookieStrength", "CookieStrength"),
                             &l->cookie_strength, 0.01f, 0.0f, 1.0f)) {
            insp_track_edit();
        }

        ImGui::TextUnformatted(jce_editor_i18n("inspector.light.iesProfile"));
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.light.ies.path", "IesPath"),
                                  l->ies_path, sizeof l->ies_path,
                                  JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.light.ies.clear", "IesClear"))) {
            l->ies_path[0] = '\0';
            l->ies_lut_texture.idx = UINT16_MAX;
            insp_track_edit();
        }
    }

    if (light_type == 0) {
        JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.light.castsShadow", "CastsShadow"), &l->casts_shadow))
            insp_undo_bool(&l->casts_shadow);

        /* P3-E.5b — Directional cookie (world-aligned ortho projection
         * centred on the camera; shares sampler 13 with spot cookies
         * under the v1 single-bind constraint — first eligible spot
         * cookie still wins.  See engine/src/renderer/AGENTS.md.). */
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("inspector.light.dirCookie"));
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.light.dirCookie.path", "DirCookiePath"),
                                  l->cookie_path, sizeof l->cookie_path,
                                  JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.light.dirCookie.clear", "DirCookieClear"))) {
            l->cookie_path[0] = '\0';
            l->cookie_texture.idx = UINT16_MAX;
            insp_track_edit();
        }
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.dirCookieStrength", "DirCookieStrength"),
                             &l->cookie_strength, 0.01f, 0.0f, 1.0f)) {
            insp_track_edit();
        }
    }
}

void draw_comp_camera(JceCameraComponent *cam)
{
    static const JceReflectType *t = jce_reflect_find("Camera");
    if (t) {
        jce_reflect_draw(t, cam);
        return;
    }
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s###FOV", jce_editor_i18n("camera.fov"));
    ImGui::DragFloat(lbl, &cam->fov_deg, 1.0f, 1.0f, 179.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Near", jce_editor_i18n("camera.nearClip"));
    ImGui::DragFloat(lbl, &cam->near_plane, 0.01f, 0.001f, 100.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Far", jce_editor_i18n("camera.farClip"));
    ImGui::DragFloat(lbl, &cam->far_plane, 1.0f, 1.0f, 100000.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Orthographic", jce_editor_i18n("camera.orthographic"));
    if (ImGui::Checkbox(lbl, &cam->ortho))
        insp_undo_bool(&cam->ortho);

    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.camera.primary", "CamPrimary"), &cam->is_primary))
        insp_undo_bool(&cam->is_primary);

    int stack_idx = (int)cam->stack_index;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.camera.stackIndex", "CamStack"), &stack_idx, 1.0f, 0, 3)) {
        cam->stack_index = (uint8_t)stack_idx; insp_track_edit();
    }

    static const char *clear_modes[] = {
        "inspector.camera.clearMode.skybox",
        "inspector.camera.clearMode.color",
        "inspector.camera.clearMode.depthOnly",
        "inspector.camera.clearMode.nothing",
    };
    int cm = (int)cam->clear_mode;
    if (cm < 0 || cm > 3) cm = 0;
    const char *cm_preview = jce_editor_i18n(clear_modes[cm]);
    if (ImGui::BeginCombo(jce_editor_i18n_id("inspector.camera.clearMode", "CamClear"), cm_preview)) {
        for (int i = 0; i < 4; ++i) {
            bool sel = (cm == i);
            if (ImGui::Selectable(jce_editor_i18n(clear_modes[i]), sel)) {
                cam->clear_mode = (uint8_t)i; insp_track_edit();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void draw_comp_virtual_camera(JceVirtualCameraComponent *vc)
{
    if (!vc) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.vcam.name", "vcam"), vc->vcam_name, sizeof vc->vcam_name);
    insp_track_edit();
    int prio = vc->priority;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.vcam.priority", "vcam"), &prio, 1, -1000, 1000)) {
        vc->priority = prio; insp_track_edit();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("inspector.vcam.solo", "vcam"))) {
        vc->priority = 9999;
        vc->active   = true;
        insp_track_edit();
    }
    jce_editor::help_tip(jce_editor_i18n("inspector.vcam.soloTooltip"));
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vcam.active", "vcam"), &vc->active))
        insp_undo_bool(&vc->active);

    static const char *track_modes[] = {
        "None", "Follow", "Look At", "Follow + Look At"
    };
    int tm = vc->track_mode; if (tm < 0 || tm > 3) tm = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.vcam.trackMode", "vcam"), &tm, track_modes, 4)) {
        vc->track_mode = tm; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vcam.fov", "vcam"), &vc->fov_deg, 0.5f, 1.0f, 179.0f, "%.1f");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vcam.damping", "vcam"), &vc->damping, 0.01f, 0.0f, 1.0f, "%.2f");
    insp_track_edit();

    ImGui::Separator();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.vcam.position", "vcam"), vc->position, 0.1f);
    insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.vcam.lookAt", "vcam"), vc->look_at, 0.1f);
    insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.vcam.followOffset", "vcam"), vc->follow_offset, 0.1f);
    insp_track_edit();

    ImGui::Separator();
    int ft = (int)vc->follow_target;
    if (ImGui::InputInt(jce_editor_i18n_id("inspector.vcam.followTargetEntity", "vcam"), &ft)) {
        vc->follow_target = (uint64_t)(ft < 0 ? 0 : ft);
        insp_track_edit();
    }
    int lt = (int)vc->look_at_target;
    if (ImGui::InputInt(jce_editor_i18n_id("inspector.vcam.lookAtTargetEntity", "vcam"), &lt)) {
        vc->look_at_target = (uint64_t)(lt < 0 ? 0 : lt);
        insp_track_edit();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.vcam.targetNote"));
    ImGui::Separator();
    ImGui::TextDisabled(jce_editor_i18n("inspector.vcam.playModeNote"));
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.vcam.priorityNote"));
}

void draw_comp_reflection_probe(JceReflectionProbeComponent *r)
{
    if (!r) return;
    static const char *modes[] = { "Baked", "Realtime", "Custom" };
    int m = r->mode; if (m < 0 || m > 2) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.refl.mode", "refl"), &m, modes, 3)) { r->mode = m; insp_track_edit(); }
    static const int res_choices[] = { 16, 32, 64, 128, 256, 512, 1024 };
    int ri = 3;
    for (int i = 0; i < 7; ++i) if (res_choices[i] == r->resolution) { ri = i; break; }
    if (ImGui::Combo(jce_editor_i18n_id("inspector.refl.resolution", "refl"), &ri, "16\0" "32\0" "64\0" "128\0" "256\0" "512\0" "1024\0\0")) {
        r->resolution = res_choices[ri]; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.intensity", "refl"),      &r->intensity,      0.05f, 0.0f, 100.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.blendDistance", "refl"), &r->blend_distance, 0.05f, 0.0f, 100.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.boxSize", "refl"),   r->box_size,   0.1f); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.boxOffset", "refl"), r->box_offset, 0.05f); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.nearClip", "refl"), &r->near_clip, 0.01f, 0.001f, 10.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.farClip", "refl"),  &r->far_clip,  1.0f, 0.1f, 100000.0f, "%.1f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.refl.boxProjection", "refl"), &r->box_projection)) insp_undo_bool(&r->box_projection);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.refl.hdr", "refl"), &r->hdr)) insp_undo_bool(&r->hdr);
    if (r->mode == JCE_REFLECTION_PROBE_CUSTOM) {
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.refl.customHdr", "refl"), r->custom_hdr_path, sizeof r->custom_hdr_path, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(r->custom_hdr_path, sizeof r->custom_hdr_path);
    }

    /* ── Bake (P3-E.3) ─────────────────────────────────────────────── */
    ImGui::Separator();

    /* Per-component bake size / dynamic-objects toggle live in editor
     * statics keyed by the component pointer so they survive frame-to-
     * frame without polluting the serialised struct. */
    static std::unordered_map<JceReflectionProbeComponent *, int>  s_bake_size;
    static std::unordered_map<JceReflectionProbeComponent *, bool> s_bake_dynamic;
    static JceReflectionProbeBakeHandle s_active_handle = 0;
    static JceReflectionProbeComponent *s_active_target = nullptr;

    int  &size_idx     = s_bake_size[r];
    bool &include_dyn  = s_bake_dynamic[r];
    if (size_idx == 0 && r->resolution >= 128) {
        /* default to component resolution, clamped to the bake set. */
        size_idx = (r->resolution >= 512) ? 2 : (r->resolution >= 256) ? 1 : 0;
    }
    if (!include_dyn) include_dyn = true;

    const char *kSizeItems = "128\0" "256\0" "512\0\0";
    const int   kSizeValues[3] = { 128, 256, 512 };
    if (ImGui::Combo(jce_editor_i18n("inspector.reflection_probe.cubemap_size"), &size_idx, kSizeItems)) {}
    if (size_idx < 0) size_idx = 0;
    if (size_idx > 2) size_idx = 2;
    ImGui::Checkbox(jce_editor_i18n("inspector.reflection_probe.include_dynamic"), &include_dyn);

    /* Read-only path of last bake (component-owned). */
    if (r->baked_cubemap_path[0]) {
        ImGui::TextDisabled("%s", r->baked_cubemap_path);
    }

    JceReflectionProbeBakeProgress prog{};
    bool active = false;
    if (s_active_target == r && s_active_handle != 0) {
        active = jce_reflection_probe_bake_poll(s_active_handle, &prog);
        if (active && prog.status != JCE_BAKE_STATUS_RENDERING_FACES &&
                      prog.status != JCE_BAKE_STATUS_CONVOLVING_IRRADIANCE &&
                      prog.status != JCE_BAKE_STATUS_CONVOLVING_SPECULAR &&
                      prog.status != JCE_BAKE_STATUS_ENCODING_KTX2) {
            /* Terminal — finalize. */
            if (prog.status == JCE_BAKE_STATUS_DONE) {
                /* Re-derive the path the engine wrote so we record the
                 * exact same string the bake submitted. */
                char path[256];
                snprintf(path, sizeof path,
                         "ReflectionProbes/probe_%p.ktx",
                         (void *)r);
                snprintf(r->baked_cubemap_path,
                         sizeof r->baked_cubemap_path, "%s", path);
                insp_track_edit();
            }
            s_active_handle = 0;
            s_active_target = nullptr;
            active          = false;
        }
    }

    if (active) {
        const char *status_key = "inspector.reflection_probe.status.rendering_faces";
        switch (prog.status) {
        case JCE_BAKE_STATUS_CONVOLVING_IRRADIANCE:
            status_key = "inspector.reflection_probe.status.convolving_irradiance"; break;
        case JCE_BAKE_STATUS_CONVOLVING_SPECULAR:
            status_key = "inspector.reflection_probe.status.convolving_specular"; break;
        case JCE_BAKE_STATUS_ENCODING_KTX2:
            status_key = "inspector.reflection_probe.status.encoding"; break;
        default: break;
        }
        ImGui::ProgressBar(prog.overall, ImVec2(-1, 0));
        ImGui::TextUnformatted(jce_editor_i18n(status_key));
        if (ImGui::Button(jce_editor_i18n("inspector.reflection_probe.cancel"))) {
            jce_reflection_probe_bake_cancel(s_active_handle);
        }
    } else {
        if (ImGui::Button(jce_editor_i18n("inspector.reflection_probe.bake"))) {
            char out[256];
            snprintf(out, sizeof out,
                     "ReflectionProbes/probe_%p.ktx", (void *)r);
            JceReflectionProbeBakeDesc desc{};
            desc.position = jce_v3(r->box_offset[0],
                                    r->box_offset[1],
                                    r->box_offset[2]);
            desc.cubemap_size            = (uint32_t)kSizeValues[size_idx];
            desc.specular_mip_count      = 5;
            desc.output_path_ktx2        = out;
            desc.include_skybox          = true;
            desc.include_dynamic_objects = include_dyn;
            JceReflectionProbeBakeHandle h =
                jce_reflection_probe_bake_submit(&desc);
            if (h != 0u) {
                s_active_handle = h;
                s_active_target = r;
            }
        }
    }
}

void draw_comp_light_probe_group(JceLightProbeGroupComponent *g)
{
    if (!g) return;
    int n = g->probe_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.lpg.probeCount", "lpg"), &n, 1.0f, 0, JCE_LIGHT_PROBE_MAX)) {
        if (n < 0) n = 0; if (n > JCE_LIGHT_PROBE_MAX) n = JCE_LIGHT_PROBE_MAX;
        g->probe_count = n; insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lpg.dering", "lpg"), &g->dering)) insp_undo_bool(&g->dering);
    if (ImGui::TreeNode(jce_editor_i18n_id("inspector.lpg.probes", "lpg"))) {
        char lbl[32];
        for (int i = 0; i < g->probe_count; ++i) {
            snprintf(lbl, sizeof lbl, "Probe %d##lpg%d", i, i);
            ImGui::DragFloat3(lbl, g->positions[i], 0.05f);
            insp_track_edit();
        }
        ImGui::TreePop();
    }
}
