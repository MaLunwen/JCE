/*
 * jce_panel_postfx.cpp  Post-processing effect editor panel.
 *
 * Provides toggle switches and parameter sliders for each PostFX effect
 * in the pipeline.  Reads/writes the engine-owned PostFX pipeline via
 * `jce_scene_renderer_get_postfx`.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_colors.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_scene_rendering_defaults.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

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
    /* Custom pass (data-driven; the engine is style-agnostic). */
    if (settings) {
        jce_postfx_set_custom_shader(pfx, settings->custom_post_shader,
                                     settings->custom_post_needs_depth);
        jce_postfx_set_custom_params(pfx, settings->custom_post_params,
                                     settings->custom_post_param_count);
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

/* ── Showcase "looks" on the generic custom pass ──────────────────────
 * Editor-side PRESET data (shader name + depth + tuned params) so a one-click
 * dropdown applies each look correctly. This is client/showcase content — the
 * engine custom-pass remains entirely style-agnostic. Add a look = add a row
 * (and the matching engine/shaders/postfx/fs_<name>.sc). */
struct CustomLook { const char *name; bool depth; int count; float p[32]; };
static const CustomLook kLooks[] = {
    { "moebius",    true,  5, { 1.0f,0.22f,2.0f,1.2f,  4.0f,0.55f,0.35f,0.35f, 0.4f,0,0,0, 0.1f,0.09f,0.12f,0, 0.96f,0.92f,0.82f,0 } },
    { "toon",       true,  7, { 1.0f,0.25f,1.5f,0.5f,  3.0f,0.08f,1.35f,1.12f, 0.4f,0.25f,0.25f,0, 0.78f,0.82f,1.0f,0, 1.0f,0.98f,0.9f,0, 0.05f,0.06f,0.09f,0, 1.0f,0.97f,0.85f,0 } },
    { "painterly",  false, 2, { 3.0f,1.3f,1.08f,0.35f, 0.04f,0,0,0 } },
    { "lowpoly",    true,  1, { 5.0f,3.0f,0.35f,1.12f } },
    { "pixel",      false, 1, { 4.0f,6.0f,0.6f,0 } },
    { "psx",        false, 1, { 3.0f,32.0f,0.7f,0.5f } },
    { "watercolor", false, 2, { 2.0f,0.4f,0.06f,0.18f, 0.25f,0,0,0 } },
    { "aquarelle",  false, 3, { 4.0f,8.0f,1.15f,0.4f,  0.05f,0.5f,0.12f,0.2f, 1.0f,1.5f,0,0 } },
    { "comic",      true,  2, { 1.0f,0.28f,5.0f,30.0f, 3.0f,1.35f,0.55f,0 } },
    { "blueprint",  true,  4, { 1.0f,0.22f,22.0f,1.0f,  0.35f,0.45f,5.0f,0, 0.055f,0.16f,0.42f,0, 0.80f,0.90f,1.0f,0 } },
};
static const int kLookCount = (int)(sizeof(kLooks) / sizeof(kLooks[0]));

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
        const char *kTonemapOps[] = { "ACES", "Neutral", "AgX" };
        int top = settings->tonemap_op;
        if (top < 0 || top > 2) top = 0;
        if (ImGui::Combo(jce_editor_i18n("postfx.tonemapOp"), &top,
                         kTonemapOps, 3)) {
            settings->tonemap_op = top;
            changed = true;
        }
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
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.bloomKnee"),
                                    &settings->bloom_knee, 0.01f, 0.0f, 1.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Color Grade (3D-LUT). */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.colorGrade"))) {
        if (ImGui::InputText(jce_editor_i18n("postfx.lutPath"),
                             settings->lut_path, sizeof(settings->lut_path),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            changed = true;
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.lutStrength"),
                                    &settings->lut_strength, 0.01f, 0.0f, 1.0f);
        ImGui::TextDisabled("%s", jce_editor_i18n("postfx.lutHint"));  /* "horizontal NxN strip PNG" */
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

    /* Custom pass — generic, data-driven. The engine knows nothing about what
     * the shader does; this UI just edits a shader name + raw vec4 params.
     * (Literal labels: i18n keys can be added once the i18n tables settle.) */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.customPass"))) {
        snprintf(lbl, sizeof(lbl), "%s##custompass", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &enabled[JCE_POSTFX_CUSTOM]);

        /* One-click look picker — applies the look's name + tuned params live. */
        int look_sel = -1;
        for (int i = 0; i < kLookCount; i++)
            if (strcmp(settings->custom_post_shader, kLooks[i].name) == 0) { look_sel = i; break; }
        if (ImGui::BeginCombo(jce_editor_i18n("postfx.look"), look_sel >= 0 ? kLooks[look_sel].name : "(custom)")) {
            for (int i = 0; i < kLookCount; i++) {
                if (ImGui::Selectable(kLooks[i].name, i == look_sel)) {
                    enabled[JCE_POSTFX_CUSTOM] = true;
                    snprintf(settings->custom_post_shader,
                             sizeof(settings->custom_post_shader), "%s", kLooks[i].name);
                    settings->custom_post_needs_depth = kLooks[i].depth;
                    settings->custom_post_param_count = kLooks[i].count;
                    memcpy(settings->custom_post_params, kLooks[i].p,
                           sizeof(settings->custom_post_params));
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::BeginDisabled(!enabled[JCE_POSTFX_CUSTOM]);
        /* Commit the shader name on Enter to avoid reloading on every keystroke. */
        if (ImGui::InputText(jce_editor_i18n("postfx.shaderLabel"), settings->custom_post_shader,
                             sizeof(settings->custom_post_shader),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            changed = true;
        changed |= ImGui::Checkbox(jce_editor_i18n("postfx.needsDepth"), &settings->custom_post_needs_depth);
        int pc = settings->custom_post_param_count;
        if (ImGui::SliderInt(jce_editor_i18n("postfx.paramCount"), &pc, 0, JCE_POSTFX_CUSTOM_PARAMS)) {
            settings->custom_post_param_count = pc;
            changed = true;
        }
        for (int i = 0; i < settings->custom_post_param_count &&
                        i < JCE_POSTFX_CUSTOM_PARAMS; i++) {
            char plbl[32];
            snprintf(plbl, sizeof(plbl), "Param %d", i);
            changed |= ImGui::DragFloat4(plbl, &settings->custom_post_params[i * 4], 0.01f);
        }
        ImGui::TextDisabled("%s", jce_editor_i18n("postfx.paramHint"));
        ImGui::EndDisabled();
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
        settings->custom_post_shader[0] = '\0';
        settings->custom_post_needs_depth = defaults.custom_post_needs_depth;
        settings->custom_post_param_count = defaults.custom_post_param_count;
        settings->tonemap_op   = defaults.tonemap_op;
        settings->lut_path[0]  = '\0';
        settings->lut_strength = defaults.lut_strength;
        settings->bloom_knee   = defaults.bloom_knee;
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
    if (jce_panel_redirect_to_workbench(JCE_PANEL_POSTFX,
                                        JCE_PANEL_LIGHTING_SETTINGS,
                                        "panel.lighting.title",
                                        "lighting_settings"))
        jce_panel_lighting_settings_request_tab(1);
}
