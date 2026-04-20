/*
 * jce_panel_postfx.cpp  Post-processing effect editor panel.
 *
 * Provides toggle switches and parameter sliders for each PostFX effect
 * in the pipeline.  Reads/writes the engine-owned PostFX pipeline via
 * `jce_scene_renderer_get_postfx`.
 */

#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "scene/jce_editor_scene_render.h"

#include <imgui.h>
#include <cstdio>

extern "C" {
#include <jce/graphics/jce_postfx.h>
#include <jce/render/jce_scene_renderer.h>
}

/* ── Helper: retrieve the engine-owned PostFX pipeline ────────────── */

static JcePostFXPipeline *get_postfx(void)
{
    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    return sr ? jce_scene_renderer_get_postfx(sr) : NULL;
}

/* ── Panel state ──────────────────────────────────────────────────── */

static struct {
    JcePostFXParams params;
    bool            enabled[JCE_POSTFX_COUNT];
    bool            initialized;
} s_pfx;

static void ensure_init(void)
{
    if (s_pfx.initialized) return;
    JcePostFXPipeline *pfx = get_postfx();
    if (pfx) {
        jce_postfx_get_params(pfx, &s_pfx.params);
        for (int i = 0; i < JCE_POSTFX_COUNT; i++)
            s_pfx.enabled[i] = jce_postfx_is_enabled(pfx,
                                                     (JcePostFXType)i);
    } else {
        s_pfx.params = jce_postfx_default_params();
        for (int i = 0; i < JCE_POSTFX_COUNT; i++)
            s_pfx.enabled[i] = false;
    }
    s_pfx.initialized = true;
}

static void sync_to_pipeline(void)
{
    JcePostFXPipeline *pfx = get_postfx();
    if (!pfx) return;
    jce_postfx_set_params(pfx, &s_pfx.params);
    for (int i = 0; i < JCE_POSTFX_COUNT; i++)
        jce_postfx_enable(pfx, (JcePostFXType)i, s_pfx.enabled[i]);
}

/* ── Content ──────────────────────────────────────────────────────── */

void jce_editor_panel_postfx_content(void)
{
    ensure_init();
    char lbl[128];

    bool changed = false;

    /* Tonemap. */
    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.tonemapping"), ImGuiTreeNodeFlags_DefaultOpen)) {
        snprintf(lbl, sizeof(lbl), "%s##tonemap", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &s_pfx.enabled[JCE_POSTFX_TONEMAP]);
        ImGui::BeginDisabled(!s_pfx.enabled[JCE_POSTFX_TONEMAP]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.exposure"), &s_pfx.params.exposure,
                                    0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.gamma"), &s_pfx.params.gamma,
                                    0.01f, 1.0f, 4.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Bloom. */
    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.bloom"), ImGuiTreeNodeFlags_DefaultOpen)) {
        snprintf(lbl, sizeof(lbl), "%s##bloom", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &s_pfx.enabled[JCE_POSTFX_BLOOM]);
        ImGui::BeginDisabled(!s_pfx.enabled[JCE_POSTFX_BLOOM]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.threshold"), &s_pfx.params.bloom_threshold,
                                     0.01f, 0.0f, 5.0f);
        snprintf(lbl, sizeof(lbl), "%s##bloom", jce_editor_i18n("light.intensity"));
        changed |= ImGui::DragFloat(lbl, &s_pfx.params.bloom_intensity,
                                     0.01f, 0.0f, 5.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* FXAA. */
    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.fxaa"), ImGuiTreeNodeFlags_DefaultOpen)) {
        snprintf(lbl, sizeof(lbl), "%s##fxaa", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &s_pfx.enabled[JCE_POSTFX_FXAA]);
        ImGui::BeginDisabled(!s_pfx.enabled[JCE_POSTFX_FXAA]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.spanMax"), &s_pfx.params.fxaa_span_max,
                                     0.5f, 1.0f, 16.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Vignette. */
    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.vignette"))) {
        snprintf(lbl, sizeof(lbl), "%s##vignette", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &s_pfx.enabled[JCE_POSTFX_VIGNETTE]);
        ImGui::BeginDisabled(!s_pfx.enabled[JCE_POSTFX_VIGNETTE]);
        snprintf(lbl, sizeof(lbl), "%s##vig", jce_editor_i18n("light.intensity"));
        changed |= ImGui::DragFloat(lbl, &s_pfx.params.vignette_intensity,
                                     0.01f, 0.0f, 2.0f);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.smoothness"), &s_pfx.params.vignette_smoothness,
                                     0.01f, 0.1f, 5.0f);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Chromatic Aberration. */
    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.chromaticAberration"))) {
        snprintf(lbl, sizeof(lbl), "%s##chrom", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &s_pfx.enabled[JCE_POSTFX_CHROMATIC]);
        ImGui::BeginDisabled(!s_pfx.enabled[JCE_POSTFX_CHROMATIC]);
        changed |= ImGui::DragFloat(jce_editor_i18n("postfx.strength"), &s_pfx.params.chromatic_strength,
                                     0.001f, 0.0f, 0.1f, "%.4f");
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();

    /* Grayscale. */
    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    if (ImGui::CollapsingHeader(jce_editor_i18n("postfx.grayscale"))) {
        snprintf(lbl, sizeof(lbl), "%s##gray", jce_editor_i18n("postfx.enable"));
        changed |= ImGui::Checkbox(lbl, &s_pfx.enabled[JCE_POSTFX_GRAYSCALE]);
    }
    ImGui::PopStyleColor();

    /* Push changes to the engine pipeline. */
    if (changed)
        sync_to_pipeline();

    /* Reset button. */
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("postfx.resetAll"))) {
        s_pfx.params = jce_postfx_default_params();
        for (int i = 0; i < JCE_POSTFX_COUNT; i++)
            s_pfx.enabled[i] = false;
        sync_to_pipeline();
    }
}

/* ── Standalone window wrapper ────────────────────────────────────── */

void jce_editor_panel_postfx(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###PostFX", jce_editor_i18n("postfx.title"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_postfx_content();
    ImGui::End();
}
