/*
 * jce_panel_lighting_settings.cpp  Unity-parity Lighting Settings window.
 *
 * Single, consolidated editor surface for scene-wide lighting. Mirrors
 * Unity's "Window > Rendering > Lighting" panel. Replaces the older
 * `jce_panel_lighting.cpp` (merged in P5-A.4 — skybox component editing,
 * scene-light enumeration, and renderer-facing fog/ambient accessors all
 * moved here).
 *
 * Per-component (Light / Sun / Probe) inspection still lives in
 * `jce_panel_inspector_lighting.cpp` — this panel is strictly scene-level.
 *
 * Sections (collapsing headers, all default-open):
 *   1. Environment           Sky type / HDRI / sky+ambient intensity /
 *                            reflection probe slot, plus the per-entity
 *                            Skybox component editor and scene ambient.
 *   2. Directional Light     Sun colour, intensity, yaw/pitch direction,
 *                            shadow toggle.
 *   3. Shadows (CSM)         Distance, cascade count, split lambda,
 *                            shadow-map resolution, soft filter.
 *   4. Image-Based Lighting  Skybox-driven auto-convolve status note,
 *                            SH band readout, specular mip count readout.
 *                            (Convolution itself is engine-driven: the
 *                            scene renderer re-bakes asynchronously when
 *                            the Skybox HDR path changes, disk-cached.)
 *   5. Fog                   Unity-style mode picker (None/Linear/
 *                            Exp/Exp²) feeding the volumetric-fog
 *                            renderer params (height-falloff aware).
 *
 * Ambient, fog, shadows, and PostFX are owned by the current scene's
 * JceSceneRenderingSettings. Project settings seed new/legacy scenes only.
 */

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_scene_rendering_defaults.h"
#include "core/jce_editor_state.h"
#include "dialogs/jce_path_input.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_volumetric_fog.h>
}

#ifndef JCE_PI_F
#define JCE_PI_F 3.14159265358979323846f
#endif

namespace {

/* ── Editor-side authoritative state ───────────────────────────────── */

enum SkyType { SKY_NONE = 0, SKY_PROCEDURAL = 1, SKY_HDRI = 2 };

struct JceLightingSettings {
    /* Environment (in-session) */
    int   sky_type            = SKY_PROCEDURAL;
    float sky_intensity       = 1.0f;
    char  reflection_probe[260] = {0};

    /* Directional Light (Sun, in-session) */
    float sun_color[3]        = { 1.0f, 0.96f, 0.9f };
    float sun_intensity       = 3.0f;
    float sun_yaw_deg         = 35.0f;
    float sun_pitch_deg       = -55.0f;
    bool  sun_cast_shadows    = true;

    /* IBL (in-session readouts) */
    int   ibl_sh_bands        = 3;
    int   ibl_spec_mips       = 5;
};

JceLightingSettings g_lit;
bool g_lit_initialized = false;

/* ── Session-only lighting workbench state ───────────────────────── */

void lit_ensure_init(void)
{
    if (g_lit_initialized) return;
    g_lit_initialized = true;
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

float clamp_float(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

JceSceneRenderingSettings *scene_rendering_settings_mut(JceScene *scene)
{
    if (!scene)
        return nullptr;
    jce_editor_scene_ensure_rendering_settings(scene);
    return jce_scene_get_rendering_settings_mut(scene);
}

const JceSceneRenderingSettings *current_scene_rendering_settings(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene)
        return nullptr;
    jce_editor_scene_ensure_rendering_settings(scene);
    return jce_scene_get_rendering_settings(scene);
}

JceDirectionalLight *first_directional_light(JceScene *scene,
                                             const LightCollect &c)
{
    if (!scene || c.n_dir <= 0)
        return nullptr;
    return jce_scene_get_dir_light(scene, (JceEntity)c.dir[0]);
}

void direction_to_angles(const jce_vec3 &dir, float *yaw_deg, float *pitch_deg)
{
    if (!yaw_deg || !pitch_deg)
        return;
    *yaw_deg = std::atan2(dir.x, dir.z) * (180.0f / JCE_PI_F);
    *pitch_deg = std::asin(clamp_float(dir.y, -1.0f, 1.0f)) *
        (180.0f / JCE_PI_F);
}

jce_vec3 angles_to_direction(float yaw_deg, float pitch_deg)
{
    const float yaw = yaw_deg * (JCE_PI_F / 180.0f);
    const float pitch = pitch_deg * (JCE_PI_F / 180.0f);
    const float cp = std::cos(pitch);
    return jce_v3(std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp);
}

/* ── Section helpers ──────────────────────────────────────────────── */

bool draw_environment(JceScene *scene, const LightCollect &c,
                      JceSceneRenderingSettings *rendering)
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

    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.env.sky_intensity"),
        &g_lit.sky_intensity, 0.0f, 4.0f, "%.2f");

    ImGui::BeginDisabled(!rendering);
    if (rendering) {
        changed |= ImGui::SliderFloat(
            jce_editor_i18n("panel.lighting.env.ambient_intensity"),
            &rendering->ambient_intensity, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::ColorEdit3(jce_editor_i18n("lighting.ambientColor"),
                                     rendering->ambient_color);

        /* ── Sky pass (gradient / equirect / analytic Preetham) ──────── */
        const char *sky_modes[3] = {
            jce_editor_i18n("panel.lighting.env.sky_mode.gradient"),
            jce_editor_i18n("panel.lighting.env.sky_mode.equirect"),
            jce_editor_i18n("panel.lighting.env.sky_mode.preetham"),
        };
        int sky_mode = rendering->sky_mode;
        if (sky_mode < JCE_SCENE_SKY_GRADIENT) sky_mode = JCE_SCENE_SKY_GRADIENT;
        if (sky_mode > JCE_SCENE_SKY_PREETHAM) sky_mode = JCE_SCENE_SKY_PREETHAM;
        if (ImGui::Combo(jce_editor_i18n("panel.lighting.env.sky_mode"),
                         &sky_mode, sky_modes, 3)) {
            rendering->sky_mode = sky_mode;
            changed = true;
        }
        if (rendering->sky_mode == JCE_SCENE_SKY_PREETHAM) {
            changed |= ImGui::DragFloat(
                jce_editor_i18n("panel.lighting.env.turbidity"),
                &rendering->sky_turbidity, 0.05f, 1.0f, 10.0f, "%.2f");
        }
    }
    ImGui::EndDisabled();

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

    /* ── Floating origin (large-world float32 precision; opt-in) ──────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("panel.lighting.env.floating_origin"));
    ImGui::BeginDisabled(!rendering);
    if (rendering) {
        if (ImGui::Checkbox(
                jce_editor_i18n("panel.lighting.env.floating_origin.enabled"),
                &rendering->floating_origin_enabled))
            changed = true;
        ImGui::BeginDisabled(!rendering->floating_origin_enabled);
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.env.floating_origin.threshold"),
            &rendering->floating_origin_threshold, 16.0f, 256.0f, 65536.0f,
            "%.0f m");
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s",
            jce_editor_i18n("panel.lighting.env.floating_origin.hint"));
    }
    ImGui::EndDisabled();

    return changed;
}

bool draw_directional_light(JceScene *scene, const LightCollect &c)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.directional_light"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    JceDirectionalLight *light = first_directional_light(scene, c);
    if (!light) {
        ImGui::TextDisabled("%s", jce_editor_i18n("lighting.noDirectionalLight"));
        return false;
    }

    bool changed = false;
    float color[3] = { light->color.x, light->color.y, light->color.z };
    changed |= ImGui::ColorEdit3(jce_editor_i18n("panel.lighting.sun.color"),
                                 color);
    if (changed) {
        light->color.x = color[0];
        light->color.y = color[1];
        light->color.z = color[2];
    }
    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.sun.intensity"),
        &light->intensity, 0.0f, 10.0f, "%.2f");

    ImGui::TextDisabled("%s",
        jce_editor_i18n("panel.lighting.sun.direction"));
    JceTransform *xf = (scene && c.n_dir > 0)
        ? jce_scene_get_transform(scene, (JceEntity)c.dir[0])
        : nullptr;
    jce_vec3 world_dir = light->direction;
    if (xf)
        world_dir = jce_q_rotate(jce_q_normalize(xf->rotation), world_dir);
    world_dir = jce_v3_normalize(world_dir);
    float yaw_deg = 0.0f;
    float pitch_deg = -55.0f;
    direction_to_angles(world_dir, &yaw_deg, &pitch_deg);
    bool yaw_changed = ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.sun.yaw"),
        &yaw_deg, -180.0f, 180.0f, "%.1f\xc2\xb0");
    bool pitch_changed = ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.sun.pitch"),
        &pitch_deg, -90.0f, 90.0f, "%.1f\xc2\xb0");
    if (yaw_changed || pitch_changed) {
        if (xf)
            xf->rotation = jce_q_identity();
        light->direction = angles_to_direction(yaw_deg, pitch_deg);
        changed = true;
    }

    changed |= ImGui::Checkbox(
        jce_editor_i18n("panel.lighting.sun.cast_shadows"),
        &light->casts_shadow);
    return changed;
}

bool draw_shadows(JceSceneRenderingSettings *rendering)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.shadows"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    ImGui::BeginDisabled(!rendering);
    bool changed = false;
    if (!rendering) {
        ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene"));
        ImGui::EndDisabled();
        return false;
    }

    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.shadows.distance"),
        &rendering->shadow_distance, 10.0f, 500.0f, "%.0f m");

    const char *cc_items[4] = { "1", "2", "3", "4" };
    int cc_idx = rendering->cascade_count - 1;
    if (cc_idx < 0) cc_idx = 0;
    if (cc_idx > 3) cc_idx = 3;
    if (ImGui::Combo(jce_editor_i18n("panel.lighting.shadows.cascade_count"),
                     &cc_idx, cc_items, 4)) {
        rendering->cascade_count = cc_idx + 1;
        changed = true;
    }

    changed |= ImGui::SliderFloat(
        jce_editor_i18n("panel.lighting.shadows.split_lambda"),
        &rendering->split_lambda, 0.0f, 1.0f, "%.2f");

    const char *res_labels[4] = { "512", "1024", "2048", "4096" };
    const int   res_values[4] = { 512, 1024, 2048, 4096 };
    int res_idx = 2;
    for (int i = 0; i < 4; i++)
        if (res_values[i] == rendering->shadow_resolution) { res_idx = i; break; }
    if (ImGui::Combo(jce_editor_i18n("panel.lighting.shadows.resolution"),
                     &res_idx, res_labels, 4)) {
        rendering->shadow_resolution = res_values[res_idx];
        changed = true;
    }

    const char *soft_items[3] = {
        jce_editor_i18n("panel.lighting.shadows.soft.off"),
        jce_editor_i18n("panel.lighting.shadows.soft.pcf"),
        jce_editor_i18n("panel.lighting.shadows.soft.vsm"),
    };
    changed |= ImGui::Combo(jce_editor_i18n("panel.lighting.shadows.soft"),
                            &rendering->soft_shadow_mode, soft_items, 3);
    ImGui::EndDisabled();
    return changed;
}

/* Image-Based Lighting — informational only.  Convolution is driven by
 * the engine: when the scene's Skybox HDR path changes, the scene
 * renderer (sr_scan_skybox → sr_ibl_start_async) re-convolves on a
 * worker thread with a disk bake-cache.  There is no public force-rebake
 * hook, so this section surfaces the driving path and how to (re)trigger
 * it instead of offering a button. */
void draw_ibl(JceScene *scene, const LightCollect &c)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.ibl"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const JceSkyboxComponent *sky =
        (scene && c.has_skybox)
            ? jce_scene_get_skybox(scene, (JceEntity)c.skybox)
            : nullptr;

    if (sky && sky->hdr_path[0] != '\0') {
        /* Read-only view of the path that actually drives IBL (edit it in
         * the Environment section above or on the Skybox component). */
        ImGui::LabelText(jce_editor_i18n("skybox.hdrPath"), "%s",
                         sky->hdr_path);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", jce_editor_i18n("lighting.ibl.autoNote"));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", jce_editor_i18n("lighting.ibl.noSkybox"));
        ImGui::PopTextWrapPos();
    }

    ImGui::Text("%s: L0..L%d (%d coeffs)",
                jce_editor_i18n("panel.lighting.ibl.sh_bands"),
                g_lit.ibl_sh_bands - 1,
                g_lit.ibl_sh_bands * g_lit.ibl_sh_bands);
    ImGui::Text("%s: %d",
                jce_editor_i18n("panel.lighting.ibl.spec_mips"),
                g_lit.ibl_spec_mips);
}

bool draw_fog(JceSceneRenderingSettings *rendering)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.fog"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    ImGui::BeginDisabled(!rendering);
    bool changed = false;
    if (!rendering) {
        ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene"));
        ImGui::EndDisabled();
        return false;
    }

    const char *items[4] = {
        jce_editor_i18n("panel.lighting.fog.mode.none"),
        jce_editor_i18n("panel.lighting.fog.mode.linear"),
        jce_editor_i18n("panel.lighting.fog.mode.exp"),
        jce_editor_i18n("panel.lighting.fog.mode.exp2"),
    };
    if (ImGui::Combo(jce_editor_i18n("panel.lighting.fog.mode"),
                     &rendering->fog_mode, items, 4)) {
        rendering->fog_enabled = (rendering->fog_mode != JCE_SCENE_FOG_NONE);
        changed = true;
    }

    ImGui::BeginDisabled(rendering->fog_mode == JCE_SCENE_FOG_NONE);
    changed |= ImGui::ColorEdit3(jce_editor_i18n("panel.lighting.fog.color"),
                                 rendering->fog_color);
    if (rendering->fog_mode == JCE_SCENE_FOG_LINEAR) {
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.fog.start"),
            &rendering->fog_start, 0.5f, 0.0f, 1000.0f, "%.1f");
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.fog.end"),
            &rendering->fog_end, 0.5f, 0.0f, 5000.0f, "%.1f");
    } else if (rendering->fog_mode == JCE_SCENE_FOG_EXP ||
               rendering->fog_mode == JCE_SCENE_FOG_EXP2) {
        changed |= ImGui::DragFloat(
            jce_editor_i18n("panel.lighting.fog.density"),
            &rendering->fog_density, 0.001f, 0.0f, 1.0f, "%.4f");
    }
    /* Volumetric fog tuning (consumed by renderer accessor below). */
    changed |= ImGui::DragFloat(jce_editor_i18n("lighting.fog.heightFalloff"),
                                &rendering->fog_height_falloff, 0.001f, 0.0f, 1.0f, "%.4f");
    changed |= ImGui::DragFloat(jce_editor_i18n("lighting.fog.heightOrigin"),
                                &rendering->fog_height_origin, 0.1f);
    ImGui::EndDisabled();
    ImGui::TextDisabled("(%s)", jce_editor_i18n("lighting.fog.note"));
    ImGui::EndDisabled();
    return changed;
}

bool advance_time_of_day(JceSceneRenderingSettings *rendering);

/* ── Weather + serialized Time-of-Day (P2-weather-decals-tod) ──────────
 *
 * Authors the scene-level JceSceneRenderingSettings weather + time-of-day
 * fields.  These settings serialize with the scene and are driven every
 * frame by the scene renderer (sr_drive_time_of_day / sr_drive_weather)
 * in both the editor preview and the shipping runtime. */
bool draw_weather_and_tod(JceSceneRenderingSettings *rendering)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n("panel.lighting.section.weather"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return false;

    if (!rendering) {
        ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene"));
        return false;
    }

    bool changed = advance_time_of_day(rendering);

    /* Serialized time-of-day. */
    changed |= ImGui::Checkbox(jce_editor_i18n("panel.lighting.tod.enabled"),
                               &rendering->tod_enabled);
    ImGui::BeginDisabled(!rendering->tod_enabled);
    ImGui::BeginDisabled(rendering->tod_speed > 0.0f);
    changed |= ImGui::SliderFloat(jce_editor_i18n("panel.lighting.tod.hour"),
                                  &rendering->tod_hour, 0.0f, 24.0f, "%.2f h");
    ImGui::EndDisabled();
    changed |= ImGui::DragFloat(jce_editor_i18n("panel.lighting.tod.speed"),
                                &rendering->tod_speed, 0.05f, 0.0f, 240.0f, "%.2f");
    changed |= ImGui::DragFloat(jce_editor_i18n("panel.lighting.tod.latitude"),
                                &rendering->tod_latitude, 0.5f, -90.0f, 90.0f, "%.1f");
    changed |= ImGui::DragFloat(jce_editor_i18n("panel.lighting.tod.dawn"),
                                &rendering->tod_dawn_hour, 0.05f, 0.0f, 24.0f, "%.2f");
    changed |= ImGui::DragFloat(jce_editor_i18n("panel.lighting.tod.dusk"),
                                &rendering->tod_dusk_hour, 0.05f, 0.0f, 24.0f, "%.2f");
    ImGui::EndDisabled();

    ImGui::Separator();

    /* Weather. */
    const char *wx_items[3] = {
        jce_editor_i18n("panel.lighting.weather.clear"),
        jce_editor_i18n("panel.lighting.weather.rain"),
        jce_editor_i18n("panel.lighting.weather.snow"),
    };
    changed |= ImGui::Combo(jce_editor_i18n("panel.lighting.weather.type"),
                            &rendering->weather_type, wx_items, 3);
    ImGui::BeginDisabled(rendering->weather_type == 0);
    changed |= ImGui::SliderFloat(jce_editor_i18n("panel.lighting.weather.intensity"),
                                  &rendering->weather_intensity, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();

    return changed;
}

bool advance_time_of_day(JceSceneRenderingSettings *rendering)
{
    if (!rendering || !rendering->tod_enabled || rendering->tod_speed <= 0.0f)
        return false;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f)
        return false;
    if (dt > 0.25f)
        dt = 0.25f;

    rendering->tod_hour += rendering->tod_speed * dt;
    while (rendering->tod_hour >= 24.0f)
        rendering->tod_hour -= 24.0f;
    while (rendering->tod_hour < 0.0f)
        rendering->tod_hour += 24.0f;
    return true;
}

/* ── Time of Day tab (merged from jce_panel_time_of_day.cpp in P6-A.2) ─ */

void draw_time_of_day_tab(void)
{
    JceScene *scene = jce_state_get_scene();
    JceSceneRenderingSettings *rendering =
        scene_rendering_settings_mut(scene);
    if (!rendering) {
        ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene"));
        return;
    }

    bool changed = draw_weather_and_tod(rendering);

    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    cfg.dawn_hour        = rendering->tod_dawn_hour;
    cfg.dusk_hour        = rendering->tod_dusk_hour;
    cfg.latitude_degrees = rendering->tod_latitude;
    JceTimeOfDayState st;
    jce_time_of_day_evaluate(&cfg, rendering->tod_hour, &st);

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

    if (changed)
        jce_state_mark_scene_modified();
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
    static const char *const kLeTypeKeys[] = {
        "lightExplorer.type.all", "lightExplorer.type.directional",
        "lightExplorer.type.point", "lightExplorer.type.spot" };
    ImGui::Combo(jce_editor_i18n("lightExplorer.typeFilter"), &filter_type,
                 jce_editor_i18n_combo(kLeTypeKeys, 4));

    std::vector<LE_Row> rows;
    rows.reserve(64);
    jce_scene_each_entity(scene, le_collect_cb, &rows);

    ImGui::Text("%s %zu", jce_editor_i18n("lightExplorer.lightsCount"), rows.size());
    ImGui::Separator();

    if (ImGui::BeginTable("##le_tbl", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lighting.col.name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lighting.col.type"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lighting.col.color"), ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lighting.col.intensity"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lighting.col.shadows"), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lighting.col.enabled"), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        const char *type_names[] = { jce_editor_i18n("lightExplorer.type.dir"), jce_editor_i18n("lightExplorer.type.point"), jce_editor_i18n("lightExplorer.type.spot") };
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
    JceSceneRenderingSettings *rendering =
        scene_rendering_settings_mut(scene);
    LightCollect c;
    if (scene) jce_scene_each_entity(scene, collect_cb, &c);

    bool dirty = false;
    dirty |= draw_environment(scene, c, rendering);
    dirty |= draw_directional_light(scene, c);
    dirty |= draw_shadows(rendering);
    draw_ibl(scene, c);
    dirty |= draw_fog(rendering);

    /* Cross-cut convenience: surface "Bake All Probes" here so users
     * don't have to open the Reflection Probes panel first. The actual
     * scene-aware queue submission lives in that panel; this is a
     * placeholder hook until a shared engine bake-all entry point lands. */
    ImGui::Separator();
    if (ImGui::Button(jce_editor_i18n("panel.lighting.bake_all_probes"))) {
        /* No-op invocation guard for now (see comment above). */
    }

    if (dirty) jce_state_mark_scene_modified();
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
bool g_outer_tab_state_loaded = false;
bool g_inner_tab_state_loaded = false;

static const char *k_outer_tab_state_key = "panel.rendering.current_tab";
static const char *k_inner_tab_state_key = "panel.lighting.current_tab";

static bool valid_outer_tab(int idx)
{
    return idx >= 0 && idx <= 4;
}

static bool valid_inner_tab(int idx)
{
    return idx >= 0 && idx <= 2;
}

static void ensure_outer_tab_state_loaded(void)
{
    if (g_outer_tab_state_loaded)
        return;
    g_current_outer_tab =
        jce_editor_ui_state_load_int(k_outer_tab_state_key, 0, 0, 4);
    g_request_outer_tab = g_current_outer_tab;
    g_outer_tab_state_loaded = true;
}

static void ensure_inner_tab_state_loaded(void)
{
    if (g_inner_tab_state_loaded)
        return;
    g_current_inner_tab =
        jce_editor_ui_state_load_int(k_inner_tab_state_key, 0, 0, 2);
    g_pending_tab = g_current_inner_tab;
    g_inner_tab_state_loaded = true;
}

static void set_outer_tab(int idx)
{
    if (!valid_outer_tab(idx) || g_current_outer_tab == idx)
        return;
    g_current_outer_tab = idx;
    if (g_outer_tab_state_loaded)
        jce_editor_ui_state_save_int(k_outer_tab_state_key, idx);
}

static void set_inner_tab(int idx)
{
    if (!valid_inner_tab(idx) || g_current_inner_tab == idx)
        return;
    g_current_inner_tab = idx;
    if (g_inner_tab_state_loaded)
        jce_editor_ui_state_save_int(k_inner_tab_state_key, idx);
}

static void draw_lighting_tab(void)
{
    ensure_inner_tab_state_loaded();
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
            set_inner_tab(0);
            lit_draw_settings_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.lighting.tab.time_of_day"),
                                nullptr, f_tod)) {
            set_inner_tab(1);
            draw_time_of_day_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.lighting.tab.light_explorer"),
                                nullptr, f_le)) {
            set_inner_tab(2);
            draw_light_explorer_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} /* anonymous namespace */

extern "C" void jce_panel_lighting_settings_request_tab(int idx)
{
    if (!valid_outer_tab(idx))
        return;
    g_request_outer_tab = idx;
    g_current_outer_tab = idx;
    jce_editor_ui_state_save_int(k_outer_tab_state_key, idx);
}

extern "C" int jce_panel_lighting_settings_current_tab(void)
{
    ensure_outer_tab_state_loaded();
    return g_current_outer_tab;
}

extern "C" int jce_panel_lighting_settings_current_inner_tab(void)
{
    ensure_inner_tab_state_loaded();
    return g_current_inner_tab;
}

extern "C" void jce_editor_panel_lighting_settings_content(void)
{
    ensure_outer_tab_state_loaded();
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
        set_outer_tab(0);
        draw_lighting_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(pfx_label, nullptr, f_pfx)) {
        set_outer_tab(1);
        jce_editor_panel_postfx_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(lm_label, nullptr, f_lm)) {
        set_outer_tab(2);
        jce_editor_panel_lightmap_bake_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(rp_label, nullptr, f_rp)) {
        set_outer_tab(3);
        jce_editor_panel_reflection_probes_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(pl_label, nullptr, f_pl)) {
        set_outer_tab(4);
        jce_editor_panel_render_pipeline_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_outer_tab = -1;
}

extern "C" void jce_editor_lighting_settings_focus_tab_time_of_day(void)
{
    g_pending_tab = 1;
    g_current_inner_tab = 1;
    jce_editor_ui_state_save_int(k_inner_tab_state_key, 1);
}

extern "C" void jce_editor_lighting_settings_focus_tab_light_explorer(void)
{
    g_pending_tab = 2;
    g_current_inner_tab = 2;
    jce_editor_ui_state_save_int(k_inner_tab_state_key, 2);
}

/* ── Renderer-facing accessors ────────────────────────────────────── */

extern "C" void jce_editor_lighting_settings_get_sun(
    float out_color_rgb[3],
    float *out_intensity,
    float out_dir_xyz[3],
    int *out_cast_shadows)
{
    JceScene *scene = jce_state_get_scene();
    LightCollect c;
    if (scene) jce_scene_each_entity(scene, collect_cb, &c);
    JceDirectionalLight *light = first_directional_light(scene, c);

    if (light) {
        jce_vec3 world_dir = light->direction;
        if (scene && c.n_dir > 0) {
            JceTransform *xf =
                jce_scene_get_transform(scene, (JceEntity)c.dir[0]);
            if (xf) {
                world_dir =
                    jce_q_rotate(jce_q_normalize(xf->rotation), world_dir);
            }
        }
        world_dir = jce_v3_normalize(world_dir);
        if (out_color_rgb) {
            out_color_rgb[0] = light->color.x;
            out_color_rgb[1] = light->color.y;
            out_color_rgb[2] = light->color.z;
        }
        if (out_intensity) *out_intensity = light->intensity;
        if (out_dir_xyz) {
            out_dir_xyz[0] = world_dir.x;
            out_dir_xyz[1] = world_dir.y;
            out_dir_xyz[2] = world_dir.z;
        }
        if (out_cast_shadows)
            *out_cast_shadows = light->casts_shadow ? 1 : 0;
        return;
    }

    if (out_color_rgb) {
        out_color_rgb[0] = g_lit.sun_color[0];
        out_color_rgb[1] = g_lit.sun_color[1];
        out_color_rgb[2] = g_lit.sun_color[2];
    }
    if (out_intensity) *out_intensity = g_lit.sun_intensity;
    if (out_dir_xyz) {
        jce_vec3 dir = angles_to_direction(g_lit.sun_yaw_deg,
                                           g_lit.sun_pitch_deg);
        out_dir_xyz[0] = dir.x;
        out_dir_xyz[1] = dir.y;
        out_dir_xyz[2] = dir.z;
    }
    if (out_cast_shadows)
        *out_cast_shadows = g_lit.sun_cast_shadows ? 1 : 0;
}

extern "C" bool jce_editor_lighting_get_fog_enabled(void)
{
    const JceSceneRenderingSettings *r = current_scene_rendering_settings();
    return r && r->fog_enabled && r->fog_mode != JCE_SCENE_FOG_NONE;
}

extern "C" void jce_editor_lighting_get_fog_params(JceVolumetricFogParams *out)
{
    if (!out) return;
    const JceSceneRenderingSettings *r = current_scene_rendering_settings();
    *out = jce_volumetric_fog_default_params();
    if (!r)
        return;
    out->color_r        = r->fog_color[0];
    out->color_g        = r->fog_color[1];
    out->color_b        = r->fog_color[2];
    out->density        = r->fog_density;
    out->height_falloff = r->fog_height_falloff;
    out->height_origin  = r->fog_height_origin;
}

extern "C" void jce_editor_lighting_get_ambient(float out_color_rgb[3], float *out_intensity)
{
    const JceSceneRenderingSettings *r = current_scene_rendering_settings();
    if (out_color_rgb) {
        out_color_rgb[0] = r ? r->ambient_color[0] : 0.1f;
        out_color_rgb[1] = r ? r->ambient_color[1] : 0.1f;
        out_color_rgb[2] = r ? r->ambient_color[2] : 0.12f;
    }
    if (out_intensity) *out_intensity = r ? r->ambient_intensity : 1.0f;
}
