/*
 * jce_panel_build_profiles.cpp  Unity-style Build Profiles panel.
 *
 * Single surface for build configuration AND build triggering:
 * lists every configurePreset from CMakePresets.json and drives the
 * async jce_build_manager so output streams into the editor Console
 * (with a "[build]" prefix) instead of blocking the UI.
 *
 * On a successful compile we also:
 *   - rewrite editor-config game_executable_path / game_working_directory
 *     so a subsequent Play "just works" (opt-in checkbox, default on);
 *   - flip the Build Report panel visible so the user lands directly
 *     on the post-build summary.
 *
 * The legacy modal jce_dialog_build.cpp is now a thin shim that just
 * opens this panel.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_build_manager.h"
#include "core/jce_editor_config.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_json.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Profile {
    std::string preset;
    std::string display;
    std::string description;
    std::string generator;
};

static struct {
    std::vector<Profile> profiles;
    int                  active = -1;
    bool                 loaded = false;
    bool                 auto_update_run_path = true;
    JceBuildState        prev_state = JCE_BUILD_IDLE;
} s_bp;

static void load_profiles()
{
    s_bp.profiles.clear();
    size_t sz = 0;
    char *buf = (char *)ed_read_file("CMakePresets.json", &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "[Build Profiles] CMakePresets.json not found at project root");
        return;
    }
    JceJson *root = jce_json_parse(buf, (int)sz);
    ED_FREE(buf);
    if (!root) return;
    JceJson *cps = jce_json_get(root, "configurePresets");
    if (jce_json_is_array(cps)) {
        int n = jce_json_array_size(cps);
        for (int i = 0; i < n; ++i) {
            JceJson *p = jce_json_array_at(cps, i);
            Profile pr;
            const char *nm = jce_json_get_string(p, "name", "");
            const char *dn = jce_json_get_string(p, "displayName", "");
            const char *ds = jce_json_get_string(p, "description", "");
            const char *gn = jce_json_get_string(p, "generator", "");
            pr.preset      = nm ? nm : "";
            pr.display     = dn && *dn ? dn : pr.preset;
            pr.description = ds ? ds : "";
            pr.generator   = gn ? gn : "";
            if (!pr.preset.empty() && pr.preset[0] != '_')
                s_bp.profiles.push_back(pr);
        }
    }
    jce_json_free(root);
    s_bp.loaded = true;
    jce_editor_console_log("[Build Profiles] loaded %d preset(s)", (int)s_bp.profiles.size());
}

/* Derive a likely output exe path from a preset name, mirroring the
 * scripts/build-*.{bat,sh} layout used by the editor's launcher. */
static std::string guess_output_exe(const std::string &preset_name)
{
    std::string p = preset_name;
    if (p.rfind("build-", 0) == 0) p.erase(0, 6);

    std::string variant = "release";
    auto dash = p.find_last_of('-');
    if (dash != std::string::npos) {
        std::string tail = p.substr(dash + 1);
        if (tail == "release" || tail == "debug" || tail == "asan")
            variant = tail;
    }
    std::string arch = (dash != std::string::npos) ? p.substr(0, dash) : p;

    if (arch.rfind("windows-", 0) == 0)
        return "build/desktop/" + arch + "/" + variant + "/caged_kingdom.exe";
    if (arch.rfind("macos-", 0) == 0 || arch.rfind("linux-", 0) == 0)
        return "build/desktop/" + arch + "/CagedKingdom";
    return "build/host/" + variant + "/caged_kingdom"
#if JCE_PLATFORM_WINDOWS
           ".exe"
#endif
        ;
}

static void apply_post_success(const JceBuildStatus &st)
{
    /* Make sure the workbench is open and focus the Report tab. */
    bool *bp_vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES);
    if (bp_vis) *bp_vis = true;
    jce_panel_build_profiles_request_tab(1);

    if (!s_bp.auto_update_run_path) return;

    std::string exe = guess_output_exe(st.preset);
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    snprintf(cfg.game_executable_path, sizeof(cfg.game_executable_path),
             "%s", exe.c_str());
    size_t slash = exe.find_last_of("/\\");
    if (slash != std::string::npos) {
        std::string cwd = exe.substr(0, slash);
        snprintf(cfg.game_working_directory,
                 sizeof(cfg.game_working_directory), "%s", cwd.c_str());
    }
    jce_editor_config_save(&cfg);
    jce_editor_console_log("[build] game_executable_path updated to %s",
                           exe.c_str());
}

} /* anonymous namespace */

/* Forward decl from jce_panel_build_report.cpp — the report body without
 * its own window chrome, so we can render it as a tab here. */
extern "C" void build_report_draw_content(void);

static int g_request_tab = -1;
static int g_current_tab = 0;  /* mirror of active TabItem for menu markers */

extern "C" void jce_panel_build_profiles_request_tab(int idx)
{
    g_request_tab = idx;
}

extern "C" int jce_panel_build_profiles_current_tab(void)
{
    return g_current_tab;
}

static void draw_profiles_tab(void)
{
    if (!s_bp.loaded) load_profiles();

    if (ImGui::Button(jce_editor_i18n("buildProfiles.refresh"))) load_profiles();
    ImGui::SameLine();
    ImGui::TextDisabled("%d %s", (int)s_bp.profiles.size(),
                        jce_editor_i18n("buildProfiles.preset"));

    ImGui::Separator();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    float left_w = sz.x * 0.45f; if (left_w < 240) left_w = 240;

    ImGui::BeginChild("##bplist", ImVec2(left_w, sz.y), true);
    if (s_bp.profiles.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s",
                           jce_editor_i18n("buildProfiles.noPresets"));
    }
    for (size_t i = 0; i < s_bp.profiles.size(); ++i) {
        Profile &p = s_bp.profiles[i];
        ImGui::PushID((int)i);
        bool active = (s_bp.active == (int)i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(80, 130, 200, 255));
        if (ImGui::Selectable(p.display.c_str(), active)) s_bp.active = (int)i;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !p.description.empty())
            ImGui::SetTooltip("%s", p.description.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##bpdetail", ImVec2(0, sz.y), true);

    JceBuildStatus st;
    jce_build_manager_get_status(&st);
    const bool running = jce_build_manager_is_running();

    if (s_bp.active >= 0 && s_bp.active < (int)s_bp.profiles.size()) {
        Profile &p = s_bp.profiles[s_bp.active];
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.preset"),     p.preset.c_str());
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.display"),    p.display.c_str());
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.generator"),  p.generator.c_str());
        ImGui::TextWrapped("%s: %s",
                           jce_editor_i18n("buildProfiles.description"),
                           p.description.empty() ? "—" : p.description.c_str());

        /* Predicted output path — mirrors what auto-update would write. */
        std::string predicted = guess_output_exe(p.preset);
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.outputPath"),
                    predicted.c_str());

        ImGui::Separator();

        if (running) ImGui::BeginDisabled();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.configure")))
            jce_build_manager_configure(p.preset.c_str());
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.build")))
            jce_build_manager_build(p.preset.c_str());
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.switchAndBuild"))) {
            jce_build_manager_configure(p.preset.c_str());
            jce_build_manager_build(p.preset.c_str());
        }
        if (running) ImGui::EndDisabled();

        ImGui::SameLine();
        if (!running) ImGui::BeginDisabled();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.stop")))
            jce_build_manager_request_stop();
        if (!running) ImGui::EndDisabled();

        ImGui::Checkbox(jce_editor_i18n("buildProfiles.autoUpdatePath"),
                        &s_bp.auto_update_run_path);

        ImGui::Separator();

        const char *stage_names[] = { "idle", "configure", "compile" };
        const char *state_names[] = { "idle", "running", "succeeded", "failed" };
        int s_idx = (int) st.state;
        int g_idx = (int) st.stage;
        if (s_idx < 0 || s_idx > 3) s_idx = 0;
        if (g_idx < 0 || g_idx > 2) g_idx = 0;

        ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        if (st.state == JCE_BUILD_RUNNING)   col = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
        if (st.state == JCE_BUILD_SUCCEEDED) col = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
        if (st.state == JCE_BUILD_FAILED)    col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(col, jce_editor_i18n("buildProfiles.statusLine"),
                           state_names[s_idx], stage_names[g_idx],
                           st.preset[0] ? st.preset : "(none)");
        if (st.last_error[0])
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s: %s",
                               jce_editor_i18n("buildProfiles.lastError"),
                               st.last_error);

        ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.consoleHint"));
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.selectPreset"));
    }
    ImGui::EndChild();

    /* Detect the IDLE→SUCCEEDED transition on the compile stage and
     * focus the Build Report tab + rewrite Play exe path. */
    if (s_bp.prev_state != JCE_BUILD_SUCCEEDED &&
        st.state       == JCE_BUILD_SUCCEEDED &&
        st.stage       == JCE_BUILD_STAGE_COMPILE)
    {
        apply_post_success(st);
    }
    s_bp.prev_state = st.state;
}

extern "C" void jce_editor_panel_build_profiles_content(void)
{
    if (!ImGui::BeginTabBar("##bp_tabs"))
        return;

    ImGuiTabItemFlags prof_flags   = (g_request_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags report_flags = (g_request_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;

    char prof_label[64];
    char report_label[64];
    std::snprintf(prof_label, sizeof(prof_label), "%s###bp_tab_profiles",
                  jce_editor_i18n("buildProfiles.title"));
    std::snprintf(report_label, sizeof(report_label), "%s###bp_tab_report",
                  jce_editor_i18n("panel.build_report.title"));

    if (ImGui::BeginTabItem(prof_label, nullptr, prof_flags)) {
        g_current_tab = 0;
        draw_profiles_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(report_label, nullptr, report_flags)) {
        g_current_tab = 1;
        build_report_draw_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_tab = -1;
}

extern "C" void jce_editor_panel_build_profiles(void)
{
    if (!*jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES)) return;
    char _wt[128];
    snprintf(_wt, sizeof(_wt), "%s###build_profiles", jce_editor_i18n("buildProfiles.title"));
    if (ImGui::Begin(_wt, jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES), ImGuiWindowFlags_NoFocusOnAppearing)) {
        jce_editor_panel_build_profiles_content();
    }
    ImGui::End();
}
