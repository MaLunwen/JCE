#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_build_manager.h"
#include "core/jce_build_log.h"
#include <jce/os/core/jce_defs.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define jce_build_manager_start_project_build probe_start_project_build
#define jce_build_manager_reject_request probe_reject_request
#define jce_build_manager_get_status probe_get_status
namespace {
enum class JceProjectBuildPlatform { Windows, MacOS, Linux };
const char *jce_project_build_arch(const char *arch, const char *host) { return arch && arch[0] ? arch : host; }
enum JceConsoleLevel { JCE_CONSOLE_INFO, JCE_CONSOLE_WARNING, JCE_CONSOLE_ERROR };
struct Build {
    void *process = nullptr;
    JceBuildState state = JCE_BUILD_IDLE;
    JceBuildStage stage = JCE_BUILD_STAGE_NONE;
    int exit_code = 0;
    std::string preset, last_error, artifact_path, package_path, asset_bom_path, dist_audit_path;
    JceBuildLog log;
    std::string stdout_partial, stderr_partial;
} g_build;
struct FinishPlan { unsigned char pak_key[32]{}; } g_finish;
std::vector<int> g_queue;
size_t g_queue_pos = 0;
using WorkerLogSink = JceBuildWorkerLog<JceConsoleLevel>;
WorkerLogSink *t_log_sink = nullptr;
bool prep_active = false;
int console_count = 0, cache_removals = 0, tool_refreshes = 0;
bool config_present = true, sdk_valid = true, tools_present = true;
bool clean_ok = true, cache_stale = false, cache_file_remove_ok = true;
constexpr char PATH_SEP_CHR_LOCAL = '/';
bool asset_prep_active() { return prep_active; }
void jce_editor_console_log_level(JceConsoleLevel, const char *, ...) { ++console_count; }
void log_line(JceConsoleLevel, const std::string &line) { g_build.log.append_line(line); }
std::string join_norm_sep(const char *path) { return path; }
bool jce_build_package_component_valid(const std::string &, std::string *) { return true; }
bool jce_build_package_admit(const std::string &, const std::string &, const std::string &,
    const char *out, const std::string &, bool, std::string *result, std::string *)
{ *result = out; return true; }
bool jce_fs_host_exists_file(const char *) { return config_present; }
std::string jce_project_build_sdk_target_error(const std::string &, const char *,
    JceProjectBuildPlatform, const std::string &) { return sdk_valid ? "" : "SDK mismatch"; }
void jce_toolchain_refresh() { ++tool_refreshes; }
std::string jce_build_tool_executable(const char *tool) { return tools_present ? tool : ""; }
const char *jce_project_build_host_arch() { return "x86_64"; }
const char *jce_project_build_target_error(const char *arch, const char *, JceProjectBuildPlatform)
{ return std::strcmp(arch, "x86_64") == 0 ? nullptr : "target refused"; }
std::string jce_project_build_exe_name(const std::string &target, const char *exe)
{ return exe ? exe : target; }
std::string jce_project_build_root(const std::string &root, JceProjectBuildPlatform, const std::string &)
{ return root + "/build/host"; }
std::string jce_project_build_output_dir(const std::string &root, const std::string &variant)
{ return root + "/" + variant; }
std::string project_reports_dir(const std::string &root) { return root + "/reports"; }
std::string project_intermediate_dir(const std::string &root, const std::string &variant)
{ return root + "/generated/" + variant; }
#if JCE_PLATFORM_WINDOWS
struct JceToolchain { bool present = false; char path[2]{}; };
constexpr int JCE_TOOLCHAIN_MSVC = 0;
const JceToolchain *jce_toolchain_get(int) { return nullptr; }
std::string find_msvc_c_compiler(const std::string &, const std::string &) { return ""; }
#endif
bool remove_project_build_cache(const std::string &, const std::string &, const std::string &)
{ ++cache_removals; return clean_ok; }
bool project_cache_needs_reset(const std::string &, const std::string &, const std::string &,
    const std::string &, std::string *reason) { *reason = "fixture"; return cache_stale; }
bool jce_fs_host_remove_file(const char *) { return cache_file_remove_ok; }

struct PanelState {
    enum PlayStage { PLAY_IDLE, PLAY_COOKING, PLAY_BUILDING, PLAY_LAUNCH_READY,
        PLAY_RUNNING, PLAY_DONE, PLAY_FAILED };
    PlayStage play_stage = PLAY_IDLE;
    char play_fail_msg[256]{};
} s_bp;
JceBuildStatus st{};
bool dispatch_accepted = false;
int run_count = 0;
bool start_build_via_script() { return dispatch_accepted; }
const char *jce_editor_i18n(const char *) { return "%s"; }
void jce_build_manager_get_status(JceBuildStatus *out)
{ out->state = g_build.state; std::snprintf(out->last_error, sizeof out->last_error, "%s", g_build.last_error.c_str()); }
struct JceEditorConfig { char game_executable_path[256]{}, game_working_directory[256]{}; };
struct JceRunConfig { char executable_path[256]{}, working_directory[256]{};
    bool capture_stdout = false, capture_stderr = false; };
void jce_editor_config_load(JceEditorConfig *cfg)
{ std::snprintf(cfg->game_executable_path, sizeof cfg->game_executable_path, "old-success.exe"); }
bool jce_run_manager_start(const JceRunConfig *) { ++run_count; return true; }

#include JCE_TEST_BUILD_REQUEST_ACTUAL_HEADER

JceBuildProjectConfig request()
{
    JceBuildProjectConfig cfg{};
    cfg.label = "current-request"; cfg.project_dir = "project";
    cfg.sdk_dir = "sdk"; cfg.target = "game"; cfg.variant = "dist";
    return cfg;
}
void reset_fixture()
{
    g_build = Build{}; prep_active = false; config_present = sdk_valid = tools_present = true;
    clean_ok = cache_file_remove_ok = true; cache_stale = false;
    console_count = cache_removals = tool_refreshes = 0;
    g_queue = {7}; g_queue_pos = 1;
    g_build.state = JCE_BUILD_SUCCEEDED; g_build.stage = JCE_BUILD_STAGE_COMPILE;
    g_build.exit_code = 0; g_build.preset = "old-request"; g_build.last_error = "old-error";
    g_build.artifact_path = "old.exe"; g_build.package_path = "old/package";
    g_build.asset_bom_path = "old/bom"; g_build.dist_audit_path = "old/audit";
    g_build.log.append_line("old transcript");
    s_bp = PanelState{}; st = {}; dispatch_accepted = false; run_count = 0;
}
void check_current_refusal(const char *reason)
{
    CHECK(g_build.state == JCE_BUILD_FAILED);
    CHECK(g_build.stage == JCE_BUILD_STAGE_NONE);
    CHECK(g_build.exit_code == -1);
    CHECK(g_build.last_error.find(reason) != std::string::npos);
    CHECK(g_build.artifact_path.empty()); CHECK(g_build.package_path.empty());
    CHECK(g_build.asset_bom_path.empty()); CHECK(g_build.dist_audit_path.empty());
    CHECK(g_build.log.text.find("old transcript") == std::string::npos);
    CHECK(g_queue.empty()); CHECK(g_queue_pos == 0);
}
} // namespace

TEST_CASE("current invalid request replaces old success before any tool or cache work")
{
    for (int missing = 0; missing < 4; ++missing) {
        reset_fixture(); auto cfg = request();
        if (missing == 1) cfg.project_dir = nullptr;
        if (missing == 2) cfg.target = "";
        if (missing == 3) cfg.sdk_dir = nullptr;
        CHECK_FALSE(jce_build_manager_start_project_build(missing ? &cfg : nullptr));
        const char *errors[] = {"cfg is null", "project_dir", "target", "sdk_dir"};
        check_current_refusal(errors[missing]);
        CHECK(cache_removals == 0); CHECK(tool_refreshes == 0);
    }
}
TEST_CASE("SDK package and clean/reset refusals clear previous artifact identities")
{
    for (int failure = 0; failure < 4; ++failure) {
        reset_fixture(); auto cfg = request();
        if (failure == 0) config_present = false;
        if (failure == 1) { cfg.clean = true; clean_ok = false; }
        if (failure == 2) { cache_stale = true; clean_ok = cache_file_remove_ok = false; }
        if (failure == 3) tools_present = false;
        CHECK_FALSE(jce_build_manager_start_project_build(&cfg));
        const char *errors[] = {"SDK CMake package", "remove project build cache", "reset project build cache", "CMake and Ninja"};
        check_current_refusal(errors[failure]);
    }
}
TEST_CASE("busy process and asset preparation preserve their entire request on reentry")
{
    for (bool worker : {false, true}) {
        reset_fixture(); g_build.state = JCE_BUILD_RUNNING;
        if (worker) prep_active = true; else g_build.process = &g_build;
        const auto transcript = g_build.log.text;
        CHECK_FALSE(jce_build_manager_start_project_build(nullptr));
        CHECK_FALSE(jce_build_manager_reject_request("refused", "preflight failure"));
        CHECK(g_build.state == JCE_BUILD_RUNNING);
        CHECK(g_build.stage == JCE_BUILD_STAGE_COMPILE); CHECK(g_build.exit_code == 0);
        CHECK(g_build.preset == "old-request"); CHECK(g_build.last_error == "old-error");
        CHECK(g_build.artifact_path == "old.exe"); CHECK(g_build.package_path == "old/package");
        CHECK(g_build.asset_bom_path == "old/bom"); CHECK(g_build.dist_audit_path == "old/audit");
        CHECK(g_build.log.text == transcript); CHECK(g_queue.size() == 1); CHECK(g_queue_pos == 1);
        CHECK(console_count == 2); CHECK(cache_removals == 0); CHECK(tool_refreshes == 0);
    }
}
TEST_CASE("editor preflight refusal uses the same current-request owner")
{
    reset_fixture(); CHECK_FALSE(jce_build_manager_reject_request("package-game", "path refused"));
    check_current_refusal("path refused"); CHECK(g_build.preset == "package-game");
}
TEST_CASE("both Play entry points require dispatch acceptance before consuming old success")
{
    for (auto initial : {PanelState::PLAY_IDLE, PanelState::PLAY_COOKING}) {
        reset_fixture(); s_bp.play_stage = initial;
        dispatch_play_build();
        CHECK(s_bp.play_stage == PanelState::PLAY_FAILED);
        advance_play_build(); advance_play_build();
        CHECK(run_count == 0);
        dispatch_accepted = true; g_build.state = JCE_BUILD_RUNNING;
        dispatch_play_build(); CHECK(s_bp.play_stage == PanelState::PLAY_BUILDING);
        advance_play_build(); CHECK(run_count == 0);
        g_build.state = JCE_BUILD_SUCCEEDED;
        jce_build_manager_get_status(&st);
        advance_play_build(); CHECK(s_bp.play_stage == PanelState::PLAY_LAUNCH_READY);
        advance_play_build(); CHECK(run_count == 1);
    }
}
