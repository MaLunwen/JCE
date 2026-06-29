/*
 * jce_editor_scene_rendering_defaults.cpp
 *
 * Project settings provide defaults for newly-created scenes. Loaded scenes
 * own their rendering state and must not be overwritten by project defaults.
 */

#include "jce_editor_scene_rendering_defaults.h"
#include "jce_project_settings.h"
#include "jce_editor_project.h"
#include <jce/renderer/jce_render_settings.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <cstdio>

static int quality_shadow_resolution_pixels(int value)
{
    static const int k_res[] = { 512, 1024, 2048, 4096 };
    if (value < 0 || value >= (int)(sizeof(k_res) / sizeof(k_res[0])))
        return 2048;
    return k_res[value];
}

static const JceProjectQualityLevel *current_quality_level(
    const JceProjectSettings *ps)
{
    if (!ps || ps->quality.count <= 0)
        return nullptr;
    int idx = ps->quality.current_level;
    if (idx < 0 || idx >= ps->quality.count)
        idx = ps->quality.count - 1;
    if (idx < 0 || idx >= JCE_PS_MAX_QUALITY_LEVELS)
        return nullptr;
    return &ps->quality.levels[idx];
}

void jce_editor_scene_rendering_settings_from_project(
    JceSceneRenderingSettings *out)
{
    if (!out)
        return;

    *out = jce_scene_rendering_settings_default();

    const JceProjectSettings *ps = jce_project_settings_current();
    JceProjectSettings defaults;
    if (!ps) {
        jce_project_settings_defaults(&defaults);
        ps = &defaults;
    }

    out->ambient_color[0] = ps->rendering.ambient_color[0];
    out->ambient_color[1] = ps->rendering.ambient_color[1];
    out->ambient_color[2] = ps->rendering.ambient_color[2];
    out->ambient_intensity = ps->rendering.ambient_intensity;

    out->fog_enabled = ps->rendering.fog_enabled;
    out->fog_mode = ps->rendering.fog_enabled
        ? JCE_SCENE_FOG_EXP
        : JCE_SCENE_FOG_NONE;
    out->fog_color[0] = ps->rendering.fog_color[0];
    out->fog_color[1] = ps->rendering.fog_color[1];
    out->fog_color[2] = ps->rendering.fog_color[2];
    out->fog_density = ps->rendering.fog_density;
    out->fog_height_falloff = ps->rendering.fog_height_falloff;
    out->fog_height_origin = ps->rendering.fog_height_origin;

    for (int i = 0; i < JCE_SCENE_RENDERING_POSTFX_COUNT && i < 6; i++)
        out->postfx_enabled[i] = ps->rendering.postfx_enabled[i];
    out->exposure = ps->rendering.exposure;
    out->gamma = ps->rendering.gamma;
    out->bloom_threshold = ps->rendering.bloom_threshold;
    out->bloom_intensity = ps->rendering.bloom_intensity;
    out->fxaa_span_max = ps->rendering.fxaa_span_max;
    out->vignette_intensity = ps->rendering.vignette_intensity;
    out->vignette_smoothness = ps->rendering.vignette_smoothness;
    out->chromatic_strength = ps->rendering.chromatic_strength;

    const JceProjectQualityLevel *q = current_quality_level(ps);
    if (q) {
        out->shadow_distance = q->shadow_distance;
        out->cascade_count = q->shadow_cascades;
        out->shadow_resolution =
            quality_shadow_resolution_pixels(q->shadow_resolution);
        out->soft_shadow_mode = (q->shadow_quality >= 2)
            ? JCE_SCENE_SOFT_SHADOW_PCF
            : JCE_SCENE_SOFT_SHADOW_OFF;
    }

    /* Look Profile project defaults (plan 02) — field-by-field copy.
     * MISSING a line here = the look field is silently dropped for new
     * scenes. ps->rendering does NOT carry these; the project-wide source
     * is jce_render_settings.json, so seed from jce_render_settings_default
     * unless the project provides them. */
    JceRenderSettings rs = jce_render_settings_default();
    /* Resolve render_settings.json RELATIVE TO THE OPEN PROJECT ROOT (NOT the
     * editor exe base dir — that returns the editor install, not the project, so
     * the load always failed and project look/grass defaults were never picked
     * up).  Try source-authored copy first, then cooked. */
    {
        bool rs_loaded = false;
        const JceProject *proj = jce_editor_project_get();
        if (proj && proj->project_root && proj->project_root[0]) {
            const char *src = (proj->source_assets && proj->source_assets[0])
                              ? proj->source_assets : "resources/assets";
            const char *cooked = (proj->cooked_assets && proj->cooked_assets[0])
                                 ? proj->cooked_assets : "resources/_cooked";
            char rpath[1024];
            int n = snprintf(rpath, sizeof(rpath), "%s/%s/render_settings.json",
                             proj->project_root, src);
            if (n > 0 && n < (int)sizeof(rpath))
                rs_loaded = jce_render_settings_load_json(rpath, &rs);
            if (!rs_loaded) {
                n = snprintf(rpath, sizeof(rpath), "%s/%s/render_settings.json",
                             proj->project_root, cooked);
                if (n > 0 && n < (int)sizeof(rpath))
                    rs_loaded = jce_render_settings_load_json(rpath, &rs);
            }
        }
        if (!rs_loaded)
            LOG_WARN("scene_defaults", "render_settings.json not found under project root — look profile defaults");
    }
    out->wrap_factor        = rs.wrap_factor;
    out->ambient_hemisphere = rs.ambient_hemisphere;
    out->ambient_ground_color[0] = rs.ambient_ground_color[0];
    out->ambient_ground_color[1] = rs.ambient_ground_color[1];
    out->ambient_ground_color[2] = rs.ambient_ground_color[2];
    out->rim_color[0] = rs.rim_color[0];
    out->rim_color[1] = rs.rim_color[1];
    out->rim_color[2] = rs.rim_color[2];
    out->rim_power     = rs.rim_power;
    out->rim_intensity = rs.rim_intensity;
    out->tonemap_op    = rs.tonemap_op;
    {
        size_t i = 0;
        for (; rs.lut_path[i] && i + 1 < sizeof(out->lut_path); i++)
            out->lut_path[i] = rs.lut_path[i];
        out->lut_path[i] = '\0';
    }
    out->lut_strength   = rs.lut_strength;
    out->toon_character = rs.toon_character;
    out->bloom_knee     = rs.bloom_knee;
}

void jce_editor_scene_ensure_rendering_settings(JceScene *scene)
{
    if (!scene || jce_scene_has_rendering_settings(scene))
        return;

    JceSceneRenderingSettings settings;
    jce_editor_scene_rendering_settings_from_project(&settings);
    jce_scene_set_rendering_settings(scene, &settings);
}
