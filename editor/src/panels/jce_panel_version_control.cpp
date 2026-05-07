/*
 * jce_panel_version_control.cpp  Git status panel.
 *
 * Read-only Git wrapper: runs `git status --porcelain=v1` in the project
 * root and renders the result. Refresh button re-runs. Designed to be
 * minimal and dependency-free (uses popen). A future revision can add
 * stage / commit / diff / push UI.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/platform/jce_host_shell.h>
}
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct VC_Entry { char status[4]; std::string path; };

static std::vector<VC_Entry> s_entries;
static std::string s_branch;
static std::string s_error;
static bool s_loaded = false;

static std::string run_git(const char *root, const char *const *git_args)
{
    if (!root || !*root) return {};
    /* Build argv: { "git", "-C", root, ...git_args, NULL } */
    std::vector<const char *> argv;
    argv.push_back("git");
    argv.push_back("-C");
    argv.push_back(root);
    for (size_t i = 0; git_args[i]; ++i) argv.push_back(git_args[i]);
    argv.push_back(nullptr);

    char *buf = nullptr;
    size_t got = 0;
    int    code = -1;
    std::string out;
    if (jce_host_run_capture(argv.data(), nullptr, &buf, &got, &code) && buf) {
        out.assign(buf, got);
        jce_free(buf);
    }
    return out;
}

static void refresh()
{
    s_entries.clear();
    s_branch.clear();
    s_error.clear();

    const char *root = jce_editor_assets_get_project();
    if (!root || !*root) { s_error = "No project open."; s_loaded = true; return; }

    const char *br_args[] = { "rev-parse", "--abbrev-ref", "HEAD", nullptr };
    std::string br = run_git(root, br_args);
    while (!br.empty() && (br.back() == '\n' || br.back() == '\r')) br.pop_back();
    if (br.find("not a git") != std::string::npos || br.find("fatal") != std::string::npos) {
        s_error = br.empty() ? "Not a git repo." : br;
        s_loaded = true;
        return;
    }
    s_branch = br;

    const char *st_args[] = { "status", "--porcelain=v1", nullptr };
    std::string st = run_git(root, st_args);
    size_t i = 0;
    while (i < st.size()) {
        size_t nl = st.find('\n', i);
        if (nl == std::string::npos) nl = st.size();
        if (nl - i >= 3) {
            VC_Entry e{};
            e.status[0] = st[i]; e.status[1] = st[i+1]; e.status[2] = '\0';
            e.path = st.substr(i + 3, nl - (i + 3));
            while (!e.path.empty() && (e.path.back() == '\r' || e.path.back() == '\n'))
                e.path.pop_back();
            s_entries.push_back(std::move(e));
        }
        i = nl + 1;
    }
    s_loaded = true;
}

static const char *status_label(const char *s)
{
    if (s[0] == 'M' || s[1] == 'M') return "Modified";
    if (s[0] == 'A')                return "Added";
    if (s[0] == 'D' || s[1] == 'D') return "Deleted";
    if (s[0] == 'R')                return "Renamed";
    if (s[0] == '?' && s[1] == '?') return "Untracked";
    return s;
}

extern "C" void jce_editor_panel_version_control_content(void)
{
    if (!s_loaded) refresh();

    if (ImGui::Button(jce_editor_i18n("common.refresh"))) refresh();
    ImGui::SameLine();
    if (!s_branch.empty()) ImGui::Text("%s %s", jce_editor_i18n("versionControl.branch"), s_branch.c_str());

    if (!s_error.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s_error.c_str());
        return;
    }

    ImGui::Separator();
    if (s_entries.empty()) {
        ImGui::TextDisabled(jce_editor_i18n("versionControl.clean"));
        return;
    }

    if (ImGui::BeginTable("##vc_tbl", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto &e : s_entries) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(status_label(e.status));
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.path.c_str());
        }
        ImGui::EndTable();
    }
}
