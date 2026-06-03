/*
 * jce_editor_scene_rendering_defaults.cpp
 *
 * Project settings provide defaults for newly-created scenes. Loaded scenes
 * own their rendering state and must not be overwritten by project defaults.
 */

#include "jce_editor_scene_rendering_defaults.h"
#include "jce_project_settings.h"

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
}

void jce_editor_scene_ensure_rendering_settings(JceScene *scene)
{
    if (!scene || jce_scene_has_rendering_settings(scene))
        return;

    JceSceneRenderingSettings settings;
    jce_editor_scene_rendering_settings_from_project(&settings);
    jce_scene_set_rendering_settings(scene, &settings);
}
