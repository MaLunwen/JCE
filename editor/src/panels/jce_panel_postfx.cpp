/*
 * jce_panel_postfx.cpp  Post-processing effect editor panel.
 *
 * Provides toggle switches and parameter sliders for each PostFX effect
 * in the pipeline.  Reads/writes the engine-owned PostFX pipeline via
 * `jce_scene_renderer_get_postfx`.
 */

#include "ui/jce_editor_colors.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_scene_rendering_defaults.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_scene_renderer.h>
}

/* ── Helper: retrieve the engine-owned PostFX pipeline ────────────── */

static JcePostFXPipeline *get_postfx(void)
{
    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    return sr ? jce_scene_renderer_get_postfx(sr) : NULL;
}

/* ── Scene-owned state bridge ─────────────────────────────────────── */

static JceSceneRenderingSettings *current_rendering_settings_mut(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene)
        return NULL;
    jce_editor_scene_ensure_rendering_settings(scene);
    return jce_scene_get_rendering_settings_mut(scene);
}

static void params_from_settings(const JceSceneRenderingSettings *s,
                                 JcePostFXParams *out)
{
    if (!out)
        return;
    *out = jce_postfx_default_params();
    if (!s)
        return;
    out->exposure            = s->exposure;
    out->gamma               = s->gamma;
    out->bloom_threshold     = s->bloom_threshold;
    out->bloom_intensity     = s->bloom_intensity;
    out->fxaa_span_max       = s->fxaa_span_max;
    out->vignette_intensity  = s->vignette_intensity;
    out->vignette_smoothness = s->vignette_smoothness;
    out->chromatic_strength  = s->chromatic_strength;
}

static void params_to_settings(JceSceneRenderingSettings *s,
                               const JcePostFXParams *params)
{
    if (!s || !params)
        return;
    s->exposure            = params->exposure;
    s->gamma               = params->gamma;
    s->bloom_threshold     = params->bloom_threshold;
    s->bloom_intensity     = params->bloom_intensity;
    s->fxaa_span_max       = params->fxaa_span_max;
    s->vignette_intensity  = params->vignette_intensity;
    s->vignette_smoothness = params->vignette_smoothness;
    s->chromatic_strength  = params->chromatic_strength;
}

static void sync_to_pipeline(const JceSceneRenderingSettings *settings)
{
    JcePostFXPipeline *pfx = get_postfx();
    if (!pfx) return;

    JcePostFXParams params;
    params_from_settings(settings, &params);
    jce_postfx_set_params(pfx, &params);
    for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
        bool enabled = settings &&
            i < JCE_SCENE_RENDERING_POSTFX_COUNT &&
            settings->postfx_enabled[i];
        jce_postfx_enable(pfx, (JcePostFXType)i, enabled);
    }
}

/* Disable all PostFX effects only when there is no active scene to mirror. */
static void disable_pipeline_only(void)
{
    JcePostFXPipeline *pfx = get_postfx();
    if (!pfx) return;
    for (int i = 0; i < JCE_POSTFX_COUNT; i++)
        jce_postfx_enable(pfx, (JcePostFXType)i, false);
}

/* ── Content ──────────────────────────────────────────────────────── */

void jce_editor_panel_postfx_content(void)
{
    JceSceneRenderingSettings *settings = current_rendering_settings_mut();
    if (!settings) {
        ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene"));
        return;
    }

    JcePostFXParams params;
    params_from_settings(settings, &params);
    bool *enabled = settings->postfx_enabled;
    char lbl[128];

    bool changed = false;

    /* Tonemap. */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.tonemapping"), ImGuiTreeNodeFlags_DefaultOpen)) {
        snprintf(lbl, sizeof(lbl), "%s##tonemap", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_TONEMAP]);
        ImGui::BeginDisabled(!enabled[JCE_POSTFX_TONEMAP]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.exposure"), &params.exposure,
                                    0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.gamma"), &params.gamma,
                                    0.01f, 1.0f, 4.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Bloom. */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.bloom"), ImGuiTreeNodeFlags_DefaultOpen)) {
        snprintf(lbl, sizeof(lbl), "%s##bloom", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_BLOOM]);
        ImGui::BeginDisabled(!enabled[JCE_POSTFX_BLOOM]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.threshold"), &params.bloom_threshold,
                                     0.01f, 0.0f, 5.0f);
        snprintf(lbl, sizeof(lbl), "%s##bloom", jce_editor_i18n("light.intensity"));
        changed |= ImGui::DragFloat(lbl, &params.bloom_intensity,
                                     0.01f, 0.0f, 5.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* FXAA. */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.fxaa"), ImGuiTreeNodeFlags_DefaultOpen)) {
        snprintf(lbl, sizeof(lbl), "%s##fxaa", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_FXAA]);
        ImGui::BeginDisabled(!enabled[JCE_POSTFX_FXAA]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.spanMax"), &params.fxaa_span_max,
                                     0.5f, 1.0f, 16.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Vignette. */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.vignette"))) {
        snprintf(lbl, sizeof(lbl), "%s##vignette", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_VIGNETTE]);
        ImGui::BeginDisabled(!enabled[JCE_POSTFX_VIGNETTE]);
        snprintf(lbl, sizeof(lbl), "%s##vig", jce_editor_i18n("light.intensity"));
        changed |= ImGui::DragFloat(lbl, &params.vignette_intensity,
                                     0.01f, 0.0f, 2.0f);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.smoothness"), &params.vignette_smoothness,
                                     0.01f, 0.1f, 5.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Chromatic Aberration. */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.chromaticAberration"))) {
        snprintf(lbl, sizeof(lbl), "%s##chrom", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_CHROMATIC]);
        ImGui::BeginDisabled(!enabled[JCE_POSTFX_CHROMATIC]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.strength"), &params.chromatic_strength,
                                     0.001f, 0.0f, 0.1f, "%.4f");
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Grayscale. */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.grayscale"))) {
        snprintf(lbl, sizeof(lbl), "%s##gray", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_GRAYSCALE]);
    }
    ImGui::PopStyleColor();

    /* Push changes to the engine pipeline. */
    if (changed) {
        params_to_settings(settings, &params);
        sync_to_pipeline(settings);
        jce_state_mark_scene_modified();
    }

    /* Reset button. */
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("postfx.resetAll"))) {
        JceSceneRenderingSettings defaults =
            jce_scene_rendering_settings_default();
        for (int i = 0; i < JCE_POSTFX_COUNT &&
             i < JCE_SCENE_RENDERING_POSTFX_COUNT; i++)
            settings->postfx_enabled[i] = defaults.postfx_enabled[i];
        settings->exposure = defaults.exposure;
        settings->gamma = defaults.gamma;
        settings->bloom_threshold = defaults.bloom_threshold;
        settings->bloom_intensity = defaults.bloom_intensity;
        settings->fxaa_span_max = defaults.fxaa_span_max;
        settings->vignette_intensity = defaults.vignette_intensity;
        settings->vignette_smoothness = defaults.vignette_smoothness;
        settings->chromatic_strength = defaults.chromatic_strength;
        sync_to_pipeline(settings);
        jce_state_mark_scene_modified();
    }
}

/* ── Standalone window wrapper ────────────────────────────────────── */

void jce_editor_panel_postfx_tick(void)
{
    JceSceneRenderingSettings *settings = current_rendering_settings_mut();
    if (settings)
        sync_to_pipeline(settings);
    else
        disable_pipeline_only();
}

void jce_editor_panel_postfx(void)
{
    /* Shim: Post-FX has been merged into the Lighting Settings
     * "Rendering" workbench as a tab.  Activating this panel now
     * redirects to that workbench and requests the Post-FX tab.
     * Symbol kept so menu/hotkey entries registered against
     * JCE_PANEL_POSTFX keep working. */
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX);
    if (!vis || !*vis) return;
    *vis = false;

    bool *ls_vis = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
    if (ls_vis) *ls_vis = true;

    char title[128];
    snprintf(title, sizeof(title), "%s###lighting_settings",
             jce_editor_i18n("panel.lighting.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_lighting_settings_request_tab(1);
}
