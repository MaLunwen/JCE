/*
 * jce_fv_render_pipeline.cpp  Render Pipeline Asset (.rp.json) viewer.
 *
 * Inspector for jce_render_pipeline.h descriptors.  Mirrors
 * jce_fv_physmat.cpp's edit-and-save pattern: parse on first show,
 * track per-tab modified state, write back on the Save button.
 *
 * Sections: Features, Quality, Targets, Presets.
 */

#include "jce_fv_common.h"

#include <string>
#include <vector>

extern "C" {
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>
}

#include "io/jce_editor_file_util.h"

#define LOG_TAG "fv_rp"

struct RpViewState {
    bool                  parsed;
    bool                  load_ok;
    bool                  modified;
    JceRenderPipelineDesc desc;
};

static std::vector<std::pair<std::string, RpViewState>> s_rp_states;

static RpViewState *fv_get_rp_state(FvTab *tab)
{
    for (auto &pr : s_rp_states)
        if (pr.first == tab->path) return &pr.second;
    s_rp_states.push_back(std::make_pair(std::string(tab->path),
                                          RpViewState()));
    return &s_rp_states.back().second;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static int shadow_res_to_idx(uint16_t r)
{
    switch (r) {
    case 512:  return 0;
    case 1024: return 1;
    case 2048: return 2;
    case 4096: return 3;
    default:   return 1;
    }
}

static uint16_t shadow_idx_to_res(int i)
{
    static const uint16_t v[] = {512, 1024, 2048, 4096};
    if (i < 0) i = 0;
    if (i > 3) i = 3;
    return v[i];
}

static int msaa_to_idx(uint8_t s)
{
    switch (s) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: return 0;
    }
}

static uint8_t msaa_idx_to_samples(int i)
{
    static const uint8_t v[] = {1, 2, 4, 8};
    if (i < 0) i = 0;
    if (i > 3) i = 3;
    return v[i];
}

static void save_now(FvTab *tab, RpViewState *ms)
{
    if (!jce_render_pipeline_save(tab->path, &ms->desc)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Failed to save render pipeline: %s", tab->path);
        return;
    }
    ms->modified  = false;
    tab->modified = false;
    jce_editor_console_log("Saved render pipeline: %s", tab->path);

    /* Live-apply the new descriptor so a developer toggling features
     * in the viewer sees the engine react immediately. */
    jce_render_pipeline_apply(&ms->desc);

    size_t got = 0, total = 0;
    char *buf = (char *)ed_read_file_capped(tab->path, FV_MAX_CONTENT,
                                            &got, &total);
    if (buf) {
        ED_FREE(tab->content);
        tab->content     = buf;
        tab->content_len = (int)got;
        tab->file_size   = (long)total;
    }
}

static bool toggle_row(const char *i18n_key, bool *v)
{
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n(i18n_key));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    bool changed = ImGui::Checkbox((std::string("##") + i18n_key).c_str(), v);
    ImGui::NextColumn();
    return changed;
}

/* ── Main render ──────────────────────────────────────────────────── */

void fv_render_render_pipeline(FvTab *tab)
{
    RpViewState *ms = fv_get_rp_state(tab);

    if (!ms->parsed) {
        ms->parsed    = true;
        ms->load_ok   = false;
        ms->modified  = false;
        tab->modified = false;
        jce_render_pipeline_preset_for_current_tier(&ms->desc);
        if (tab->path[0] != '\0')
            ms->load_ok = jce_render_pipeline_load(tab->path, &ms->desc);
    }

    /* Toolbar */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("asset.rp.title"));
    ImGui::SameLine();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "  %s%s  |  %.1f KB",
        tab->display_name,
        ms->modified ? " *" : "",
        (double)tab->file_size / 1024.0);

    if (ms->modified && ms->load_ok) {
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("dialog.save")))
            save_now(tab, ms);
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("dialog.cancel"))) {
            ms->parsed    = false;
            ms->modified  = false;
            tab->modified = false;
        }
    }

    ImGui::Separator();

    if (!ms->load_ok) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
            "%s", jce_editor_i18n("viewer.materialParseFailed"));
        ImGui::Spacing();
        ImGui::Separator();
        fv_render_code(tab);
        return;
    }

    JceRenderPipelineDesc &d = ms->desc;

    /* ── Features ─────────────────────────────────────────────── */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("viewer.rp.section.features"));
    ImGui::Columns(2, "##rpfeat", false);
    ImGui::SetColumnWidth(0, 220);
    if (toggle_row("viewer.rp.feature.csm",           &d.enable_csm))            ms->modified = true;
    if (toggle_row("viewer.rp.feature.ssao",          &d.enable_ssao))           ms->modified = true;
    if (toggle_row("viewer.rp.feature.ssr",           &d.enable_ssr))            ms->modified = true;
    if (toggle_row("viewer.rp.feature.taa",           &d.enable_taa))            ms->modified = true;
    if (toggle_row("viewer.rp.feature.bloom",         &d.enable_bloom))          ms->modified = true;
    if (toggle_row("viewer.rp.feature.volfog",        &d.enable_volumetric_fog)) ms->modified = true;
    if (toggle_row("viewer.rp.feature.gpu_particles", &d.enable_gpu_particles))  ms->modified = true;
    if (toggle_row("viewer.rp.feature.motion_blur",   &d.enable_motion_blur))    ms->modified = true;
    if (toggle_row("viewer.rp.feature.cloth",         &d.enable_cloth))          ms->modified = true;
    ImGui::Columns(1);
    ImGui::Spacing();

    /* ── Quality ──────────────────────────────────────────────── */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("viewer.rp.section.quality"));
    ImGui::Columns(2, "##rpqual", false);
    ImGui::SetColumnWidth(0, 220);

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("viewer.rp.shadow_res"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    {
        static const char *items[] = {"512", "1024", "2048", "4096"};
        int idx = shadow_res_to_idx(d.shadow_resolution);
        if (ImGui::Combo("##sres", &idx, items, 4)) {
            d.shadow_resolution = shadow_idx_to_res(idx);
            ms->modified = true;
        }
    }
    ImGui::NextColumn();

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("viewer.rp.cascades"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    {
        int c = (int)d.csm_cascade_count;
        if (ImGui::SliderInt("##cc", &c, 1, 4)) {
            d.csm_cascade_count = (uint8_t)c;
            ms->modified = true;
        }
    }
    ImGui::NextColumn();

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("viewer.rp.msaa"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    {
        static const char *items[] = {"1x", "2x", "4x", "8x"};
        int idx = msaa_to_idx(d.msaa_samples);
        if (ImGui::Combo("##msaa", &idx, items, 4)) {
            d.msaa_samples = msaa_idx_to_samples(idx);
            ms->modified = true;
        }
    }
    ImGui::NextColumn();

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("viewer.rp.render_scale"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##rs", &d.render_scale, 0.25f, 2.0f, "%.2f"))
        ms->modified = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) ms->modified = true;
    ImGui::NextColumn();

    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("viewer.rp.post_quality"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    {
        static const char *items[] = {"Low", "Mid", "High", "Ultra"};
        int idx = (int)d.post_quality;
        if (ImGui::Combo("##pq", &idx, items, 4)) {
            d.post_quality = (JceRpQuality)idx;
            ms->modified = true;
        }
    }
    ImGui::NextColumn();
    ImGui::Columns(1);
    ImGui::Spacing();

    /* ── Targets ──────────────────────────────────────────────── */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("viewer.rp.section.targets"));
    ImGui::Columns(2, "##rptgt", false);
    ImGui::SetColumnWidth(0, 220);
    if (toggle_row("viewer.rp.hdr_color",     &d.hdr_color))     ms->modified = true;
    if (toggle_row("viewer.rp.depth_prepass", &d.depth_prepass)) ms->modified = true;
    ImGui::Columns(1);
    ImGui::Spacing();

    /* ── Presets ──────────────────────────────────────────────── */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("viewer.rp.section.presets"));
    if (ImGui::Button(jce_editor_i18n("viewer.rp.preset.low"))) {
        jce_render_pipeline_preset_low(&d);
        ms->modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("viewer.rp.preset.mid"))) {
        jce_render_pipeline_preset_mid(&d);
        ms->modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("viewer.rp.preset.high"))) {
        jce_render_pipeline_preset_high(&d);
        ms->modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("viewer.rp.preset.ultra"))) {
        jce_render_pipeline_preset_ultra(&d);
        ms->modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("viewer.rp.reset_tier_default"))) {
        jce_render_pipeline_preset_for_current_tier(&d);
        ms->modified = true;
    }

    if (ms->modified)
        tab->modified = true;
}
