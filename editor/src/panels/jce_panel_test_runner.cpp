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
 * Execution: one non-blocking public process owner polls each selected case;
 * logs live in the shared profile/build tree. Cancel/timeout/incomplete output
 * never PASS even when a child exit code is zero.
 *
 * Engine-side test infrastructure is intentionally not added here; the
 * panel is the surface that the user drives whatever native binaries
 * the repo already builds (catch2, unity-test, ad-hoc smoke exes...).
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_test_run.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_colors.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace {

using CaseStatus = JceEditorTestRunStatus;

struct TestCase {
    std::string name;
    std::string args;
    CaseStatus  status = CaseStatus::Idle;
    double      duration_s = 0.0;
    std::string log, log_path, error;
    bool        arguments_valid = true;
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
    JceEditorTestRun      run;
    std::vector<std::pair<size_t,size_t>> queue;
    size_t               next = 0;
    int                  active_suite = -1, active_case = -1;
    int                  timeout_s = 60;
    uint64_t             ordinal = 0;
    std::string          error;
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
            tc.arguments_valid = jce_editor_test_run_arguments(args, tc.args, tc.error);
            if (!tc.arguments_valid) tc.status = CaseStatus::Failed;
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

static bool busy()
{
    return s_tr.run.process || s_tr.active_suite >= 0 || s_tr.next < s_tr.queue.size();
}
static void queue_suite(int selected)
{
    if (busy()) return;
    s_tr.queue.clear(); s_tr.next = 0; s_tr.error.clear();
    for (size_t si=0; si<s_tr.suites.size(); ++si) {
        if (selected>=0 && (size_t)selected!=si) continue;
        for (size_t ci=0; ci<s_tr.suites[si].cases.size(); ++ci)
            if (s_tr.suites[si].cases[ci].arguments_valid) s_tr.queue.emplace_back(si,ci);
    }
}
static void publish_result()
{
    if (s_tr.active_suite<0 || s_tr.run.process) return;
    TestSuite &suite=s_tr.suites[(size_t)s_tr.active_suite];
    TestCase &test=suite.cases[(size_t)s_tr.active_case];
    test.status=s_tr.run.status; test.duration_s=s_tr.run.duration_s;
    test.log=s_tr.run.output.substr(0,64*1024); test.log_path=s_tr.run.log_path; test.error=s_tr.run.error;
    if (s_tr.run.output.size()>test.log.size())
        test.log+="\n[UI preview shortened; complete evidence remains in the log above]\n";
    jce_editor_console_log_level(test.status==CaseStatus::Passed?JCE_CONSOLE_INFO:JCE_CONSOLE_WARNING,
        "[Test] %s/%s -> %s (%.3fs), exit=%d, log=%s", suite.name.c_str(),test.name.c_str(),
        jce_editor_test_run_status_name(test.status),test.duration_s,s_tr.run.exit_code,test.log_path.c_str());
    s_tr.active_suite=s_tr.active_case=-1;
}
static void start_next()
{
    if (s_tr.next>=s_tr.queue.size()) return;
    const auto item=s_tr.queue[s_tr.next++];
    TestSuite &suite=s_tr.suites[item.first]; TestCase &test=suite.cases[item.second];
    char directory[1024], path[1024], leaf[96];
    std::snprintf(leaf,sizeof(leaf),"case-%llu-%llu.log",
        (unsigned long long)jce_time_ticks_ns(),(unsigned long long)++s_tr.ordinal);
    if (!jce_editor_dotjce_path("build/editor-test-runs",directory,sizeof(directory)) ||
        !jce_path_join(path,sizeof(path),directory,leaf)) {
        s_tr.error="Test start refused: no writable profile/build log destination.";
        s_tr.queue.clear(); s_tr.next=0; return;
    }
    const bool started=jce_editor_test_run_start(s_tr.run,suite.exe.c_str(),test.args.c_str(),
        nullptr,path,(uint32_t)s_tr.timeout_s*1000u);
    if (!started && s_tr.run.log_path!=path) {
        s_tr.error=s_tr.run.error; s_tr.queue.clear(); s_tr.next=0;
        return; /* Preflight did not replace the previous case result. */
    }
    s_tr.active_suite=(int)item.first; s_tr.active_case=(int)item.second;
    test.status=s_tr.run.status; test.duration_s=0; test.log.clear(); test.error.clear();
    test.log_path=s_tr.run.log_path;
}
static void cancel_queue()
{
    s_tr.queue.clear(); s_tr.next=0;
    jce_editor_test_run_cancel(s_tr.run);
}

static ImU32 status_color(CaseStatus st)
{
    switch (st) {
        case CaseStatus::Passed:    return JCE_COL32_STATUS_OK;
        case CaseStatus::Failed:    return JCE_COL32_STATUS_ERR;
        case CaseStatus::Running: return IM_COL32(220, 200, 80, 255);
        default:         return IM_COL32(160, 160, 160, 255);
    }
}

static const char *status_label(CaseStatus status)
{
    return jce_editor_test_run_status_name(status);
}

} /* anonymous namespace */

extern "C" void jce_editor_panel_test_runner_content(void)
{
    set_default_dir();

    const bool running=busy();
    ImGui::BeginDisabled(running);
    ImGui::TextUnformatted(jce_editor_i18n("testRunner.dir"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-260);
    jce_draw_path_input("##trdir", s_tr.dir, sizeof(s_tr.dir), JcePathKind::FolderAbs);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("testRunner.rescan"))) rescan();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("testRunner.runAll"))) queue_suite(-1);
    char timeout_label[96];
    std::snprintf(timeout_label,sizeof(timeout_label),"%s (s)##test-run-timeout",jce_editor_i18n("ai.provider.timeout"));
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(timeout_label,&s_tr.timeout_s))
        s_tr.timeout_s=std::max(1,std::min(3600,s_tr.timeout_s));
    ImGui::EndDisabled();
    if (running && ImGui::Button(jce_editor_i18n("common.cancel"))) cancel_queue();
    if (!s_tr.error.empty()) ImGui::TextWrapped("%s",s_tr.error.c_str());

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
        ImGui::BeginDisabled(running);
        if (ImGui::SmallButton(jce_editor_i18n("testRunner.run"))) queue_suite((int)si);
        ImGui::EndDisabled();
        for (size_t ci = 0; ci < s.cases.size(); ++ci) {
            TestCase &c = s.cases[ci];
            total++;
            if (c.status == CaseStatus::Passed) pass_total++;
            else if (c.status == CaseStatus::Failed || c.status == CaseStatus::Cancelled || c.status == CaseStatus::TimedOut) fail_total++;
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
        if (!c.log_path.empty()) ImGui::TextWrapped("%s",c.log_path.c_str());
        if (!c.error.empty()) ImGui::TextWrapped("%s",c.error.c_str());
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

extern "C" void jce_editor_panel_test_runner_tick(void)
{
    jce_editor_test_run_poll(s_tr.run);
    if (s_tr.active_suite>=0 && s_tr.run.process) {
        TestCase &test=s_tr.suites[(size_t)s_tr.active_suite].cases[(size_t)s_tr.active_case];
        test.status=s_tr.run.status; test.duration_s=s_tr.run.duration_s;
    }
    publish_result();
    if (!s_tr.run.process && s_tr.active_suite<0) start_next();
}
extern "C" void jce_editor_panel_test_runner_shutdown(void)
{
    s_tr.queue.clear(); s_tr.next=0;
    jce_editor_test_run_close(s_tr.run);
    publish_result();
}
