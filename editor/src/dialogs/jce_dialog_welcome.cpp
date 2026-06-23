/*
 * jce_dialog_welcome.cpp — Welcome / start screen.
 *
 * Centered modal-style window shown at startup when no project is open.
 * Surfaces:
 *   - New Project        (delegates to s_show_new_project)
 *   - Open Project...    (delegates to s_show_open_project)
 *   - Open Sample        (caged_kingdom inside engine tree, when present)
 *   - Recent projects    (read from JceEditorConfig.recent_projects)
 *
 * Style: minimal — reuses existing dialog flags + i18n keys where
 * possible.  Auto-closes once a project has been opened.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

#include "jce_editor_dialogs_internal.h"
#include "core/jce_editor_project.h"
#include "core/jce_editor_config.h"
#include "panels/jce_panel_preferences.h"

#include <string.h>

/* Forward-declared in jce_editor_dialogs_internal.h: */
extern char s_current_project_root[512];
void set_current_project_root(const char *path);
bool is_valid_project_dir(const char *path);

/* These three flags live in jce_editor_layout.cpp.  We expose tiny
 * setters via the dialog-internal header to avoid leaking globals. */
void jce_editor_layout_request_new_project(void);
void jce_editor_layout_request_open_project(void);

namespace {

struct WelcomeState {
    bool        cfg_loaded   = false;
    JceEditorConfig cfg{};
    char        sample_path[512] = {0};
    bool        sample_checked   = false;
};

WelcomeState s_welcome;

/* Probe a few likely locations for the bundled caged_kingdom sample.
 * Order: cwd/caged_kingdom, ../caged_kingdom (editor exe in build/),
 *        engine_workspace root + /caged_kingdom. */
void detect_sample(void)
{
    if (s_welcome.sample_checked) return;
    s_welcome.sample_checked = true;

    const char *candidates[] = {
        "caged_kingdom",
        "../caged_kingdom",
        "../../caged_kingdom",
        "../../../caged_kingdom",
    };
    for (const char *c : candidates) {
        char manifest[1024];
        jce_path_join(manifest, sizeof(manifest), c, "jce_project.json");
        if (jce_fs_host_exists_file(manifest)) {
            snprintf(s_welcome.sample_path,
                     sizeof(s_welcome.sample_path), "%s", c);
            return;
        }
    }
}

} /* namespace */

void jce_editor_dialog_welcome(bool *p_open)
{
    if (!p_open) return;
    if (!*p_open) {
        s_welcome.cfg_loaded = false;
        return;
    }

    /* Auto-close once any project becomes active. */
    if (s_current_project_root[0] != '\0') {
        *p_open = false;
        return;
    }

    if (!s_welcome.cfg_loaded) {
        jce_editor_config_load(&s_welcome.cfg);
        /* Prune entries whose project dir / manifest is gone on disk so
         * the welcome list stays in sync with reality. */
        if (sanitize_recent_projects(&s_welcome.cfg))
            jce_editor_config_save(&s_welcome.cfg);
        s_welcome.cfg_loaded = true;
        detect_sample();
    }

    /* Centered modal-ish window — kept floating (no docking) but
     * always centered on the viewport on first appearance. */
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 center(vp->Pos.x + vp->Size.x * 0.5f,
                  vp->Pos.y + vp->Size.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);

    char title[128];
    snprintf(title, sizeof(title), "%s###Welcome",
             jce_editor_i18n("welcome.title"));

    if (!ImGui::Begin(title, p_open,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("welcome.brand"));
    ImGui::TextDisabled("%s", jce_editor_i18n("welcome.subtitle"));
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    /* Action row */
    const float bw = 200.0f;
    if (ImGui::Button(jce_editor_i18n("welcome.newProject"), ImVec2(bw, 36))) {
        jce_editor_layout_request_new_project();
        *p_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("welcome.openProject"), ImVec2(bw, 36))) {
        jce_editor_layout_request_open_project();
        *p_open = false;
    }
    if (s_welcome.sample_path[0]) {
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("welcome.openSample"),
                          ImVec2(bw, 36))) {
            set_current_project_root(s_welcome.sample_path);
            JceEditorConfig cfg{};
            jce_editor_config_load(&cfg);
            jce_editor_config_add_recent(&cfg, s_welcome.sample_path);
            jce_editor_config_save(&cfg);
            *p_open = false;
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                       jce_editor_i18n("openProject.recentProjects"));
    ImGui::Spacing();

    ImGui::BeginChild("##WelcomeRecents",
                      ImVec2(0, ImGui::GetContentRegionAvail().y - 40),
                      ImGuiChildFlags_Borders);
    if (s_welcome.cfg.recent_count == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("openProject.noRecent"));
    } else {
        int remove_idx = -1;
        for (int i = 0; i < s_welcome.cfg.recent_count; i++) {
            const char *p = s_welcome.cfg.recent_projects[i];
            bool exists = is_valid_project_dir(p);
            ImGui::PushID(i);

            char label[600];
            if (exists) {
                snprintf(label, sizeof(label), "%s", p);
            } else {
                snprintf(label, sizeof(label), "%s %s", p,
                         jce_editor_i18n("menu.file.recentMissing"));
            }

            if (!exists) ImGui::BeginDisabled(true);
            if (ImGui::Selectable(label, false,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(ImGui::GetContentRegionAvail().x - 30, 0))) {
                if (exists && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    set_current_project_root(p);
                    jce_editor_config_add_recent(&s_welcome.cfg, p);
                    jce_editor_config_save(&s_welcome.cfg);
                    *p_open = false;
                }
            }
            if (!exists) ImGui::EndDisabled();

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
            if (ImGui::SmallButton("X")) {
                remove_idx = i;
            }
            ImGui::PopID();
        }
        if (remove_idx >= 0) {
            for (int j = remove_idx; j < s_welcome.cfg.recent_count - 1; j++) {
                snprintf(s_welcome.cfg.recent_projects[j],
                         sizeof(s_welcome.cfg.recent_projects[j]),
                         "%s", s_welcome.cfg.recent_projects[j + 1]);
            }
            s_welcome.cfg.recent_count--;
            if (s_welcome.cfg.recent_count >= 0 && s_welcome.cfg.recent_count < 10)
                s_welcome.cfg.recent_projects[s_welcome.cfg.recent_count][0] = '\0';
            jce_editor_config_save(&s_welcome.cfg);
        }
    }
    ImGui::EndChild();

    bool show_on_startup =
        jce_editor_prefs_startup_behavior() == JCE_EDITOR_STARTUP_PICKER;
    if (ImGui::Checkbox(jce_editor_i18n("welcome.showOnStartup"),
                        &show_on_startup)) {
        jce_editor_prefs_set_startup_behavior(
            show_on_startup ? JCE_EDITOR_STARTUP_PICKER
                            : JCE_EDITOR_STARTUP_EMPTY);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("welcome.hint"));

    /* Official user guide — right-aligned in the footer; opens the guide
     * panel (and closes the welcome screen so the reader lands in it). */
    {
        const char *guide_lbl = jce_editor_i18n("menu.help.guide");
        float w = ImGui::CalcTextSize(guide_lbl).x
                + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
        if (ImGui::Button(guide_lbl)) {
            *jce_editor_panel_visible_ptr(JCE_PANEL_USER_GUIDE) = true;
            jce_editor_panel_request_focus("###user_guide");
            *p_open = false;
        }
    }

    ImGui::End();
}
