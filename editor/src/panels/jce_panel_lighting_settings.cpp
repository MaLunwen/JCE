/*
 * jce_panel_lighting_settings.cpp  Unity-parity Lighting Settings window.
 *
 * Single, consolidated editor surface for scene-wide lighting. Mirrors
 * Unity's "Window > Rendering > Lighting" panel. Replaces the older
 * `jce_panel_lighting.cpp` (merged in P5-A.4 — skybox component editing,
 * scene-light enumeration, project-settings persistence for ambient+fog,
 * and the renderer-facing fog/ambient accessors all moved here).
 *
 * Per-component (Light / Sun / Probe) inspection still lives in
 * `inspector_lighting.cpp` — this panel is strictly scene-level.
 *
 * Sections (collapsing headers, all default-open):
 *   1. Environment           Sky type / HDRI / sky+ambient intensity /
 *                            reflection probe slot, plus the per-entity
 *                            Skybox component editor and the ambient
 *                            colour persisted to project settings.
 *   2. Directional Light     Sun colour, intensity, yaw/pitch direction,
 *                            shadow toggle.
 *   3. Shadows (CSM)         Distance, cascade count, split lambda,
 *                            shadow-map resolution, soft filter.
 *   4. Image-Based Lighting  Convolve trigger, SH band readout,
 *                            specular mip count readout.
 *   5. Lights in Scene       Quick-edit colour/intensity for every
 *                            Directional / Point / Spot light, with
 *                            ping-to-select.
 *   6. Fog                   Unity-style mode picker (None/Linear/
 *                            Exp/Exp²) feeding the volumetric-fog
 *                            renderer params (height-falloff aware).
 *
 * The ambient + fog values round-trip through `JceProjectSettings` so
 * they survive editor restarts. Sky/sun/shadow/IBL state is currently
 * per-session and intended to be picked up by the scene serializer.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "core/jce_project_settings.h"
#include "dialogs/jce_path_input.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_volumetric_fog.h>
}

#ifndef JCE_PI_F
#define JCE_PI_F 3.14159265358979323846f
#endif

namespace {

/* ── Editor-side authoritative state ───────────────────────────────── */

enum SkyType { SKY_NONE = 0, SKY_PROCEDURAL = 1, SKY_HDRI = 2 };
enum SoftShadow { SOFT_OFF = 0, SOFT_PCF = 1, SOFT_VSM = 2 };
enum FogMode { FOG_NONE = 0, FOG_LINEAR = 1, FOG_EXP = 2, FOG_EXP2 = 3 };

struct JceLightingSettings {
    /* Environment (in-session) */
    int   sky_type            = SKY_PROCEDURAL;
    char  hdri_path[260]      = {0};
    float sky_intensity       = 1.0f;
    char  reflection_probe[260] = {0};

    /* Ambient (persisted to JceProjectSettings) */
    float ambient_color[3]    = { 0.1f, 0.1f, 0.12f };
    float ambient_intensity   = 1.0f;

    /* Directional Light (Sun, in-session) */
    float sun_color[3]        = { 1.0f, 0.96f, 0.9f };
    float sun_intensity       = 3.0f;
    float sun_yaw_deg         = 35.0f;
    float sun_pitch_deg       = -55.0f;
    bool  sun_cast_shadows    = true;

    /* Shadows (CSM, in-session) */
    float shadow_distance     = 100.0f;
    int   cascade_count       = 4;
    float split_lambda        = 0.7f;
    int   shadow_resolution   = 2048;
    int   soft_shadow_mode    = SOFT_PCF;

    /* IBL (in-session) */
    int   ibl_sh_bands        = 3;
    int   ibl_spec_mips       = 5;
    bool  ibl_dirty           = false;

    /* Fog (persisted to JceProjectSettings; mode is in-session) */
    int   fog_mode            = FOG_NONE;
    bool  fog_enabled         = false;          /* legacy on/off, mirrors mode != NONE */
    float fog_color[3]        = { 0.7f, 0.75f, 0.85f };
    float fog_density         = 0.02f;
    float fog_start           = 10.0f;          /* linear mode */
    float fog_end             = 200.0f;         /* linear mode */
    float fog_height_falloff  = 0.05f;
    float fog_height_origin   = 0.0f;
};

JceLightingSettings g_lit;
bool g_lit_initialized = false;

/* ── Persistence (project settings) ─────────────────────────────── */

void lit_ensure_init(void)
{
    if (g_lit_initialized) return;
    /* Cache is pre-warmed by jce_editor_init; pointer read, no disk I/O. */
    const JceProjectSettings *ps = jce_project_settings_current();
    if (ps) {
        g_lit.ambient_color[0]    = ps->rendering.ambient_color[0];
        g_lit.ambient_color[1]    = ps->rendering.ambient_color[1];
        g_lit.ambient_color[2]    = ps->rendering.ambient_color[2];
        g_lit.ambient_intensity   = ps->rendering.ambient_intensity;
        g_lit.fog_enabled         = ps->rendering.fog_enabled;
        g_lit.fog_mode            = ps->rendering.fog_enabled ? FOG_EXP : FOG_NONE;
        g_lit.fog_color[0]        = ps->rendering.fog_color[0];
        g_lit.fog_color[1]        = ps->rendering.fog_color[1];
        g_lit.fog_color[2]        = ps->rendering.fog_color[2];
        g_lit.fog_density         = ps->rendering.fog_density;
        g_lit.fog_height_falloff  = ps->rendering.fog_height_falloff;
        g_lit.fog_height_origin   = ps->rendering.fog_height_origin;
    }
    g_lit_initialized = true;
}

void lit_save_to_project_settings(void)
{
    const JceProjectSettings *cur = jce_project_settings_current();
    JceProjectSettings ps;
    if (cur) ps = *cur; else jce_project_settings_defaults(&ps);
    ps.rendering.ambient_color[0]   = g_lit.ambient_color[0];
    ps.rendering.ambient_color[1]   = g_lit.ambient_color[1];
    ps.rendering.ambient_color[2]   = g_lit.ambient_color[2];
    ps.rendering.ambient_intensity  = g_lit.ambient_intensity;
    ps.rendering.fog_enabled        = (g_lit.fog_mode != FOG_NONE);
    ps.rendering.fog_color[0]       = g_lit.fog_color[0];
    ps.rendering.fog_color[1]       = g_lit.fog_color[1];
    ps.rendering.fog_color[2]       = g_lit.fog_color[2];
    ps.rendering.fog_density        = g_lit.fog_density;
    ps.rendering.fog_height_falloff = g_lit.fog_height_falloff;
    ps.rendering.fog_height_origin  = g_lit.fog_height_origin;
    jce_project_settings_save(&ps);
}

/* ── Scene light collection ─────────────────────────────────────── */

struct LightCollect {
    uint32_t dir[16];   int n_dir   = 0;
    uint32_t point[64]; int n_point = 0;
    uint32_t spot[64];  int n_spot  = 0;
    uint32_t skybox    = 0;
    bool     has_skybox= false;
};

void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    LightCollect *c = (LightCollect *)ud;
    if (jce_scene_has_dir_light(s, e) && c->n_dir < 16)
        c->dir[c->n_dir++] = (uint32_t)e;
    if (jce_scene_has_point_light(s, e) && c->n_point < 64)
        c->point[c->n_point++] = (uint32_t)e;
    if (jce_scene_has_spot_light(s, e) && c->n_spot < 64)
        c->spot[c->n_spot++] = (uint32_t)e;
    if (!c->has_skybox && jce_scene_has_skybox(s, e)) {
        c->has_skybox = true;
        c->skybox     = (uint32_t)e;
    }
}

void ping_entity(uint32_t id)
{
    jce_state_select_entity(id, false);
}

/* ── Section helpers ──────────────────────────────────────────────── */

bool draw_environment(JceScene *scene, const LightCollect &c)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.environment"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    bool changed = false;
    const char *items[3] = {
        jce_editor_i18n("panel.lighting.env.sky_type.none"),
        jce_editor_i18n("panel.lighting.env.sky_type.procedural"),
        jce_editor_i18n("panel.lighting.env.sky_type.hdri"),
    };
    changed |= ImGui::Combo(jce_editor_i18n("panel.lighting.env.sky_type"),
                            &g_lit.sky_type, items, 3);

    if (g_lit.sky_type == SKY_HDRI) {
        ImGui::PushID("hdri");
        if (ImGui::InputText(jce_editor_i18n("panel.lighting.env.hdri"),
                             g_lit.hdri_path, sizeof(g_lit.hdri_path))) {
            changed = true;
            g_lit.ibl_dirty = true;
        }
        ImGui::PopID();
    }

    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.env.sky_intensity"),
        &g_lit.sky_intensity, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.env.ambient_intensity"),
        &g_lit.ambient_intensity, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::ColorEdit3(jce_editor_i18n("lighting.ambientColor"),
                                 g_lit.ambient_color);

    ImGui::PushID("refprobe");
    changed |= ImGui::InputText(
        jce_editor_i18n("panel.lighting.env.reflection_probe"),
        g_lit.reflection_probe, sizeof(g_lit.reflection_probe));
    ImGui::PopID();

    /* Skybox component editor — surfaces the scene's first JceSkybox
     * so the user doesn't have to find the right entity. */
    ImGui::Spacing();
    ImGui::TextDisabled("%s", jce_editor_i18n("lighting.skybox"));
    if (c.has_skybox && scene) {
        JceSkyboxComponent *sky = jce_scene_get_skybox(scene, (JceEntity)c.skybox);
        if (sky) {
            const char *name = jce_scene_entity_name(scene, (JceEntity)c.skybox);
            ImGui::PushID((int)c.skybox);
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.skybox"));
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("lighting.ping")))
                ping_entity(c.skybox);
            jce_draw_path_input_asset(jce_editor_i18n("skybox.hdrPath"),
                                      sky->hdr_path, sizeof(sky->hdr_path));
            ImGui::DragFloat(jce_editor_i18n("skybox.rotation"),
                             &sky->rotation, 1.0f, 0.0f, 360.0f, "%.1f deg");
            ImGui::DragFloat(jce_editor_i18n("skybox.exposure"),
                             &sky->exposure, 0.01f, 0.0f, 8.0f);
            ImGui::Checkbox(jce_editor_i18n("skybox.useAsIbl"),
                            &sky->use_as_ibl);
            ImGui::PopID();
        }
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("lighting.noSkybox"));
    }
    return changed;
}

bool draw_directional_light(void)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.directional_light"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    bool changed = false;
    changed |= ImGui::ColorEdit3(jce_editor_i18n("panel.lighting.sun.color"),
                                 g_lit.sun_color);
    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.sun.intensity"),
        &g_lit.sun_intensity, 0.0f, 10.0f, "%.2f");

    ImGui::TextDisabled("%s",
        jce_editor_i18n("panel.lighting.sun.direction"));
    changed |= ImGui::SliderFloat(jce_editor_i18n("panel.lighting.sun.yaw"),
                                  &g_lit.sun_yaw_deg,
                                  -180.0f, 180.0f, "%.1f\xc2\xb0");
    changed |= ImGui::SliderFloat(jce_editor_i18n("panel.lighting.sun.pitch"),
                                  &g_lit.sun_pitch_deg,
                                  -90.0f, 90.0f, "%.1f\xc2\xb0");

    changed |= ImGui::Checkbox(
        jce_editor_i18n("panel.lighting.sun.cast_shadows"),
        &g_lit.sun_cast_shadows);
    return changed;
}

bool draw_shadows(void)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.shadows"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    bool changed = false;
    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.shadows.distance"),
        &g_lit.shadow_distance, 10.0f, 500.0f, "%.0f m");

    const char *cc_items[4] = { "1", "2", "3", "4" };
    int cc_idx = g_lit.cascade_count - 1;
    if (cc_idx < 0) cc_idx = 0;
    if (cc_idx > 3) cc_idx = 3;
    if (ImGui::Combo(jce_editor_i18n("panel.lighting.shadows.cascade_count"),
                     &cc_idx, cc_items, 4)) {
        g_lit.cascade_count = cc_idx + 1;
        changed = true;
    }

    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.shadows.split_lambda"),
        &g_lit.split_lambda, 0.0f, 1.0f, "%.2f");

    const char *res_labels[4] = { "512", "1024", "2048", "4096" };
    const int   res_values[4] = { 512, 1024, 2048, 4096 };
    int res_idx = 2;
    for (int i = 0; i < 4; i++)
        if (res_values[i] == g_lit.shadow_resolution) { res_idx = i; break; }
    if (ImGui::Combo(jce_editor_i18n("panel.lighting.shadows.resolution"),
                     &res_idx, res_labels, 4)) {
        g_lit.shadow_resolution = res_values[res_idx];
        changed = true;
    }

    const char *soft_items[3] = {
        jce_editor_i18n("panel.lighting.shadows.soft.off"),
        jce_editor_i18n("panel.lighting.shadows.soft.pcf"),
        jce_editor_i18n("panel.lighting.shadows.soft.vsm"),
    };
    changed |= ImGui::Combo(jce_editor_i18n("panel.lighting.shadows.soft"),
                            &g_lit.soft_shadow_mode, soft_items, 3);
    return changed;
}

bool draw_ibl(void)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.ibl"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    bool changed = false;
    bool can_convolve = (g_lit.sky_type == SKY_HDRI &&
                         g_lit.hdri_path[0] != '\0');
    ImGui::BeginDisabled(!can_convolve);
    if (ImGui::Button(jce_editor_i18n("panel.lighting.ibl.convolve"))) {
        /* TODO: wire to jce_ibl_generate() once the HDRI is resolved to
         * a JceTexture handle. For now mark cache fresh. */
        g_lit.ibl_dirty = false;
        changed = true;
    }
    ImGui::EndDisabled();
    if (g_lit.ibl_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "*");
    }

    ImGui::Text("%s: L0..L%d (%d coeffs)",
                jce_editor_i18n("panel.lighting.ibl.sh_bands"),
                g_lit.ibl_sh_bands - 1,
                g_lit.ibl_sh_bands * g_lit.ibl_sh_bands);
    ImGui::Text("%s: %d",
                jce_editor_i18n("panel.lighting.ibl.spec_mips"),
                g_lit.ibl_spec_mips);
    return changed;
}

void draw_scene_lights(JceScene *scene, const LightCollect &c)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("lighting.section.lights"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (!scene) {
        ImGui::TextDisabled("(none)");
        return;
    }

    char lbl[160];
    /* Directional */
    std::snprintf(lbl, sizeof(lbl), "%s (%d)",
                  jce_editor_i18n("lighting.directional"), c.n_dir);
    if (ImGui::TreeNodeEx(lbl, ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < c.n_dir; i++) {
            uint32_t id = c.dir[i];
            ImGui::PushID((int)id);
            JceDirectionalLight *l = jce_scene_get_dir_light(scene, (JceEntity)id);
            const char *name = jce_scene_entity_name(scene, (JceEntity)id);
            if (ImGui::SmallButton("\xe2\x86\x92")) ping_entity(id);
            ImGui::SameLine();
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.light"));
            if (l) {
                float col[3] = { l->color.x, l->color.y, l->color.z };
                if (ImGui::ColorEdit3("##col", col)) {
                    l->color.x = col[0]; l->color.y = col[1]; l->color.z = col[2];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::DragFloat("##i", &l->intensity, 0.05f, 0.0f, 100.0f);
            }
            ImGui::PopID();
        }
        if (c.n_dir == 0) ImGui::TextDisabled("(none)");
        ImGui::TreePop();
    }

    /* Point */
    std::snprintf(lbl, sizeof(lbl), "%s (%d)",
                  jce_editor_i18n("lighting.point"), c.n_point);
    if (ImGui::TreeNodeEx(lbl)) {
        for (int i = 0; i < c.n_point; i++) {
            uint32_t id = c.point[i];
            ImGui::PushID((int)id + 0x10000);
            JcePointLight *l = jce_scene_get_point_light(scene, (JceEntity)id);
            const char *name = jce_scene_entity_name(scene, (JceEntity)id);
            if (ImGui::SmallButton("\xe2\x86\x92")) ping_entity(id);
            ImGui::SameLine();
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.light"));
            if (l) {
                float col[3] = { l->color.x, l->color.y, l->color.z };
                if (ImGui::ColorEdit3("##col", col)) {
                    l->color.x = col[0]; l->color.y = col[1]; l->color.z = col[2];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::DragFloat("##i", &l->intensity, 0.05f, 0.0f, 100.0f);
            }
            ImGui::PopID();
        }
        if (c.n_point == 0) ImGui::TextDisabled("(none)");
        ImGui::TreePop();
    }

    /* Spot */
    std::snprintf(lbl, sizeof(lbl), "%s (%d)",
                  jce_editor_i18n("lighting.spot"), c.n_spot);
    if (ImGui::TreeNodeEx(lbl)) {
        for (int i = 0; i < c.n_spot; i++) {
            uint32_t id = c.spot[i];
            ImGui::PushID((int)id + 0x20000);
            JceSpotLight *l = jce_scene_get_spot_light(scene, (JceEntity)id);
            const char *name = jce_scene_entity_name(scene, (JceEntity)id);
            if (ImGui::SmallButton("\xe2\x86\x92")) ping_entity(id);
            ImGui::SameLine();
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.light"));
            if (l) {
                float col[3] = { l->color.x, l->color.y, l->color.z };
                if (ImGui::ColorEdit3("##col", col)) {
                    l->color.x = col[0]; l->color.y = col[1]; l->color.z = col[2];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::DragFloat("##i", &l->intensity, 0.05f, 0.0f, 100.0f);
            }
            ImGui::PopID();
        }
        if (c.n_spot == 0) ImGui::TextDisabled("(none)");
        ImGui::TreePop();
    }
}

bool draw_fog(void)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.fog"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    bool changed = false;
    const char *items[4] = {
        jce_editor_i18n("panel.lighting.fog.mode.none"),
        jce_editor_i18n("panel.lighting.fog.mode.linear"),
        jce_editor_i18n("panel.lighting.fog.mode.exp"),
        jce_editor_i18n("panel.lighting.fog.mode.exp2"),
    };
    if (ImGui::Combo(jce_editor_i18n("panel.lighting.fog.mode"),
                     &g_lit.fog_mode, items, 4)) {
        g_lit.fog_enabled = (g_lit.fog_mode != FOG_NONE);
        changed = true;
    }

    ImGui::BeginDisabled(g_lit.fog_mode == FOG_NONE);
    changed |= ImGui::ColorEdit3(jce_editor_i18n("panel.lighting.fog.color"),
                                 g_lit.fog_color);
    if (g_lit.fog_mode == FOG_LINEAR) {
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.fog.start"),
            &g_lit.fog_start, 0.5f, 0.0f, 1000.0f, "%.1f");
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.fog.end"),
            &g_lit.fog_end, 0.5f, 0.0f, 5000.0f, "%.1f");
    } else if (g_lit.fog_mode == FOG_EXP || g_lit.fog_mode == FOG_EXP2) {
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.fog.density"),
            &g_lit.fog_density, 0.001f, 0.0f, 1.0f, "%.4f");
    }
    /* Volumetric fog tuning (consumed by renderer accessor below). */
    changed |= ImGui::DragFloat(jce_editor_i18n("lighting.fog.heightFalloff"),
                                &g_lit.fog_height_falloff, 0.001f, 0.0f, 1.0f, "%.4f");
    changed |= ImGui::DragFloat(jce_editor_i18n("lighting.fog.heightOrigin"),
                                &g_lit.fog_height_origin, 0.1f);
    ImGui::EndDisabled();
    ImGui::TextDisabled("(%s)", jce_editor_i18n("lighting.fog.note"));
    return changed;
}

/* ── Time of Day tab (merged from jce_panel_time_of_day.cpp in P6-A.2) ─ */

JceTimeOfDayConfig g_tod_cfg     = jce_time_of_day_default_config();
float              g_tod_hour    = 12.0f;
bool               g_tod_auto    = false;
bool               g_tod_advance = false;
float              g_tod_rate    = 0.5f; /* hours per second */
double             g_tod_last_t  = 0.0;

void draw_time_of_day_tab(void)
{
    double now = ImGui::GetTime();
    if (g_tod_advance && g_tod_last_t > 0.0) {
        g_tod_hour += (float)(now - g_tod_last_t) * g_tod_rate;
        while (g_tod_hour >= 24.0f) g_tod_hour -= 24.0f;
        while (g_tod_hour <  0.0f)  g_tod_hour += 24.0f;
    }
    g_tod_last_t = now;

    ImGui::SliderFloat(jce_editor_i18n("timeOfDay.hourOfDay"),
                       &g_tod_hour, 0.0f, 24.0f, "%.2f h");
    ImGui::Checkbox(jce_editor_i18n("timeOfDay.autoAdvance"), &g_tod_advance);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat(jce_editor_i18n("timeOfDay.rate"),
                     &g_tod_rate, 0.05f, 0.0f, 24.0f, "%.2f");

    ImGui::Separator();
    if (ImGui::CollapsingHeader(jce_editor_i18n("timeOfDay.configuration"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat(jce_editor_i18n("timeOfDay.dawnHour"),
                         &g_tod_cfg.dawn_hour,        0.05f, 0.0f, 24.0f, "%.2f");
        ImGui::DragFloat(jce_editor_i18n("timeOfDay.duskHour"),
                         &g_tod_cfg.dusk_hour,        0.05f, 0.0f, 24.0f, "%.2f");
        ImGui::DragFloat(jce_editor_i18n("timeOfDay.latitude"),
                         &g_tod_cfg.latitude_degrees, 0.5f, -90.0f, 90.0f, "%.1f");
    }

    JceTimeOfDayState st;
    jce_time_of_day_evaluate(&g_tod_cfg, g_tod_hour, &st);

    ImGui::Separator();
    ImGui::Text("%s   : %+.2f %+.2f %+.2f", jce_editor_i18n("timeOfDay.sunDir"),
                st.sun_direction.x, st.sun_direction.y, st.sun_direction.z);
    ImGui::Text("%s : %.2f %.2f %.2f  (%s %.2f)",
                jce_editor_i18n("timeOfDay.sunColor"),
                st.sun_color.x, st.sun_color.y, st.sun_color.z,
                jce_editor_i18n("timeOfDay.intensity"), st.sun_intensity);
    ImGui::Text("%s   : %.2f %.2f %.2f", jce_editor_i18n("timeOfDay.ambient"),
                st.ambient_color.x, st.ambient_color.y, st.ambient_color.z);
    ImGui::Text("%s       : %.2f %.2f %.2f  %s %.4f",
                jce_editor_i18n("timeOfDay.fog"),
                st.fog_color.x, st.fog_color.y, st.fog_color.z,
                jce_editor_i18n("timeOfDay.density"), st.fog_density);
    ImGui::Text("%s  : %.2f%s", jce_editor_i18n("timeOfDay.exposure"),
                st.exposure,
                st.is_night ? jce_editor_i18n("timeOfDay.night") : "");

    ImGui::Separator();
    ImGui::Checkbox(jce_editor_i18n("timeOfDay.autoApplyFrame"), &g_tod_auto);
    ImGui::SameLine();
    bool apply = ImGui::Button(jce_editor_i18n("timeOfDay.applyNow"));

    if (apply || g_tod_auto) {
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        if (sr) jce_scene_renderer_set_time_of_day(sr, &st);
    }
}

/* ── Tab focus state (set by sibling shims) ───────────────────────── */
int g_pending_tab = -1; /* 0=settings 1=time_of_day 2=light_explorer */

/* ── Light Explorer tab (merged from jce_panel_light_explorer.cpp in P6-A.3) ─ */

struct LE_Row {
    JceEntity e;
    int       type; /* 0 dir, 1 point, 2 spot */
    jce_vec3  color;
    float     intensity;
    bool      casts_shadow;
    bool      enabled;
    char      name[64];
};

void le_collect_cb(JceScene *s, JceEntity e, void *ud)
{
    auto *rows = (std::vector<LE_Row> *)ud;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    const char *nm = m ? m->name : "(unnamed)";
    bool en = m ? m->enabled : true;

    if (jce_scene_has_dir_light(s, e)) {
        JceDirectionalLight *l = jce_scene_get_dir_light(s, e);
        if (l) {
            LE_Row r{}; r.e = e; r.type = 0; r.color = l->color;
            r.intensity = l->intensity; r.casts_shadow = l->casts_shadow;
            r.enabled = en; std::snprintf(r.name, sizeof(r.name), "%s", nm);
            rows->push_back(r);
        }
    }
    if (jce_scene_has_point_light(s, e)) {
        JcePointLight *l = jce_scene_get_point_light(s, e);
        if (l) {
            LE_Row r{}; r.e = e; r.type = 1; r.color = l->color;
            r.intensity = l->intensity; r.casts_shadow = false;
            r.enabled = en; std::snprintf(r.name, sizeof(r.name), "%s", nm);
            rows->push_back(r);
        }
    }
    if (jce_scene_has_spot_light(s, e)) {
        JceSpotLight *l = jce_scene_get_spot_light(s, e);
        if (l) {
            LE_Row r{}; r.e = e; r.type = 2; r.color = l->color;
            r.intensity = l->intensity; r.casts_shadow = false;
            r.enabled = en; std::snprintf(r.name, sizeof(r.name), "%s", nm);
            rows->push_back(r);
        }
    }
}

void draw_light_explorer_tab(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene")); return; }

    static int filter_type = -1; /* -1 all */
    ImGui::SetNextItemWidth(150);
    ImGui::Combo(jce_editor_i18n("lightExplorer.typeFilter"), &filter_type,
                 "All\0Directional\0Point\0Spot\0");

    std::vector<LE_Row> rows;
    rows.reserve(64);
    jce_scene_each_entity(scene, le_collect_cb, &rows);

    ImGui::Text("%s %zu", jce_editor_i18n("lightExplorer.lightsCount"), rows.size());
    ImGui::Separator();

    if (ImGui::BeginTable("##le_tbl", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Intensity", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Shadows", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        const char *type_names[] = {"Dir", "Point", "Spot"};
        for (auto &r : rows) {
            if (filter_type >= 0 && r.type != filter_type) continue;
            ImGui::TableNextRow();
            ImGui::PushID((int)r.e);

            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(type_names[r.type]);

            ImGui::TableSetColumnIndex(2);
            float col[3] = { r.color.x, r.color.y, r.color.z };
            if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs |
                                              ImGuiColorEditFlags_NoLabel)) {
                jce_vec3 nc{ col[0], col[1], col[2] };
                if (r.type == 0) { JceDirectionalLight *l = jce_scene_get_dir_light(scene, r.e); if (l) l->color = nc; }
                else if (r.type == 1) { JcePointLight *l = jce_scene_get_point_light(scene, r.e); if (l) l->color = nc; }
                else                  { JceSpotLight  *l = jce_scene_get_spot_light(scene, r.e); if (l) l->color = nc; }
            }

            ImGui::TableSetColumnIndex(3);
            float intensity = r.intensity;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##i", &intensity, 0.05f, 0.0f, 100.0f, "%.2f")) {
                if (r.type == 0) { JceDirectionalLight *l = jce_scene_get_dir_light(scene, r.e); if (l) l->intensity = intensity; }
                else if (r.type == 1) { JcePointLight *l = jce_scene_get_point_light(scene, r.e); if (l) l->intensity = intensity; }
                else                  { JceSpotLight  *l = jce_scene_get_spot_light(scene, r.e); if (l) l->intensity = intensity; }
            }

            ImGui::TableSetColumnIndex(4);
            if (r.type == 0) {
                bool s = r.casts_shadow;
                if (ImGui::Checkbox("##sh", &s)) {
                    JceDirectionalLight *l = jce_scene_get_dir_light(scene, r.e);
                    if (l) l->casts_shadow = s;
                }
            } else ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(5);
            bool en = r.enabled;
            if (ImGui::Checkbox("##en", &en))
                jce_state_set_entity_enabled((uint32_t)r.e, en);

            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton(jce_editor_i18n("common.ping")))
                jce_state_select_entity((uint32_t)r.e, false);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} /* namespace */

/* ── Public entry points ──────────────────────────────────────────── */

static void lit_draw_settings_tab(void)
{
    lit_ensure_init();
    JceScene *scene = jce_state_get_scene();
    LightCollect c;
    if (scene) jce_scene_each_entity(scene, collect_cb, &c);

    bool dirty = false;
    dirty |= draw_environment(scene, c);
    dirty |= draw_directional_light();
    dirty |= draw_shadows();
    dirty |= draw_ibl();
    draw_scene_lights(scene, c);
    dirty |= draw_fog();

    /* Cross-cut convenience: surface "Bake All Probes" here so users
     * don't have to open the Reflection Probes panel first. The actual
     * scene-aware queue submission lives in that panel; this is a
     * placeholder hook until a shared engine bake-all entry point lands. */
    ImGui::Separator();
    if (ImGui::Button(jce_editor_i18n("panel.lighting.bake_all_probes"))) {
        /* No-op invocation guard for now (see comment above). */
    }

    if (dirty) lit_save_to_project_settings();
}

/* ──────────────────────────────────────────────────────────────────
 * Rendering Workbench (P7-W3)
 *
 * Lighting Settings hosts Post-FX / Lightmap Bake / Reflection Probes /
 * Render Pipeline as sibling tabs of an outer "Rendering" TabBar.
 * Sibling panels remain registered (JCE_PANEL_POSTFX / _LIGHTMAP_BAKE /
 * _REFLECTION_PROBES / _RENDER_PIPELINE) and route here via
 * jce_panel_lighting_settings_request_tab().  The inner Settings /
 * Time of Day / Light Explorer TabBar continues to live nested inside
 * the outer "Lighting" tab.
 * ────────────────────────────────────────────────────────────────── */

extern "C" void jce_editor_panel_postfx_content(void);
extern "C" void jce_editor_panel_lightmap_bake_content(void);
extern "C" void jce_editor_panel_reflection_probes_content(void);
extern "C" void jce_editor_panel_render_pipeline_content(void);

namespace {

int g_request_outer_tab = -1; /* 0=Lighting 1=PostFX 2=Lightmap 3=ReflectionProbes 4=Pipeline */
int g_current_outer_tab = 0;  /* mirror of active outer TabItem for menu markers */
int g_current_inner_tab = 0;  /* mirror of active inner tab inside Lighting (0=settings 1=tod 2=le) */

static void draw_lighting_tab(void)
{
    if (ImGui::BeginTabBar("##lighting_tabs")) {
        ImGuiTabItemFlags f_set  = (g_pending_tab == 0)
            ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags f_tod  = (g_pending_tab == 1)
            ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags f_le   = (g_pending_tab == 2)
            ? ImGuiTabItemFlags_SetSelected : 0;
        g_pending_tab = -1;
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.lighting.tab.settings"),
                                nullptr, f_set)) {
            g_current_inner_tab = 0;
            lit_draw_settings_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.lighting.tab.time_of_day"),
                                nullptr, f_tod)) {
            g_current_inner_tab = 1;
            draw_time_of_day_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.lighting.tab.light_explorer"),
                                nullptr, f_le)) {
            g_current_inner_tab = 2;
            draw_light_explorer_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} /* anonymous namespace */

extern "C" void jce_panel_lighting_settings_request_tab(int idx)
{
    g_request_outer_tab = idx;
}

extern "C" int jce_panel_lighting_settings_current_tab(void)
{
    return g_current_outer_tab;
}

extern "C" int jce_panel_lighting_settings_current_inner_tab(void)
{
    return g_current_inner_tab;
}

extern "C" void jce_editor_panel_lighting_settings_content(void)
{
    if (!ImGui::BeginTabBar("##rendering_tabs"))
        return;

    ImGuiTabItemFlags f_lit = (g_request_outer_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags f_pfx = (g_request_outer_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags f_lm  = (g_request_outer_tab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags f_rp  = (g_request_outer_tab == 3) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags f_pl  = (g_request_outer_tab == 4) ? ImGuiTabItemFlags_SetSelected : 0;

    char lit_label[96];
    char pfx_label[96];
    char lm_label [96];
    char rp_label [96];
    char pl_label [96];
    std::snprintf(lit_label, sizeof(lit_label), "%s###rw_tab_lighting",
                  jce_editor_i18n("panel.lighting.title"));
    std::snprintf(pfx_label, sizeof(pfx_label), "%s###rw_tab_postfx",
                  jce_editor_i18n("postfx.title"));
    std::snprintf(lm_label,  sizeof(lm_label),  "%s###rw_tab_lightmap",
                  jce_editor_i18n("lightmapBake.title"));
    std::snprintf(rp_label,  sizeof(rp_label),  "%s###rw_tab_reflprobes",
                  jce_editor_i18n("window.reflectionProbes"));
    std::snprintf(pl_label,  sizeof(pl_label),  "%s###rw_tab_pipeline",
                  jce_editor_i18n("panel.render_pipeline.title"));

    if (ImGui::BeginTabItem(lit_label, nullptr, f_lit)) {
        g_current_outer_tab = 0;
        draw_lighting_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(pfx_label, nullptr, f_pfx)) {
        g_current_outer_tab = 1;
        jce_editor_panel_postfx_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(lm_label, nullptr, f_lm)) {
        g_current_outer_tab = 2;
        jce_editor_panel_lightmap_bake_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(rp_label, nullptr, f_rp)) {
        g_current_outer_tab = 3;
        jce_editor_panel_reflection_probes_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(pl_label, nullptr, f_pl)) {
        g_current_outer_tab = 4;
        jce_editor_panel_render_pipeline_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_outer_tab = -1;
}

extern "C" void jce_editor_lighting_settings_focus_tab_time_of_day(void)
{
    g_pending_tab = 1;
}

extern "C" void jce_editor_lighting_settings_focus_tab_light_explorer(void)
{
    g_pending_tab = 2;
}

/* ── Renderer-facing accessors ────────────────────────────────────── */

extern "C" void jce_editor_lighting_settings_get_sun(
    float out_color_rgb[3],
    float *out_intensity,
    float out_dir_xyz[3],
    int *out_cast_shadows)
{
    if (out_color_rgb) {
        out_color_rgb[0] = g_lit.sun_color[0];
        out_color_rgb[1] = g_lit.sun_color[1];
        out_color_rgb[2] = g_lit.sun_color[2];
    }
    if (out_intensity) *out_intensity = g_lit.sun_intensity;
    if (out_dir_xyz) {
        const float yaw   = g_lit.sun_yaw_deg   * (JCE_PI_F / 180.0f);
        const float pitch = g_lit.sun_pitch_deg * (JCE_PI_F / 180.0f);
        const float cp = std::cos(pitch);
        out_dir_xyz[0] = std::sin(yaw)   * cp;
        out_dir_xyz[1] = std::sin(pitch);
        out_dir_xyz[2] = std::cos(yaw)   * cp;
    }
    if (out_cast_shadows)
        *out_cast_shadows = g_lit.sun_cast_shadows ? 1 : 0;
}

extern "C" bool jce_editor_lighting_get_fog_enabled(void)
{
    /* Fog preview gated on the panel being open, so closing it
     * immediately removes fog from the viewport. */
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
    if (!vis || !*vis) return false;
    return g_lit.fog_mode != FOG_NONE;
}

extern "C" void jce_editor_lighting_get_fog_params(JceVolumetricFogParams *out)
{
    if (!out) return;
    *out = jce_volumetric_fog_default_params();
    out->color_r        = g_lit.fog_color[0];
    out->color_g        = g_lit.fog_color[1];
    out->color_b        = g_lit.fog_color[2];
    out->density        = g_lit.fog_density;
    out->height_falloff = g_lit.fog_height_falloff;
    out->height_origin  = g_lit.fog_height_origin;
}

extern "C" void jce_editor_lighting_get_ambient(float out_color_rgb[3], float *out_intensity)
{
    if (out_color_rgb) {
        out_color_rgb[0] = g_lit.ambient_color[0];
        out_color_rgb[1] = g_lit.ambient_color[1];
        out_color_rgb[2] = g_lit.ambient_color[2];
    }
    if (out_intensity) *out_intensity = g_lit.ambient_intensity;
}
