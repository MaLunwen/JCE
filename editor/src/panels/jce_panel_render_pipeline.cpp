/*
 * jce_panel_render_pipeline.cpp  Render Pipeline Asset editor panel.
 *
 * Exposes every JceRenderPipelineDesc field as checkboxes / sliders and
 * provides Save / Load buttons so artists can author .rp.json assets without
 * touching the C API directly.
 *
 * Pattern: mirrors jce_panel_postfx.cpp — extern "C" for all C includes,
 * static state, ensure_init(), _content() for embedding in the docked window,
 * _tick() for any per-frame housekeeping (currently just guards init).
 */

#include "ui/jce_editor_colors.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_texture.h>
}

/* ── Panel state ──────────────────────────────────────────────────── */

static struct {
    JceRenderPipelineDesc desc;
    char                  asset_path[512];
    bool                  initialized;
    char                  status_msg[128]; /* "" = no message */
} s_rp;

static void ensure_init(void)
{
    if (s_rp.initialized) return;
    jce_render_pipeline_get(&s_rp.desc);
    s_rp.asset_path[0] = '\0';
    s_rp.status_msg[0] = '\0';
    s_rp.initialized   = true;
}

/* ── Content ──────────────────────────────────────────────────────── */

void jce_editor_panel_render_pipeline_content(void)
{
    ensure_init();

    /* One-time prefill of the last-used asset path (per-project; the store
     * is inert until a project root is known, so the first draw can be too
     * early to read from it). */
    static bool s_path_prefilled = false;
    if (!s_path_prefilled && jce_editor_pstate_active()) {
        s_path_prefilled = true;
        if (!s_rp.asset_path[0])
            jce_editor_pstate_get_str("rp.asset_path", s_rp.asset_path,
                                      sizeof(s_rp.asset_path));
    }

    char lbl[128];
    bool changed = false;

    /* ── Asset I/O ───────────────────────────────────────────────── */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.render_pipeline.section.asset"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-100.0f);
        ImGui::InputText("##rp_path", s_rp.asset_path, sizeof(s_rp.asset_path));
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("panel.render_pipeline.asset.save"))) {
            if (jce_render_pipeline_save(s_rp.asset_path, &s_rp.desc)) {
                snprintf(s_rp.status_msg, sizeof(s_rp.status_msg),
                         jce_editor_i18n("panel.render_pipeline.status.saved"), s_rp.asset_path);
                jce_editor_pstate_set_str("rp.asset_path", s_rp.asset_path);
            } else {
                snprintf(s_rp.status_msg, sizeof(s_rp.status_msg), "%s", jce_editor_i18n("panel.render_pipeline.status.saveFailed"));
            }
        }
        if (ImGui::Button(jce_editor_i18n("panel.render_pipeline.asset.load"))) {
            JceRenderPipelineDesc loaded;
            if (jce_render_pipeline_load(s_rp.asset_path, &loaded)) {
                s_rp.desc = loaded;
                jce_render_pipeline_apply(&s_rp.desc);
                snprintf(s_rp.status_msg, sizeof(s_rp.status_msg),
                         jce_editor_i18n("panel.render_pipeline.status.loaded"), s_rp.asset_path);
                jce_editor_pstate_set_str("rp.asset_path", s_rp.asset_path);
            } else {
                snprintf(s_rp.status_msg, sizeof(s_rp.status_msg), "%s", jce_editor_i18n("panel.render_pipeline.status.loadFailed"));
            }
        }
        if (s_rp.status_msg[0])
            ImGui::TextDisabled("%s", s_rp.status_msg);
    }
    ImGui::PopStyleColor();

    /* ── Presets ─────────────────────────────────────────────────── */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.render_pipeline.section.presets"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(jce_editor_i18n("panel.render_pipeline.preset.low"))) {
            jce_render_pipeline_preset_low(&s_rp.desc);
            jce_render_pipeline_apply(&s_rp.desc);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("panel.render_pipeline.preset.mid"))) {
            jce_render_pipeline_preset_mid(&s_rp.desc);
            jce_render_pipeline_apply(&s_rp.desc);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("panel.render_pipeline.preset.high"))) {
            jce_render_pipeline_preset_high(&s_rp.desc);
            jce_render_pipeline_apply(&s_rp.desc);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("panel.render_pipeline.preset.ultra"))) {
            jce_render_pipeline_preset_ultra(&s_rp.desc);
            jce_render_pipeline_apply(&s_rp.desc);
            changed = true;
        }
    }
    ImGui::PopStyleColor();

    /* ── Features ────────────────────────────────────────────────── */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.render_pipeline.section.features"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.csm"),
            &s_rp.desc.enable_csm);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.ssao"),
            &s_rp.desc.enable_ssao);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.ssr"),
            &s_rp.desc.enable_ssr);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.taa"),
            &s_rp.desc.enable_taa);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.bloom"),
            &s_rp.desc.enable_bloom);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.volumetric_fog"),
            &s_rp.desc.enable_volumetric_fog);
        {
            /* GPU particles require compute shaders; grey out the toggle on
             * devices without BGFX_CAPS_COMPUTE (the engine would no-op). */
            const bool compute_ok =
                (jce_renderer_get_caps() & JCE_CAP_COMPUTE) != 0;
            if (!compute_ok) ImGui::BeginDisabled();
            changed |= ImGui::Checkbox(
                jce_editor_i18n("panel.render_pipeline.feature.gpu_particles"),
                &s_rp.desc.enable_gpu_particles);
            if (!compute_ok) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", jce_editor_i18n(
                        "panel.render_pipeline.feature.gpu_particles.unsupported"));
            }
        }
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.motion_blur"),
            &s_rp.desc.enable_motion_blur);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.feature.cloth"),
            &s_rp.desc.enable_cloth);
    }
    ImGui::PopStyleColor();

    /* ── Quality ─────────────────────────────────────────────────── */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.render_pipeline.section.quality"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Shadow resolution: 512 / 1024 / 2048 / 4096. */
        static const int   kSrVals[]   = {512, 1024, 2048, 4096};
        static const char *kSrLabels[] = {"512", "1024", "2048", "4096"};
        int cur_sr = 1;
        for (int i = 0; i < 4; i++) {
            if ((int)s_rp.desc.shadow_resolution == kSrVals[i]) { cur_sr = i; break; }
        }
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##rp_shadow_res", &cur_sr, kSrLabels, 4)) {
            s_rp.desc.shadow_resolution = (uint16_t)kSrVals[cur_sr];
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(
            jce_editor_i18n("panel.render_pipeline.quality.shadow_resolution"));

        /* Global texture LOD bias — the "editor quality slider" the texture
         * module anticipates (jce_texture.h).  LIVE control, not part of the
         * .rp.json asset: applies immediately via the mip-streaming system
         * (+1 = drop the top mip everywhere).  The low-memory pressure
         * bridge writes the same global bias on pressure TRANSITIONS, so a
         * manual value can be overridden when memory pressure changes. */
        {
            int bias = (int)jce_texture_get_global_mip_bias();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::SliderInt(
                    jce_editor_i18n("panel.render_pipeline.quality.tex_lod_bias"),
                    &bias, 0, 3)) {
                jce_texture_set_global_mip_bias((int8_t)bias);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", jce_editor_i18n(
                    "panel.render_pipeline.quality.tex_lod_bias.tip"));
        }

        /* CSM cascade count 1–4. */
        int cascades = (int)s_rp.desc.csm_cascade_count;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::SliderInt(
                jce_editor_i18n("panel.render_pipeline.quality.cascade_count"),
                &cascades, 1, 4)) {
            s_rp.desc.csm_cascade_count = (uint8_t)cascades;
            changed = true;
        }

        /* MSAA samples. */
        static const int   kMsVals[]   = {1, 2, 4, 8};
        const char *kMsLabels[] = {jce_editor_i18n("panel.render_pipeline.msaa.off"), "2x", "4x", "8x"};
        int cur_ms = 0;
        for (int i = 0; i < 4; i++) {
            if ((int)s_rp.desc.msaa_samples == kMsVals[i]) { cur_ms = i; break; }
        }
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##rp_msaa", &cur_ms, kMsLabels, 4)) {
            s_rp.desc.msaa_samples = (uint8_t)kMsVals[cur_ms];
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(jce_editor_i18n("panel.render_pipeline.quality.msaa"));

        /* Render scale 0.25–2.0. */
        ImGui::SetNextItemWidth(110.0f);
        changed |= ImGui::SliderFloat(
            jce_editor_i18n("panel.render_pipeline.quality.render_scale"),
            &s_rp.desc.render_scale, 0.25f, 2.0f, "%.2f");

        /* Post quality dropdown. */
        const char *kPqLabels[] = {jce_editor_i18n("panel.render_pipeline.preset.low"), jce_editor_i18n("panel.render_pipeline.preset.mid"), jce_editor_i18n("panel.render_pipeline.preset.high"), jce_editor_i18n("panel.render_pipeline.preset.ultra")};
        int cur_pq = (int)s_rp.desc.post_quality;
        ImGui::SetNextItemWidth(110.0f);
        snprintf(lbl, sizeof(lbl), "##rp_post_quality");
        if (ImGui::Combo(lbl, &cur_pq, kPqLabels, 4)) {
            s_rp.desc.post_quality = (JceRpQuality)cur_pq;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(jce_editor_i18n("panel.render_pipeline.quality.post_quality"));
    }
    ImGui::PopStyleColor();

    /* ── Format ──────────────────────────────────────────────────── */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.render_pipeline.section.format"))) {
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.format.hdr_color"),
            &s_rp.desc.hdr_color);
        changed |= ImGui::Checkbox(
            jce_editor_i18n("panel.render_pipeline.format.depth_prepass"),
            &s_rp.desc.depth_prepass);
    }
    ImGui::PopStyleColor();

    /* ── Performance (settings S3/S4) ────────────────────────────────
     * Each engine perf path is a tri-state: Auto (the tier preset's built-in
     * default), On (force), Off (force).  Serialized into the .rp.json 'perf'
     * object; a shipped game consumes the same field.  Human-readable feature
     * descriptions live here (dev-facing panel; not localized). */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    if (ImGui::CollapsingHeader(
            jce_editor_i18n("panel.render_pipeline.section.performance"))) {
        static const char *kTri[] = { "Auto", "On", "Off" };
        struct PerfRow { JceRpPerfFeature f; const char *label; const char *tip; };
        static const PerfRow kRows[] = {
            { JCE_RP_PERF_PRIM_INSTANCE,   "Primitive instancing",
              "Batch factor-only shape primitives into instanced draws." },
            { JCE_RP_PERF_TEX_INSTANCE,    "Texture-array instancing",
              "Batch same-mesh entities with per-instance albedo textures." },
            { JCE_RP_PERF_DRAWCMD_CACHE,   "Draw-command cache",
              "Persist built draw commands across frames for static entities." },
            { JCE_RP_PERF_PARALLEL_GATHER, "Parallel color gather",
              "Build materials on worker threads (mutually exclusive with the "
              "instancing batchers; helps non-instanceable content)." },
            { JCE_RP_PERF_PARALLEL_SUBMIT, "Parallel submit",
              "Record draw calls across multiple bgfx encoders (multi-core)." },
            { JCE_RP_PERF_HIZ_OCCLUSION,   "Hi-Z occlusion",
              "GPU depth-pyramid occlusion cull (wins only in heavy occlusion)." },
            { JCE_RP_PERF_GPU_SCENE,       "GPU-driven scene",
              "Compute-shader frustum cull + indirect draw for instanced meshes." },
            { JCE_RP_PERF_FOLIAGE_GPU_CULL,"Foliage GPU cull",
              "Compute cull for the vegetation-scatter path (HIGH tier)." },
            { JCE_RP_PERF_CROWD_INSTANCE,  "Crowd instancing",
              "GPU skinning of animated crowds (color/shadow/velocity)." },
        };
        for (const PerfRow &row : kRows) {
            int8_t v = s_rp.desc.perf[row.f];
            int idx = (v < 0) ? 0 : (v ? 1 : 2);   /* auto/on/off */
            char id[64];
            snprintf(id, sizeof(id), "##perf_%s",
                     jce_render_pipeline_perf_name(row.f));
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo(id, &idx, kTri, 3)) {
                s_rp.desc.perf[row.f] =
                    (idx == 0) ? JCE_RP_AUTO : (int8_t)(idx == 1 ? 1 : 0);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(row.label);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", row.tip);
        }
    }
    ImGui::PopStyleColor();

    if (changed)
        jce_render_pipeline_apply(&s_rp.desc);
}

/* ── Tick ─────────────────────────────────────────────────────────── */

void jce_editor_panel_render_pipeline_tick(void)
{
    ensure_init();
}

/* Shim: Render Pipeline has been merged into the Lighting Settings
 * "Rendering" workbench as a tab.  Activating this panel now redirects
 * to that workbench and requests the Pipeline tab.  Symbol kept so
 * menu/hotkey entries registered against JCE_PANEL_RENDER_PIPELINE
 * keep working. */
extern "C" void jce_editor_panel_render_pipeline(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_RENDER_PIPELINE);
    if (!vis || !*vis) return;
    *vis = false;

    bool *ls_vis = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
    if (ls_vis) *ls_vis = true;

    char title[128];
    snprintf(title, sizeof(title), "%s###lighting_settings",
             jce_editor_i18n("panel.lighting.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_lighting_settings_request_tab(4);
}
