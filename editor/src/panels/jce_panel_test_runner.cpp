/*
 * jce_panel_test_runner.cpp  Unity-style Test Runner panel.
 *                            Sprint 3 #14 / 0.8.26
 *
 * Discovers test entry points by scanning a configurable directory
 * (default: scripts/tests) for *.test.json manifests describing one or
 * more named test cases and the executable they belong to. The panel
 * displays a tree (suite -> case), per-case status, last-run duration,
 * and stdout/stderr of the most recent run.
 *
 * Backing format (.test.json):
 *   {
 *     "suite": "Renderer Smoke",
 *     "exe":   "build/.../tests/render_smoke.exe",
 *     "cases": [
 *       { "name": "init_shutdown",    "args": ["--case=init"]    },
 *       { "name": "draw_one_quad",    "args": ["--case=quad"]    }
 *     ]
 *   }
 *
 * Execution: spawns the configured executable per case via system();
 * captures combined stdout+stderr to a temp log read back into the UI.
 * Exit code 0 = pass, anything else = fail.
 *
 * Engine-side test infrastructure is intentionally not added here; the
 * panel is the surface that the user drives whatever native binaries
 * the repo already builds (catch2, unity-test, ad-hoc smoke exes...).
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum CaseStatus { CS_PENDING = 0, CS_PASS = 1, CS_FAIL = 2, CS_RUNNING = 3 };

struct TestCase {
    std::string name;
    std::string args;
    CaseStatus  status = CS_PENDING;
    double      duration_s = 0.0;
    std::string log;
};

struct TestSuite {
    std::string source_path;
    std::string name;
    std::string exe;
    std::vector<TestCase> cases;
    bool        expanded = true;
};

static struct {
    char                 dir[260] = {0};
    std::vector<TestSuite> suites;
    int                  selected_suite = -1;
    int                  selected_case  = -1;
} s_tr;

static void set_default_dir()
{
    if (s_tr.dir[0]) return;
    snprintf(s_tr.dir, sizeof(s_tr.dir), "scripts/tests");
}

static bool ends_with_test_json(const char *name)
{
    size_t ln = std::strlen(name);
    const char *suf = ".test.json";
    size_t ls = std::strlen(suf);
    if (ln < ls) return false;
    return std::strcmp(name + (ln - ls), suf) == 0;
}

static bool tr_list_cb(const char *name, bool is_dir, void *user)
{
    (void)user;
    if (is_dir) return true;
    if (!ends_with_test_json(name)) return true;

    char fp[512]; snprintf(fp, sizeof(fp), "%s/%s", s_tr.dir, name);
    size_t sz = 0;
    char *buf = (char *)ed_read_file(fp, &sz);
    if (!buf) return true;
    JceJson *root = jce_json_parse(buf, (int)sz);
    ED_FREE(buf);
    if (!root) return true;

    TestSuite s;
    s.source_path = fp;
    const char *nm = jce_json_get_string(root, "suite", "");
    const char *ex = jce_json_get_string(root, "exe",   "");
    s.name = (nm && *nm) ? nm : name;
    s.exe  = ex ? ex : "";
    JceJson *cases = jce_json_get(root, "cases");
    if (jce_json_is_array(cases)) {
        int n = jce_json_array_size(cases);
        for (int i = 0; i < n; ++i) {
            JceJson *c = jce_json_array_at(cases, i);
            TestCase tc;
            const char *cn = jce_json_get_string(c, "name", "case");
            tc.name = cn ? cn : "case";
            JceJson *args = jce_json_get(c, "args");
            if (jce_json_is_array(args)) {
                int an = jce_json_array_size(args);
                for (int j = 0; j < an; ++j) {
                    JceJson *a = jce_json_array_at(args, j);
                    const char *as = a ? jce_json_get_string(a, "", "") : "";
                    if (j) tc.args += ' ';
                    tc.args += (as ? as : "");
                }
            }
            s.cases.push_back(tc);
        }
    }
    jce_json_free(root);
    s_tr.suites.push_back(s);
    return true;
}

static void rescan()
{
    s_tr.suites.clear();
    if (!jce_fs_host_exists_dir(s_tr.dir)) {
        jce_editor_console_log("[Test Runner] no manifests found in %s", s_tr.dir);
        return;
    }
    jce_fs_host_list_dir(s_tr.dir, &tr_list_cb, nullptr);
    jce_editor_console_log("[Test Runner] discovered %d suite(s)", (int)s_tr.suites.size());
}

static void run_case(TestSuite &s, TestCase &tc)
{
    if (s.exe.empty()) {
        tc.status = CS_FAIL;
        tc.log = "(suite has no exe)";
        return;
    }
    char tmp[512]; snprintf(tmp, sizeof(tmp), "_jce_testrun_%p.log", (void *)&tc);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s > \"%s\" 2>&1",
             s.exe.c_str(), tc.args.c_str(), tmp);
    tc.status = CS_RUNNING;
    double t0 = ImGui::GetTime();
    int rc = std::system(cmd);
    tc.duration_s = ImGui::GetTime() - t0;
    tc.status = (rc == 0) ? CS_PASS : CS_FAIL;
    size_t sz = 0;
    char *buf = (char *)ed_read_file(tmp, &sz);
    if (buf) {
        tc.log.assign(buf, sz);
        ED_FREE(buf);
    } else {
        tc.log = "(no output)";
    }
    std::remove(tmp);
    jce_editor_console_log("[Test] %s/%s -> %s (%.3fs)",
                           s.name.c_str(), tc.name.c_str(),
                           rc == 0 ? "PASS" : "FAIL", tc.duration_s);
}

static void run_all()
{
    for (auto &s : s_tr.suites)
        for (auto &c : s.cases) run_case(s, c);
}

static ImU32 status_color(CaseStatus st)
{
    switch (st) {
        case CS_PASS:    return IM_COL32(80, 220, 120, 255);
        case CS_FAIL:    return IM_COL32(220, 80, 80,  255);
        case CS_RUNNING: return IM_COL32(220, 200, 80, 255);
        default:         return IM_COL32(160, 160, 160, 255);
    }
}

static const char *status_label(CaseStatus st)
{
    switch (st) {
        case CS_PASS:    return "PASS";
        case CS_FAIL:    return "FAIL";
        case CS_RUNNING: return "...";
        default:         return "—";
    }
}

} /* anonymous namespace */

extern "C" void jce_editor_panel_test_runner_content(void)
{
    set_default_dir();

    ImGui::TextUnformatted(jce_editor_i18n("testRunner.dir"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-260);
    jce_draw_path_input("##trdir", s_tr.dir, sizeof(s_tr.dir), JcePathKind::FolderAbs);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("testRunner.rescan"))) rescan();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("testRunner.runAll"))) run_all();

    ImGui::Separator();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    float left_w = sz.x * 0.45f; if (left_w < 220) left_w = 220;
    ImGui::BeginChild("##trtree", ImVec2(left_w, sz.y), true);
    int pass_total = 0, fail_total = 0, total = 0;
    for (size_t si = 0; si < s_tr.suites.size(); ++si) {
        TestSuite &s = s_tr.suites[si];
        ImGui::PushID((int)si);
        if (ImGui::Selectable(s.name.c_str(), s_tr.selected_suite == (int)si, 0, ImVec2(left_w * 0.7f, 0))) {
            s_tr.selected_suite = (int)si;
            s_tr.selected_case  = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("testRunner.run"))) {
            for (auto &c : s.cases) run_case(s, c);
        }
        for (size_t ci = 0; ci < s.cases.size(); ++ci) {
            TestCase &c = s.cases[ci];
            total++;
            if (c.status == CS_PASS) pass_total++;
            else if (c.status == CS_FAIL) fail_total++;
            ImGui::PushID((int)ci);
            ImGui::Indent(16);
            char lbl[128]; snprintf(lbl, sizeof(lbl), "  %s", c.name.c_str());
            bool sel = (s_tr.selected_suite == (int)si && s_tr.selected_case == (int)ci);
            if (ImGui::Selectable(lbl, sel)) {
                s_tr.selected_suite = (int)si;
                s_tr.selected_case  = (int)ci;
            }
            ImGui::SameLine(left_w * 0.55f);
            ImGui::PushStyleColor(ImGuiCol_Text, status_color(c.status));
            ImGui::TextUnformatted(status_label(c.status));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%.2fs", c.duration_s);
            ImGui::Unindent(16);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##trlog", ImVec2(0, sz.y), true);
    ImGui::Text("%s: %d   %s: %d   %s: %d",
                jce_editor_i18n("testRunner.total"), total,
                jce_editor_i18n("testRunner.pass"),  pass_total,
                jce_editor_i18n("testRunner.fail"),  fail_total);
    ImGui::Separator();
    if (s_tr.selected_suite >= 0 && s_tr.selected_suite < (int)s_tr.suites.size()
     && s_tr.selected_case  >= 0
     && s_tr.selected_case  < (int)s_tr.suites[s_tr.selected_suite].cases.size()) {
        const TestCase &c = s_tr.suites[s_tr.selected_suite].cases[s_tr.selected_case];
        ImGui::Text("%s: %s", jce_editor_i18n("testRunner.case"), c.name.c_str());
        ImGui::Text("%s: %s", jce_editor_i18n("testRunner.args"), c.args.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("testRunner.output"));
        ImGui::BeginChild("##trout", ImVec2(0, 0), true);
        ImGui::TextUnformatted(c.log.c_str());
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("testRunner.selectCase"));
    }
    ImGui::EndChild();
}

extern "C" void jce_editor_panel_test_runner(void)
{
    if (!*jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER)) return;
    char _wt[128];
    snprintf(_wt, sizeof(_wt), "%s###test_runner", jce_editor_i18n("testRunner.title"));
    if (ImGui::Begin(_wt, jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER), ImGuiWindowFlags_NoFocusOnAppearing)) {
        jce_editor_panel_test_runner_content();
    }
    ImGui::End();
}
