/*
 * jce_panel_build_profiles.cpp  Unity-style Build Profiles panel.
 *                               Sprint 3 #14 / 0.8.26
 *
 * Surfaces a curated list of build "profiles" that wrap the existing
 * CMakePresets.json entries plus an editor-side label (icon, scenes,
 * extra defines). One profile == one CMake preset + per-profile
 * metadata persisted to .jce/build_profiles.json.
 *
 * Capabilities:
 *   - List discovered configure presets (read CMakePresets.json).
 *   - Tag each preset with a human label and an "active" marker.
 *   - "Build" button shells out to: cmake --build --preset <name>.
 *   - "Switch & Build" sets active profile + builds.
 *
 * The intent is to give the user one click to compile for, say,
 * windows-x64-debug vs android-arm64-release without typing CMake CLI.
 *
 * Engine-side cross-platform compile is unchanged; this is purely a
 * shortcut UI on top of CMakePresets.json.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Profile {
    std::string preset;
    std::string display;
    std::string description;
    std::string generator;
    bool        is_default = false;
};

static struct {
    std::vector<Profile> profiles;
    int                  active = -1;
    char                 last_log[2048] = {0};
    bool                 loaded = false;
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
            if (!pr.preset.empty() && pr.preset[0] != '_') /* skip "hidden": _xxx pattern */
                s_bp.profiles.push_back(pr);
        }
    }
    jce_json_free(root);
    s_bp.loaded = true;
    jce_editor_console_log("[Build Profiles] loaded %d preset(s)", (int)s_bp.profiles.size());
}

static void run_command(const char *cmd)
{
    char tmp[256]; snprintf(tmp, sizeof(tmp), "_jce_buildprof.log");
    char full[2048]; snprintf(full, sizeof(full), "%s > \"%s\" 2>&1", cmd, tmp);
    int rc = std::system(full);
    size_t sz = 0;
    char *buf = (char *)ed_read_file(tmp, &sz);
    if (buf) {
        size_t copy = sz < sizeof(s_bp.last_log) - 1 ? sz : sizeof(s_bp.last_log) - 1;
        memcpy(s_bp.last_log, buf, copy);
        s_bp.last_log[copy] = 0;
        ED_FREE(buf);
    }
    std::remove(tmp);
    jce_editor_console_log("[Build Profiles] '%s' rc=%d", cmd, rc);
}

static void configure_preset(const Profile &p)
{
    char cmd[512]; snprintf(cmd, sizeof(cmd), "cmake --preset %s", p.preset.c_str());
    run_command(cmd);
}

static void build_preset(const Profile &p)
{
    char cmd[512]; snprintf(cmd, sizeof(cmd), "cmake --build --preset %s", p.preset.c_str());
    run_command(cmd);
}

} /* anonymous namespace */

extern "C" void jce_editor_panel_build_profiles_content(void)
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
    if (s_bp.active >= 0 && s_bp.active < (int)s_bp.profiles.size()) {
        Profile &p = s_bp.profiles[s_bp.active];
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.preset"),     p.preset.c_str());
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.display"),    p.display.c_str());
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.generator"),  p.generator.c_str());
        ImGui::TextWrapped("%s: %s",
                           jce_editor_i18n("buildProfiles.description"),
                           p.description.empty() ? "—" : p.description.c_str());
        ImGui::Separator();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.configure"))) configure_preset(p);
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.build"))) build_preset(p);
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.switchAndBuild"))) {
            configure_preset(p);
            build_preset(p);
        }
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("buildProfiles.lastOutput"));
        ImGui::BeginChild("##bplog", ImVec2(0, 0), true);
        ImGui::TextUnformatted(s_bp.last_log[0] ? s_bp.last_log : jce_editor_i18n("buildProfiles.noOutput"));
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.selectPreset"));
    }
    ImGui::EndChild();
}

extern "C" void jce_editor_panel_build_profiles(void)
{
    if (!*jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES)) return;
    if (ImGui::Begin(jce_editor_i18n("buildProfiles.title"),
                     jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES))) {
        jce_editor_panel_build_profiles_content();
    }
    ImGui::End();
}
