/*
 * jce_panel_inspector_lighting.cpp
 * Light, camera, virtual camera, reflection probe, and light probe group
 * component inspector drawers.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_tip.h"

#include <unordered_map>

extern "C" {
#include <jce/renderer/jce_reflection_probe_bake.h>
#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/os/core/jce_math.h>
}

/* The shared 32-slot layer-mask combo, defined below with the camera that used
 * to own it.  Declared here because the light panel is the first caller. */
static void draw_layer_mask_combo(uint32_t *field, const char *label,
                                  const char *imgui_id);

void draw_comp_light(JceScene *scene, JceEntity e, uint64_t flags)
{
    char lbl[256];

    int light_type = -1;
    if (flags & JCE_COMP_FLAG_DIR_LIGHT)        light_type = 0;
    else if (flags & JCE_COMP_FLAG_POINT_LIGHT) light_type = 1;
    else if (flags & JCE_COMP_FLAG_SPOT_LIGHT)  light_type = 2;
    /* Area lights are PRESENCE-gated: the 64-bit JCE_COMP_FLAG space is full,
     * so there is no bit to test and `flags` can never name this one. */
    else if (jce_scene_has_area_light(scene, e)) light_type = 3;
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
    } else if (light_type == 2) {
        JceSpotLight *l = jce_scene_get_spot_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    } else {
        JceAreaLight *l = jce_scene_get_area_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    }

    snprintf(lbl, sizeof(lbl), "%s###Color", jce_editor_i18n("light.color"));
    if (ImGui::ColorEdit3(lbl, color)) {
        if (light_type == 0)      { JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);   l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else if (light_type == 1) { JcePointLight       *l = jce_scene_get_point_light(scene, e); l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else if (light_type == 2) { JceSpotLight        *l = jce_scene_get_spot_light(scene, e);  l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else                      { JceAreaLight        *l = jce_scene_get_area_light(scene, e); l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
    }
    INSP_RESET_CTX("##rst_lightColor",
        if (light_type == 0)      { JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);   l->color = { 1.0f, 1.0f, 1.0f }; }
        else if (light_type == 1) { JcePointLight       *l = jce_scene_get_point_light(scene, e); l->color = { 1.0f, 1.0f, 1.0f }; }
        else if (light_type == 2) { JceSpotLight        *l = jce_scene_get_spot_light(scene, e);  l->color = { 1.0f, 1.0f, 1.0f }; }
        else                      { JceAreaLight        *l = jce_scene_get_area_light(scene, e); l->color = { 1.0f, 1.0f, 1.0f }; });
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
    if (ImGui::DragFloat(lbl, &intensity, 0.1f, 0.0f, 100.0f)) {
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = intensity; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = intensity; }
        else if (light_type == 2) { jce_scene_get_spot_light(scene, e)->intensity  = intensity; }
        else                      { jce_scene_get_area_light(scene, e)->intensity  = intensity; }
    }
    INSP_RESET_CTX("##rst_lightIntensity",
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = 1.0f; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = 1.0f; }
        else if (light_type == 2) { jce_scene_get_spot_light(scene, e)->intensity  = 1.0f; }
        else                      { jce_scene_get_area_light(scene, e)->intensity  = 1.0f; });
    insp_track_edit();

    const char *light_types[] = {
        jce_editor_i18n("light.directional"),
        jce_editor_i18n("light.point"),
        jce_editor_i18n("light.spot"),
        jce_editor_i18n_id("light.area", "Area (Rect)")
    };
    int new_type = light_type;
    snprintf(lbl, sizeof(lbl), "%s###Type", jce_editor_i18n("light.type"));
    if (ImGui::Combo(lbl, &new_type, light_types, 4) && new_type != light_type) {
        jce_state_begin_batch_edit();
        if (light_type == 0) jce_scene_remove_dir_light(scene, e);
        else if (light_type == 1) jce_scene_remove_point_light(scene, e);
        else if (light_type == 2) jce_scene_remove_spot_light(scene, e);
        else jce_scene_remove_area_light(scene, e);

        if (new_type == 0) {
            JceDirectionalLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.casts_shadow = false;
            l.cookie_texture.idx = UINT16_MAX;
            jce_scene_set_dir_light(scene, e, &l);
        } else if (new_type == 1) {
            JcePointLight l = {};
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            jce_scene_set_point_light(scene, e, &l);
        } else if (new_type == 2) {
            JceSpotLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            l.inner_cone_cos = cosf(25.0f * JCE_DEG2RAD);
            l.outer_cone_cos = cosf(35.0f * JCE_DEG2RAD);
            l.cookie_texture.idx = UINT16_MAX;
            l.ies_lut_texture.idx = UINT16_MAX;
            jce_scene_set_spot_light(scene, e, &l);
        } else {
            JceAreaLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            /* One unit square, not zero: a rectangle with no extent subtends
             * no solid angle and emits nothing, so a light switched to Area
             * would go dark and read as broken. */
            l.width  = 1.0f;
            l.height = 1.0f;
            jce_scene_set_area_light(scene, e, &l);
        }
        jce_state_end_batch_edit();
        light_type = new_type;
    }

    /* Direction control for directional + spot lights — Win-3D-Viewer-style
     * "Light Rotation": Azimuth (yaw around Y) + Elevation (height above the
     * horizon), which is far more intuitive than a raw vector. The viewport
     * arrow gizmo shows the resulting shine direction. Elevation 90 = straight
     * down (sun overhead); 0 = horizontal. */
    if (light_type == 0 || light_type == 2 || light_type == 3) {
        /* Operate on the ACTUAL world shine direction = entity rotation * the
           light's local direction field (what the renderer + viewport arrow
           use). On edit we write the world direction into the field AND clear
           the entity rotation, so dial / gizmo / lighting / shadow always agree.
           (Before: the dial edited only the raw field, so a non-identity entity
           rotation made the real light differ from the displayed angle — e.g.
           "90 deg" in the inspector but a long low-angle shadow.) */
        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_quat rot = xf ? jce_q_normalize(xf->rotation) : jce_q_identity();
        jce_vec3 cur = (light_type == 0)
            ? jce_scene_get_dir_light(scene, e)->direction
            : (light_type == 2)
                ? jce_scene_get_spot_light(scene, e)->direction
                : jce_scene_get_area_light(scene, e)->direction;
        cur = jce_v3_normalize(cur);
        if (cur.x == 0.0f && cur.y == 0.0f && cur.z == 0.0f)
            cur = jce_v3(0.0f, -1.0f, 0.0f);
        cur = jce_v3_normalize(jce_q_rotate(rot, cur));   /* world shine dir */

        const float RAD2DEG = 57.2957795f;
        const float DEG2RAD = 0.0174532925f;
        float ny = cur.y; if (ny < -1.0f) ny = -1.0f; if (ny > 1.0f) ny = 1.0f;
        float elev_deg = asinf(-ny) * RAD2DEG;            /* 90 = straight down */
        float azim_deg = atan2f(cur.x, cur.z) * RAD2DEG;

        bool changed = false;
        snprintf(lbl, sizeof(lbl), "%s###LightAzim",
                 jce_editor_i18n_id("inspector.light.azimuth", "Azimuth"));
        if (ImGui::DragFloat(lbl, &azim_deg, 1.0f, -360.0f, 360.0f, "%.0f deg"))
            changed = true;
        snprintf(lbl, sizeof(lbl), "%s###LightElev",
                 jce_editor_i18n_id("inspector.light.elevation", "Elevation"));
        if (ImGui::DragFloat(lbl, &elev_deg, 1.0f, -89.0f, 89.0f, "%.0f deg"))
            changed = true;

        /* 2D rotation dial (Win-3D-Viewer "Light Rotation" tray): drag the sun
           handle anywhere inside the ring. Angle from centre = Azimuth; distance
           from centre = Elevation (centre = straight down / overhead = 90, edge
           = horizon = 0). The 2D mapping avoids the degenerate azimuth when the
           light points straight down (the old "default 90 deg" problem). */
        {
            const float dial_r = 56.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 c  = ImVec2(p0.x + dial_r + 8.0f, p0.y + dial_r + 8.0f);
            ImGui::InvisibleButton("##lightDial",
                                   ImVec2((dial_r + 8.0f) * 2.0f,
                                          (dial_r + 8.0f) * 2.0f));
            bool active = ImGui::IsItemActive();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(c, dial_r, IM_COL32(70, 70, 70, 255), 48);
            dl->AddCircle(c, dial_r, IM_COL32(80, 150, 235, 255), 56, 2.5f);
            dl->AddCircleFilled(c, 3.0f, IM_COL32(190, 190, 190, 255), 10);
            float rn = (90.0f - elev_deg) / 90.0f;
            if (rn < 0.0f) rn = 0.0f;
            if (rn > 1.0f) rn = 1.0f;
            float a = azim_deg * DEG2RAD;
            ImVec2 sun = ImVec2(c.x + sinf(a) * dial_r * rn,
                                c.y - cosf(a) * dial_r * rn);
            dl->AddLine(c, sun, IM_COL32(80, 150, 235, 150), 1.5f);
            dl->AddCircleFilled(sun, 8.0f, IM_COL32(255, 210, 90, 255), 18);
            if (active) {
                ImVec2 m = ImGui::GetIO().MousePos;
                float dx = m.x - c.x, dy = m.y - c.y;
                float dist = sqrtf(dx * dx + dy * dy);
                float rnn = dist / dial_r;
                if (rnn > 1.0f) rnn = 1.0f;
                azim_deg = atan2f(dx, -dy) * RAD2DEG;
                elev_deg = 90.0f - rnn * 90.0f;
                changed = true;
            }
        }

        if (changed) {
            float el = elev_deg * DEG2RAD;
            float az = azim_deg * DEG2RAD;
            float horiz = cosf(el);
            jce_vec3 nd = jce_v3_normalize(
                jce_v3(horiz * sinf(az), -sinf(el), horiz * cosf(az)));
            /* Clear the entity rotation so the world shine == nd exactly (no
               hidden rotation offset), then store nd as the direction field. */
            if (xf) xf->rotation = jce_q_identity();
            if (light_type == 0)      jce_scene_get_dir_light(scene, e)->direction = nd;
            else if (light_type == 2) jce_scene_get_spot_light(scene, e)->direction = nd;
            else                      jce_scene_get_area_light(scene, e)->direction = nd;
        }
        INSP_RESET_CTX("##rst_lightDir",
            if (xf) xf->rotation = jce_q_identity();
            if (light_type == 0)      jce_scene_get_dir_light(scene, e)->direction = jce_v3(0.0f, -1.0f, 0.0f);
            else if (light_type == 2) jce_scene_get_spot_light(scene, e)->direction = jce_v3(0.0f, -1.0f, 0.0f);
            else                      jce_scene_get_area_light(scene, e)->direction = jce_v3(0.0f, -1.0f, 0.0f); );
        insp_track_edit();
    }

    if (light_type == 1 || light_type == 2 || light_type == 3) {
        float radius = (light_type == 1)
            ? jce_scene_get_point_light(scene, e)->radius
            : (light_type == 2)
                ? jce_scene_get_spot_light(scene, e)->radius
                : jce_scene_get_area_light(scene, e)->radius;
        snprintf(lbl, sizeof(lbl), "%s###Radius", jce_editor_i18n("collider.radius"));
        if (ImGui::DragFloat(lbl, &radius, 0.1f, 0.01f, 1000.0f)) {
            if (light_type == 1)      jce_scene_get_point_light(scene, e)->radius = radius;
            else if (light_type == 2) jce_scene_get_spot_light(scene, e)->radius  = radius;
            else                      jce_scene_get_area_light(scene, e)->radius  = radius;
        }
        insp_track_edit();
    }

    /* AREA: the two numbers that make it an area light rather than a point.
     * They are FULL extents in world units, which is what an artist measures
     * off the softbox or the window they are matching. */
    if (light_type == 3) {
        JceAreaLight *l = jce_scene_get_area_light(scene, e);
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.areaWidth", "Rect Width"),
                             &l->width, 0.05f, 0.001f, 1000.0f))
            { if (l->width < 0.001f) l->width = 0.001f; }
        insp_track_edit();
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.areaHeight", "Rect Height"),
                             &l->height, 0.05f, 0.001f, 1000.0f))
            { if (l->height < 0.001f) l->height = 0.001f; }
        insp_track_edit();
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.light.areaTwoSided",
                                               "Two Sided"), &l->two_sided))
            insp_undo_bool(&l->two_sided);
        ImGui::TextDisabled("%s", jce_editor_i18n_id(
            "inspector.light.areaNoShadow",
            "area lights do not cast shadows in this engine"));
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

        /* P1 — local (atlas) spot shadow toggle + depth bias. */
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.light.castsShadow", "CastsShadow"), &l->casts_shadow))
            insp_undo_bool(&l->casts_shadow);
        l->casts_shadow &&
            ImGui::DragFloat(jce_editor_i18n_id("inspector.light.shadowBias", "ShadowBias"),
                             &l->shadow_bias, 0.0005f, 0.0f, 0.05f);
        insp_track_edit();

        /* P3-E.5 — Light cookie + IES profile (spot lights). */
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("inspector.light.cookie"));
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.light.cookie.path", "CookiePath"),
                                  l->cookie_path, sizeof l->cookie_path,
                                  JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.light.cookie.clear", "CookieClear"))) {
            INSP_UNDO_SCOPE();
            l->cookie_path[0] = '\0';
            l->cookie_texture.idx = UINT16_MAX;
        }
        ImGui::DragFloat(jce_editor_i18n_id("inspector.light.cookieStrength", "CookieStrength"),
                         &l->cookie_strength, 0.01f, 0.0f, 1.0f);
        insp_track_edit();

        ImGui::TextUnformatted(jce_editor_i18n("inspector.light.iesProfile"));
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.light.ies.path", "IesPath"),
                                  l->ies_path, sizeof l->ies_path,
                                  JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("inspector.light.ies.clear", "IesClear"))) {
            INSP_UNDO_SCOPE();
            l->ies_path[0] = '\0';
            l->ies_lut_texture.idx = UINT16_MAX;
        }
    }

    if (light_type == 1) {
        /* P1b — point lights cast a (downward-hemisphere) local shadow. */
        JcePointLight *l = jce_scene_get_point_light(scene, e);
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.light.castsShadow", "CastsShadow"), &l->casts_shadow))
            insp_undo_bool(&l->casts_shadow);
        l->casts_shadow &&
            ImGui::DragFloat(jce_editor_i18n_id("inspector.light.shadowBias", "ShadowBias"),
                             &l->shadow_bias, 0.0005f, 0.0f, 0.05f);
        insp_track_edit();
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
            INSP_UNDO_SCOPE();
            l->cookie_path[0] = '\0';
            l->cookie_texture.idx = UINT16_MAX;
        }
        ImGui::DragFloat(jce_editor_i18n_id("inspector.light.dirCookieStrength", "DirCookieStrength"),
                         &l->cookie_strength, 0.01f, 0.0f, 1.0f);
        insp_track_edit();
    }

    /* RENDERING LAYERS -- which objects this light reaches.  Last, and shown
     * for every light type, because it is the one control on this panel that
     * is about the REST of the scene rather than about the light. */
    {
        uint32_t *lm = NULL;
        switch (light_type) {
        case 0: lm = &jce_scene_get_dir_light(scene, e)->layer_mask;   break;
        case 1: lm = &jce_scene_get_point_light(scene, e)->layer_mask; break;
        case 2: lm = &jce_scene_get_spot_light(scene, e)->layer_mask;  break;
        case 3: lm = &jce_scene_get_area_light(scene, e)->layer_mask;  break;
        default: break;
        }
        if (lm) {
            ImGui::Separator();
            draw_layer_mask_combo(lm, "inspector.light.layerMask", "LightLayerMask");
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.light.layerMask.help"));
        }
    }
}

static void draw_camera_extra_fields(JceCameraComponent *cam);

/* Unity's layer-mask dropdown: "Nothing" / "Everything" / "Mixed...", with a
 * checkbox per named layer.  There is no flags widget anywhere in this editor
 * to reuse -- zero ImGui::CheckboxFlags call sites -- so it is built here from
 * the same name array the entity Layer combo and the physics layer combo read
 * (JceProjectSettings::tags_layers.layers), because a mask that named layers
 * differently from the Layer combo directly above it would be worse than none.
 *
 * TWO consumers now: JceCameraComponent.culling_mask (which objects a camera
 * renders) and the four light components' layer_mask (which objects a light
 * reaches).  Same 32 slots, same names, same convention -- so one control, not
 * two that could drift apart in wording or in which slots they list.
 *
 * 0 is spelled "Everything" and not "Nothing": both fields document 0 as "no
 * filtering", which is what every zero-initialised component and every scene
 * authored before the fields existed carries. */
static void draw_layer_mask_combo(uint32_t *field, const char *label,
                                  const char *imgui_id)
{
    const JceProjectSettings *ps = jce_project_settings_current();
    uint32_t mask = *field;

    char preview[128];
    if (mask == 0u || mask == 0xFFFFFFFFu) {
        snprintf(preview, sizeof preview, "%s",
                 jce_editor_i18n("inspector.layerMask.everything"));
    } else {
        int n = 0, first = -1;
        for (int i = 0; i < 32; ++i)
            if (mask & (1u << i)) { if (first < 0) first = i; ++n; }
        if (n == 0)
            snprintf(preview, sizeof preview, "%s",
                     jce_editor_i18n("inspector.layerMask.nothing"));
        else if (n == 1) {
            const char *nm = ps ? ps->tags_layers.layers[first] : "";
            if (!nm || !nm[0]) nm = jce_editor_i18n("inspector.layerMask.unnamed");
            snprintf(preview, sizeof preview, "%s", nm);
        } else {
            snprintf(preview, sizeof preview, "%s",
                     jce_editor_i18n("inspector.layerMask.mixed"));
        }
    }

    uint32_t next = mask;
    if (ImGui::BeginCombo(jce_editor_i18n_id(label, imgui_id), preview)) {
        if (ImGui::Selectable(
                jce_editor_i18n("inspector.layerMask.everything"),
                mask == 0u || mask == 0xFFFFFFFFu))
            next = 0u;              /* 0 == no filtering; see the header */
        if (ImGui::Selectable(
                jce_editor_i18n("inspector.layerMask.nothing"), false))
            next = 0x80000000u;     /* a bit no layer uses: masks everything
                                     * without colliding with 0 == "all" */
        ImGui::Separator();
        const uint32_t eff = (mask == 0u) ? 0xFFFFFFFFu : mask;
        for (int i = 0; i < 32; ++i) {
            const char *nm = ps ? ps->tags_layers.layers[i] : "";
            /* Only NAMED slots, matching the entity Layer combo -- an unnamed
             * slot has no entity that can be assigned to it from the UI. */
            if (i != 0 && (!nm || !nm[0])) continue;
            char row[96];
            snprintf(row, sizeof row, "%d: %s", i,
                     (nm && nm[0]) ? nm
                                   : jce_editor_i18n("inspector.layerMask.defaultLayer"));
            bool on = (eff & (1u << i)) != 0u;
            if (ImGui::Checkbox(row, &on))
                next = on ? (eff | (1u << i)) : (eff & ~(1u << i));
        }
        ImGui::EndCombo();
    }
    /* OUTSIDE the combo predicate: a Combo's activate/deactivate fire on frames
     * where nothing changed, and tracking inside the body is what
     * check_inspector_undo_scope.py exists to catch. */
    if (next != mask)
        insp_undo_set(field, next);
    insp_track_edit();
}

/* The camera's culling mask, in the shared control above. */
static void draw_camera_culling_mask(JceCameraComponent *cam)
{
    draw_layer_mask_combo(&cam->culling_mask, "inspector.camera.cullingMask",
                          "CamCullMask");
}

void draw_comp_camera(JceCameraComponent *cam)
{
    static const JceReflectType *t = jce_reflect_find("Camera");
    if (t) {
        /* fov / near / far / primary / ortho.  NOT a return: the three fields
         * below are not in the reflect table, and this used to `return` right
         * here -- so Stack Index, Clear Mode and their unwired badges have
         * never rendered, in any build.  The reflect page is five rows; the
         * struct has eight members. */
        jce_reflect_draw(t, cam);
        draw_camera_culling_mask(cam);
        draw_camera_extra_fields(cam);
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

    draw_camera_culling_mask(cam);
    draw_camera_extra_fields(cam);
}

/* stack_index + clear_mode.
 *
 * BOTH are wired now.  clear_mode reaches JceSceneRenderConfig.draw_skybox for
 * every camera; for an overlay it also reaches
 * JceSceneRenderConfig.camera_clear_mode, and for a BASE camera it reaches
 * jce_offscreen_target_prepare_keep through jce_scene_camera_clear_keeps --
 * so all four modes now do something wherever the camera sits.
 * stack_index picks the overlays: 0 is the base camera (the primary),
 * 1..3 are drawn on top of it in that order by
 * jce_scene_renderer_render_camera_overlay, in the editor Game View and in the
 * shipped exe alike.
 *
 * Both badges are therefore gone.  A badge that outlives the wire says the
 * opposite of what the code does -- see the VCam name field below, which
 * records the same rule. */
static void draw_camera_extra_fields(JceCameraComponent *cam)
{
    /* Orthographic height in WORLD UNITS.  Only the height is authored: the
     * width follows the viewport aspect, so a board framed in a 16:9 window
     * is framed the same in a 4:3 one.  0 leaves jce_camera_create's own
     * 800x450 default, which is what every orthographic scene silently got
     * before this field existed -- the flag parsed, the mode applied, and
     * nothing could set the extent.
     *
     * Shown only when the camera IS orthographic, because for a perspective
     * camera the number has no effect and a control that does nothing is
     * worse than none. */
    if (cam->ortho) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.camera.orthoSize",
                                            "CamOrthoSize"),
                         &cam->ortho_size, 0.25f, 0.0f, 10000.0f, "%.2f");
        insp_track_edit();
        if (cam->ortho_size <= 0.0f)
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.camera.orthoSize.unset"));
    }

    int stack_idx = (int)cam->stack_index;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.camera.stackIndex", "CamStack"), &stack_idx, 1.0f, 0, 3)) {
        cam->stack_index = (uint8_t)stack_idx;
    }
    insp_track_edit();
    /* What this camera IS, in one line, because 0-vs-1 is the whole difference
     * between "the scene" and "the thing drawn on top of the scene" and the
     * number alone does not say it. */
    if (cam->stack_index == 0)
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.camera.stackIndex.base"));
    else
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.camera.stackIndex.overlay"));

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
            if (ImGui::Selectable(jce_editor_i18n(clear_modes[i]), sel))
                insp_undo_set(&cam->clear_mode, (uint8_t)i);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    /* The mode decides the sky for every camera, and it decides what survives
     * from the previous frame -- for an overlay, the base camera's image; for
     * a base camera, its own last frame.  Which sentence applies depends on
     * stack_index, so the hint does too: a designer reading "keeps the base
     * camera's image" on a base camera would be reading about a code path that
     * camera does not take.
     *
     * The base-camera branch is the new one.  It did not exist while those two
     * modes did nothing there, and a panel that stays silent after the code
     * starts acting is the same defect as a badge that outlives the wire --
     * just quieter. */
    const bool keeps = cam->clear_mode == (uint8_t)JCE_CAMERA_CLEAR_DEPTH_ONLY ||
                       cam->clear_mode == (uint8_t)JCE_CAMERA_CLEAR_NOTHING;
    if (cam->stack_index != 0) {
        if (cam->clear_mode == (uint8_t)JCE_CAMERA_CLEAR_SKYBOX)
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.camera.clearMode.overlaySkyRefused"));
        else if (keeps)
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.camera.clearMode.overlayActive"));
    } else if (keeps) {
        ImGui::TextDisabled("%s", jce_editor_i18n(
            cam->clear_mode == (uint8_t)JCE_CAMERA_CLEAR_NOTHING
                ? "inspector.camera.clearMode.baseKeepBoth"
                : "inspector.camera.clearMode.baseKeepColor"));
        /* The one caveat, said where the choice is made rather than left for
         * the log: the shipped exe honours a keep only through the offscreen
         * target, which it uses when post-processing is on.  Straight to the
         * backbuffer it clears fully, because "what was already there" is a
         * swapchain image from two or three frames ago. */
        ImGui::TextDisabled("%s",
            jce_editor_i18n("inspector.camera.clearMode.baseKeepNeedsPostFx"));
    }
}

void draw_comp_virtual_camera(JceVirtualCameraComponent *vc)
{
    if (!vc) return;
    /* Read now: jce_vcam_system_set_active_by_name() addresses cameras by this
     * string, so a script, a trigger or the VCam Manager's Cut button can say
     * "cut to BossIntro".  The unwired badge that stood here came off in the
     * same commit that wired it -- a badge that outlives the wire says the
     * opposite of what the code does. */
    ImGui::InputText(jce_editor_i18n_id("inspector.vcam.name", "vcam"), vc->vcam_name, sizeof vc->vcam_name);
    insp_track_edit();
    int prio = vc->priority;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.vcam.priority", "vcam"), &prio, 1, -1000, 1000)) {
        vc->priority = prio;
    }
    insp_track_edit();
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("inspector.vcam.solo", "vcam"))) {
        INSP_UNDO_SCOPE();
        vc->priority = 9999;
        vc->active   = true;
    }
    jce_editor::help_tip(jce_editor_i18n("inspector.vcam.soloTooltip"));
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vcam.active", "vcam"), &vc->active))
        insp_undo_bool(&vc->active);

    static const char *track_modes[] = {
        "None", "Follow", "Look At", "Follow + Look At"
    };
    int tm = vc->track_mode; if (tm < 0 || tm > 3) tm = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.vcam.trackMode", "vcam"), &tm, track_modes, 4))
        insp_undo_set(&vc->track_mode, tm);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vcam.fov", "vcam"), &vc->fov_deg, 0.5f, 1.0f, 179.0f, "%.1f");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vcam.damping", "vcam"), &vc->damping, 0.01f, 0.0f, 1.0f, "%.2f");
    insp_unwired_field_badge();   /* damping */
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
    }
    insp_track_edit();
    int lt = (int)vc->look_at_target;
    if (ImGui::InputInt(jce_editor_i18n_id("inspector.vcam.lookAtTargetEntity", "vcam"), &lt)) {
        vc->look_at_target = (uint64_t)(lt < 0 ? 0 : lt);
    }
    insp_track_edit();
    ImGui::TextDisabled(jce_editor_i18n("inspector.vcam.targetNote"));
    ImGui::Separator();
    ImGui::TextDisabled(jce_editor_i18n("inspector.vcam.playModeNote"));
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.vcam.priorityNote"));
}

/* WHAT THE ENGINE ACTUALLY READS off this component: baked_cubemap_path,
 * custom_hdr_path, mode, box_offset, intensity, box_projection and box_size.
 * That is seven of eleven.  sr_gather_rprobe_cb (jce_sr_cull.c) picks the
 * probe whose box_offset centre is NEAREST the camera, resolves its source
 * cubemap through sr_rprobe_source_path (mode CUSTOM takes custom_hdr_path;
 * everything else takes the baked one) and binds it at that intensity, with
 * box projection applied from box_size when box_projection is set.
 *
 * THIS COMMENT WAS ITSELF STALE, which is the reason it now names its own
 * date: 17baccc0 wired box_projection and box_size and never opened this
 * file, so the text above kept saying they "change nothing about what is
 * rendered" for as long as they worked.  A panel comment describing the
 * engine has to be re-read whenever the engine changes, and nothing enforces
 * that -- so keep it short and keep it true.
 *
 * near_clip, far_clip and hdr lost their badges together, for one reason:
 * LIVE CAPTURE now exists.  Both bake buttons call
 * jce_scene_reflection_probe_capture first and fall back to the procedural
 * bake only when there is no renderer, and the capture reads all three --
 * near/far as the cube camera's clip planes, hdr as the format it captures,
 * convolves and stores in (RGBA16F instead of RGBA8, so a sun or a lamp in
 * the scene reflects brighter than white paper).  The paragraph above said
 * they "wait on a capture nobody performs", which was true right up until
 * somebody performed it -- the same staleness this comment is about, one
 * more time.  resolution lost its badge at 2f9b8c95 (the bake writes at the
 * authored face size) and this comment kept it for two commits.
 *
 * blend_distance lost its badge with the influence volume: box_size +
 * blend_distance now decide WHETHER this probe reaches the camera at all
 * (sr_rprobe_influences).  What it still does NOT do is WEIGHT that
 * influence -- Unity fades one probe into the next across the band, which
 * needs two environment cubemaps bound at once, and fs_pbr_body.sh has all
 * sixteen sampler stages occupied.  So the band is a hard boundary, and the
 * tooltip beside the control says so rather than leaving a designer to infer
 * a fade that is not there. */
/* Takes the scene and entity so the BAKE can go through the engine: the
 * artefact name is derived from the ENTITY, which is what gives one probe
 * one file across runs and across both panels. */
void draw_comp_reflection_probe(JceScene *scene, JceEntity e,
                                JceReflectionProbeComponent *r)
{
    if (!r) return;
    const char *modes[] = { jce_editor_i18n("inspector.refl.mode.baked"),
                            jce_editor_i18n("inspector.refl.mode.realtime"),
                            jce_editor_i18n("inspector.refl.mode.custom"),
                            jce_editor_i18n("inspector.refl.mode.planar") };
    int m = r->mode; if (m < 0 || m > 3) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.refl.mode", "refl"), &m, modes, 4))
        insp_undo_set(&r->mode, m);
    const bool planar = (r->mode == JCE_REFLECTION_PROBE_PLANAR);
    if (planar) {
        /* THE MIRROR'S OWN FIELDS, and only in this mode.
         *
         * A cube probe captures a room from a point; a planar probe mirrors
         * the camera through a plane and composites the result in screen
         * space over every pixel on that plane -- a polished floor, a wet
         * road, a still lake -- rather than into one material's shader.
         * jce_scene.h and jce_planar_reflection.h say why it has to work that
         * way: fs_pbr has no free sampler slot to hand a mirror texture.
         *
         * The three fields below are what decide which pixels ARE the mirror,
         * so they are shown together and above the cube fields rather than
         * mixed in with them.  Resolution, near/far clip, box projection and
         * HDR belong to the cube capture and are hidden here: a control that
         * does nothing in the selected mode is a question the user cannot
         * answer. */
        ImGui::SeparatorText(jce_editor_i18n("inspector.refl.planar"));
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.planeNormal", "refl"),
                          r->plane_normal, 0.01f, -1.0f, 1.0f, "%.3f");
        insp_track_edit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("inspector.refl.planeNormal.tip"));
        ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.planarThickness", "refl"),
                         &r->planar_thickness, 0.005f, 0.0f, 5.0f, "%.3f");
        insp_track_edit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("inspector.refl.planarThickness.tip"));
        ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.planarAngle", "refl"),
                         &r->planar_angle_deg, 0.5f, 0.0f, 89.0f, "%.1f");
        insp_track_edit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("inspector.refl.planarAngle.tip"));
        /* THE ONE LIMIT A USER CAN HIT WITHOUT BEING TOLD.  A second probe is
         * a second full scene render per frame, so the nearest one containing
         * the camera wins and the others do nothing -- silently, because a
         * probe that does not draw looks exactly like a probe out of range. */
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.refl.planar.oneAtATime"));
        ImGui::Separator();
    }

    static const int res_choices[] = { 16, 32, 64, 128, 256, 512, 1024 };
    int ri = 3;
    for (int i = 0; i < 7; ++i) if (res_choices[i] == r->resolution) { ri = i; break; }
    if (!planar &&
        ImGui::Combo(jce_editor_i18n_id("inspector.refl.resolution", "refl"), &ri, "16\0" "32\0" "64\0" "128\0" "256\0" "512\0" "1024\0\0"))
        insp_undo_set(&r->resolution, res_choices[ri]);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.intensity", "refl"),      &r->intensity,      0.05f, 0.0f, 100.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.blendDistance", "refl"), &r->blend_distance, 0.05f, 0.0f, 100.0f, "%.2f"); insp_track_edit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.refl.blendDistance.tip"));
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.boxSize", "refl"),   r->box_size,   0.1f); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.boxOffset", "refl"), r->box_offset, 0.05f); insp_track_edit();
    if (!planar) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.nearClip", "refl"), &r->near_clip, 0.01f, 0.001f, 10.0f, "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.farClip", "refl"),  &r->far_clip,  1.0f, 0.1f, 100000.0f, "%.1f"); insp_track_edit();
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.refl.boxProjection", "refl"), &r->box_projection)) insp_undo_bool(&r->box_projection);
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.refl.hdr", "refl"), &r->hdr)) insp_undo_bool(&r->hdr);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("inspector.refl.hdr.tip"));
    }
    if (r->mode == JCE_REFLECTION_PROBE_CUSTOM) {
        jce_draw_path_input_asset(jce_editor_i18n_id("inspector.refl.customHdr", "refl"), r->custom_hdr_path, sizeof r->custom_hdr_path, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(r->custom_hdr_path, sizeof r->custom_hdr_path);
        /* Badge gone: sr_rprobe_source_path binds THIS file in CUSTOM mode.
         * It must be a cubemap CONTAINER (.ktx / .ktx2 / .dds) -- an
         * equirectangular .hdr is one lat-long image and needs a projection
         * pass that does not exist, and handing it to the loader would burn
         * one of the eight probe-cache slots permanently. */
        ImGui::TextDisabled("%s",
            jce_editor_i18n("inspector.refl.customHdr.cubeOnly"));
    }

    /* ── Bake (P3-E.3) ─────────────────────────────────────────────── */
    ImGui::Separator();

    static JceReflectionProbeBakeHandle s_active_handle = 0;
    static JceReflectionProbeComponent *s_active_target = nullptr;
    static bool s_include_dynamic = true;

    /* The bake-size combo that used to live here is GONE, and with it a
     * std::unordered_map keyed by the raw component pointer.  It offered
     * {128, 256, 512} while the Resolution combo a few lines above offers
     * {16 .. 1024}, so the number a designer picked was not the number that
     * got baked; and being keyed by a pointer it dangled when a probe was
     * deleted, leaked a node per distinct probe ever inspected, and needed a
     * 64-entry cap whose only job was to nurse it.  The component's own
     * `resolution` is the one writer now, and the engine reads it. */

    ImGui::Checkbox(jce_editor_i18n("inspector.reflection_probe.include_dynamic"),
                    &s_include_dynamic);

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
                /* The path is recorded at SUBMIT below, from what the engine
                 * returned.  Re-deriving it here is exactly how the two panels
                 * drifted onto two naming schemes for the same probe. */
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
            /* The engine composes the desc from the component, so the
             * Resolution combo a few lines above is finally the number that
             * gets baked.  A CUSTOM probe returns 0 without submitting: it
             * already has an authored source, and baking would overwrite the
             * path while sr_rprobe_source_path still preferred the custom one. */
            char out[256] = {0};
            /* CAPTURE THE REAL SCENE when the renderer can.  Six 90-degree
             * renders read back one face per frame; the procedural gradient
             * every probe used to contain is the fallback for a host that has
             * no renderer, not the plan.  Both produce the same artefact
             * path, so the component records it either way. */
            if (jce_scene_reflection_probe_capture(scene, e, out,
                                                   (int)sizeof out)) {
                snprintf(r->baked_cubemap_path,
                         sizeof r->baked_cubemap_path, "%s", out);
                s_active_handle = 0u;   /* the capture submits its own bake */
                s_active_target = r;
            } else {
                JceReflectionProbeBakeHandle h =
                    jce_scene_reflection_probe_bake(scene, e, s_include_dynamic,
                                                    out, (int)sizeof out);
                if (h != 0u) {
                    snprintf(r->baked_cubemap_path,
                             sizeof r->baked_cubemap_path, "%s", out);
                    s_active_handle = h;
                    s_active_target = r;
                }
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
        g->probe_count = n;
    }
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lpg.dering", "lpg"), &g->dering)) insp_undo_bool(&g->dering);
    /* Badge gone: the probe bake now windows the L1/L2 bands when this is set
     * (jce_lightmapper_sh9_dering).  It is a BAKE setting -- toggling it edits
     * nothing until the next bake. */
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
