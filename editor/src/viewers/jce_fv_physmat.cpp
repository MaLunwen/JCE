/*
 * jce_fv_physmat.cpp  Physics Material (.physmat.json) sub-viewer.
 *
 * Editable widgets for dynamic/static friction, restitution, and the two
 * combine modes.  Mirrors jce_fv_material.cpp's per-tab-state pattern.
 */

#include "jce_fv_common.h"

#include <string>
#include <vector>

extern "C" {
#include <jce/middleware/physics/jce_physics_material.h>
}

#include "io/jce_editor_file_util.h"

#define LOG_TAG "fv_physmat"

struct PhysMatViewState {
    bool               parsed;
    bool               load_ok;
    bool               modified;
    JcePhysicsMaterial mat;
};

static std::vector<std::pair<std::string, PhysMatViewState>> s_pm_states;

static PhysMatViewState *fv_get_pm_state(FvTab *tab)
{
    for (auto &pr : s_pm_states)
        if (pr.first == tab->path) return &pr.second;
    s_pm_states.push_back(std::make_pair(std::string(tab->path),
                                          PhysMatViewState()));
    return &s_pm_states.back().second;
}

static void check_edit(PhysMatViewState *ms)
{
    if (ImGui::IsItemDeactivatedAfterEdit())
        ms->modified = true;
}

static int combine_to_idx(JcePhysicsCombine c)
{
    switch (c) {
    case JCE_PHYS_COMBINE_MIN:      return 1;
    case JCE_PHYS_COMBINE_MAX:      return 2;
    case JCE_PHYS_COMBINE_MULTIPLY: return 3;
    case JCE_PHYS_COMBINE_AVERAGE:
    default:                        return 0;
    }
}

static JcePhysicsCombine idx_to_combine(int i)
{
    switch (i) {
    case 1:  return JCE_PHYS_COMBINE_MIN;
    case 2:  return JCE_PHYS_COMBINE_MAX;
    case 3:  return JCE_PHYS_COMBINE_MULTIPLY;
    default: return JCE_PHYS_COMBINE_AVERAGE;
    }
}

void fv_render_physmat(FvTab *tab)
{
    PhysMatViewState *ms = fv_get_pm_state(tab);

    /* ── Parse on first access ──────────────────────────────────── */
    if (!ms->parsed) {
        ms->parsed   = true;
        ms->load_ok  = false;
        ms->modified = false;
        tab->modified = false;
        jce_physics_material_init_default(&ms->mat);
        if (tab->path[0] != '\0')
            ms->load_ok = jce_physics_material_load(tab->path, &ms->mat);
    }

    /* ── Toolbar ────────────────────────────────────────────────── */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
        jce_editor_i18n("asset.physmat.title"));
    ImGui::SameLine();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "  %s%s  |  %.1f KB",
        tab->display_name,
        ms->modified ? " *" : "",
        (double)tab->file_size / 1024.0);

    if (ms->modified && ms->load_ok) {
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("dialog.save"))) {
            if (jce_physics_material_save(tab->path, &ms->mat)) {
                ms->modified  = false;
                tab->modified = false;
                jce_editor_console_log("Saved physics material: %s", tab->path);
                size_t got = 0, total = 0;
                char *buf = (char *)ed_read_file_capped(tab->path,
                                                        FV_MAX_CONTENT,
                                                        &got, &total);
                if (buf) {
                    ED_FREE(tab->content);
                    tab->content     = buf;
                    tab->content_len = (int)got;
                    tab->file_size   = (long)total;
                }
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Failed to save physics material: %s", tab->path);
            }
        }
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

    /* ── Editable properties ────────────────────────────────────── */
    JcePhysicsMaterial &m = ms->mat;

    const char *combine_items[] = {
        jce_editor_i18n("enum.physmat.combine.average"),
        jce_editor_i18n("enum.physmat.combine.min"),
        jce_editor_i18n("enum.physmat.combine.max"),
        jce_editor_i18n("enum.physmat.combine.multiply"),
    };

    ImGui::Columns(2, "##pmprops", false);
    ImGui::SetColumnWidth(0, 180);

    /* Dynamic friction */
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("inspector.physmat.dynamic_friction"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##df", &m.dynamic_friction, 0.01f, 0.0f, 1.0f);
    check_edit(ms);
    ImGui::NextColumn();

    /* Static friction */
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("inspector.physmat.static_friction"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##sf", &m.static_friction, 0.01f, 0.0f, 1.0f);
    check_edit(ms);
    ImGui::NextColumn();

    /* Restitution */
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("inspector.physmat.restitution"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##re", &m.restitution, 0.01f, 0.0f, 1.0f);
    check_edit(ms);
    ImGui::NextColumn();

    /* Friction combine */
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("inspector.physmat.friction_combine"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    {
        int fc = combine_to_idx(m.friction_combine);
        if (ImGui::Combo("##fc", &fc, combine_items, 4)) {
            m.friction_combine = idx_to_combine(fc);
            ms->modified = true;
        }
    }
    ImGui::NextColumn();

    /* Restitution combine */
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
        jce_editor_i18n("inspector.physmat.restitution_combine"));
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    {
        int rc = combine_to_idx(m.restitution_combine);
        if (ImGui::Combo("##rc", &rc, combine_items, 4)) {
            m.restitution_combine = idx_to_combine(rc);
            ms->modified = true;
        }
    }
    ImGui::NextColumn();

    ImGui::Columns(1);
    if (ms->modified)
        tab->modified = true;
}
