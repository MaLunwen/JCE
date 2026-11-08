/* Complete profile owner and actual Run gatherer/layout bodies; counted
 * config/project/state/exists ports. No native window, compiler or process. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_build_profile.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_project.h"
#include "core/jce_editor_project_state.h"
#include "core/jce_project_build_target.h"
#include "core/jce_run_manager_internal.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#ifndef JCE_TEST_PROJECT_RUN_GATHER
#error actual Run input gatherer is required
#endif
#ifndef JCE_TEST_PROJECT_RUN_LAYOUT
#error actual shared build-layout/naming bodies are required
#endif

char s_current_project_root[512] = "/projects/current";
namespace {
struct Ports {
    JceEditorConfig config{};
    JceProject project{};
    bool active = true, workspace = false, have_project = true;
    unsigned loads = 0, saves = 0;
    std::map<std::string, std::string> strings;
    std::map<std::string, int> ints;
    std::set<std::string> files;
} port;
#if JCE_PLATFORM_WINDOWS
constexpr const char *kExeSuffix = ".exe";
#else
constexpr const char *kExeSuffix = "";
#endif
static bool run_path_exists(const std::string &path, void *) {
    return port.files.count(path) != 0;
}
struct Fixture {
    Fixture() {
        port = {};
        port.active = port.have_project = true;
        std::strcpy(s_current_project_root, "/projects/current");
        port.project.project_root = s_current_project_root;
        port.project.target_name = const_cast<char *>("CurrentTarget");
        port.project.name = const_cast<char *>("Current Project");
        port.project.output_exe = const_cast<char *>("current-window");
        port.config.run_mode = 1;
        std::strcpy(port.config.game_executable_path, "foreign/output/foreign-game");
        std::strcpy(port.config.game_working_directory, "foreign/output");
        std::strcpy(port.config.game_target_name, "ForeignTarget");
        std::strcpy(port.config.build_configure_preset, "foreign-configure");
        std::strcpy(port.config.build_preset, "foreign-build");
        std::strcpy(port.config.build_output_path, "foreign/output");
        std::strcpy(port.config.build_project_root, "/projects/foreign");
        port.config.font_size = 19;
        std::strcpy(port.config.language, "zh_cn");
    }
};
std::string artifact(const char *variant, const char *name = "current-window") {
    const auto root = jce_project_build_root(s_current_project_root,
        jce_project_build_host_platform(), jce_project_build_host_arch());
    return jce_project_build_output_dir(root, variant) + "/" +
        jce_project_build_exe_name("CurrentTarget", name);
}
}
extern "C" bool jce_editor_config_load(JceEditorConfig *cfg) {
    ++port.loads; *cfg = port.config; return true;
}
extern "C" bool jce_editor_config_save(const JceEditorConfig *cfg) {
    ++port.saves; port.config = *cfg; return true;
}
extern "C" bool jce_editor_pstate_active() { return port.active; }
extern "C" bool jce_editor_pstate_get_str(const char *key, char *out, size_t cap) {
    const auto it = port.strings.find(key);
    if (it == port.strings.end() || !out || !cap) return false;
    std::snprintf(out, cap, "%s", it->second.c_str()); return true;
}
extern "C" void jce_editor_pstate_set_str(const char *key, const char *value) {
    port.strings[key] = value;
}
extern "C" int jce_editor_pstate_get_int(const char *key, int fallback) {
    const auto it = port.ints.find(key);
    return it == port.ints.end() ? fallback : it->second;
}
extern "C" void jce_editor_pstate_set_int(const char *key, int value) { port.ints[key] = value; }
extern "C" const char *jce_editor_current_project_root() { return s_current_project_root; }
extern "C" const JceProject *jce_editor_project_get() {
    return port.have_project ? &port.project : nullptr;
}
extern "C" bool jce_editor_project_is_engine_workspace() { return port.workspace; }

#include JCE_TEST_PROJECT_RUN_LAYOUT
#include JCE_TEST_PROJECT_RUN_GATHER

TEST_CASE("fresh ordinary profile drops another project's launch and build fields") {
    Fixture f;
    jce_editor_build_profile_restore();
    CHECK(port.config.run_mode == 0);
    CHECK(std::string(port.config.game_target_name) == "CurrentTarget");
    CHECK(std::string(port.config.build_project_root) == "/projects/current");
    CHECK(std::string(port.config.game_executable_path).empty());
    CHECK(std::string(port.config.game_working_directory).empty());
    CHECK(std::string(port.config.build_output_path).empty());
    CHECK(std::string(port.config.build_configure_preset).empty());
    CHECK(std::string(port.config.build_preset).empty());
    CHECK(port.config.font_size == 19);
    CHECK(std::string(port.config.language) == "zh_cn");
    CHECK(port.strings.at("build.target") == "CurrentTarget");
    CHECK(port.strings.at("build.exe").empty());
    CHECK(port.ints.at("build.run_mode") == 0);
    const auto saves = port.saves;
    jce_editor_build_profile_restore();
    CHECK(port.saves == saves);
}
TEST_CASE("fresh manifest without target uses the shared project-name fallback") {
    Fixture f;
    port.project.target_name = nullptr;
    jce_editor_build_profile_restore();
    CHECK(std::string(port.config.game_target_name) == "Current Project");
}
TEST_CASE("fresh asset folder has no inherited executable or invented target") {
    Fixture f;
    port.have_project = false;
    jce_editor_build_profile_restore();
    CHECK(std::string(port.config.game_target_name).empty());
    CHECK(std::string(port.config.game_executable_path).empty());
    CHECK(port.config.run_mode == 0);
    CHECK(port.strings.count("build.target") == 1);
}
TEST_CASE("engine workspace keeps first-sight global profile migration") {
    Fixture f;
    port.workspace = true;
    jce_editor_build_profile_restore();
    CHECK(std::string(port.config.game_target_name) == "ForeignTarget");
    CHECK(std::string(port.config.game_executable_path) == "foreign/output/foreign-game");
    CHECK(port.config.run_mode == 1);
    CHECK(port.saves == 0);
}
TEST_CASE("existing explicit project profile keeps its saved overrides") {
    Fixture f;
    port.strings = {{"build.target", "CustomTarget"}, {"build.exe", "/custom/app"},
        {"build.workdir", "/custom/work"}, {"build.output", "/custom/out"},
        {"build.configure_preset", "custom-configure"}, {"build.preset", "custom-build"},
        {"build.project_root", "/custom/source"}};
    port.ints["build.run_mode"] = 1;
    jce_editor_build_profile_restore();
    CHECK(std::string(port.config.game_target_name) == "CustomTarget");
    CHECK(std::string(port.config.game_executable_path) == "/custom/app");
    CHECK(std::string(port.config.game_working_directory) == "/custom/work");
    CHECK(std::string(port.config.build_output_path) == "/custom/out");
    CHECK(std::string(port.config.build_preset) == "custom-build");
    CHECK(std::string(port.config.build_project_root) == "/custom/source");
    CHECK(port.config.run_mode == 1);
}
TEST_CASE("inactive project state leaves the global profile untouched") {
    Fixture f;
    port.active = false;
    jce_editor_build_profile_restore();
    CHECK(port.loads == 0);
    CHECK(port.saves == 0);
    CHECK(std::string(port.config.game_target_name) == "ForeignTarget");
}
TEST_CASE("actual Run gatherer finds manifest output name in the current native layout") {
    Fixture f;
    jce_editor_build_profile_restore();
    port.files = {artifact("dist")};
    CHECK(resolve_executable("") == artifact("dist"));
}
TEST_CASE("actual Run gatherer searches release and debug without a duplicate layout") {
    Fixture f;
    jce_editor_build_profile_restore();
    for (const char *variant : {"release", "debug"}) {
        port.files = {artifact(variant)};
        CHECK(resolve_executable("") == artifact(variant));
    }
}
TEST_CASE("explicit external executable still wins over the derived project artifact") {
    Fixture f;
    jce_editor_build_profile_restore();
    port.files = {"/custom/executable", artifact("dist")};
    CHECK(resolve_executable("/custom/executable") == "/custom/executable");
}
TEST_CASE("configured output directory consumes the manifest output filename") {
    Fixture f;
    jce_editor_build_profile_restore();
    std::strcpy(port.config.build_output_path, "/custom/out");
    const auto expected = "/custom/out/" + jce_project_build_exe_name("CurrentTarget", "current-window");
    port.files = {expected, artifact("dist")};
    CHECK(resolve_executable("") == expected);
}
TEST_CASE("an explicit different target does not borrow another target's output name") {
    Fixture f;
    jce_editor_build_profile_restore();
    std::strcpy(port.config.game_target_name, "CustomTarget");
    const auto expected = artifact("dist", "CustomTarget");
    port.files = {expected, artifact("dist")};
    CHECK(resolve_executable("") == expected);
}
TEST_CASE("ordinary project Run never guesses an engine-workspace output") {
    Fixture f;
    jce_editor_build_profile_restore();
    const auto exe = jce_project_build_exe_name("CurrentTarget", "current-window");
    port.files = {"./build/desktop/windows-x64/release/" + exe};
    CHECK(resolve_executable("").empty());
}
