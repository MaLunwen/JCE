#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_project_context.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <cstring>
#include "jce_test_owned_directory.h"
#include "core/jce_editor_project_state.h"
#include <jce/os/core/jce_str.h>
#include <cstdio>
#include <string>

struct Fixture {
    JceTestOwnedDirectory directory{"_ut_project_context"};
    std::string root = directory.root;
    std::string project(const char *name, const char *manifest = "{\"schema\":3,\"name\":\"fixture\"}")
    {
        const std::string path = root + "/" + name;
        REQUIRE(jce_fs_host_create_directory(path.c_str()));
        const std::string file = path + "/jce_project.json";
        REQUIRE(jce_fs_host_write_all(file.c_str(), manifest, std::strlen(manifest)));
        return path;
    }
};

TEST_CASE("Bundle preserves valid sibling authoring context; plain Scene changes project")
{
    Fixture f;
    const auto a = f.project("author"), b = f.project("physical");
    char result[1024];
    REQUIRE(jce_editor_project_context_select((b + "/content.jbundle").c_str(), a.c_str(), true, result, sizeof(result)));
    CHECK(std::string(result) == a);
    REQUIRE(jce_editor_project_context_select((b + "/scene.json").c_str(), a.c_str(), false, result, sizeof(result)));
    CHECK(std::string(result) == b);
}
TEST_CASE("canonical manifest resolves and input normalization does not mutate caller")
{
    Fixture f;
    const auto a = f.project("canonical");
    std::string input = a + "/./jce_project.json";
    const auto original = input;
    char result[1024];
    REQUIRE(jce_editor_project_context_resolve(input.c_str(), result, sizeof(result)));
    CHECK(std::string(result) == a);
    CHECK(input == original);
    REQUIRE(jce_editor_project_context_resolve(result, result, sizeof(result)));
    CHECK(std::string(result) == a);
}
TEST_CASE("public ancestor search works beyond twelve levels")
{
    Fixture f;
    const auto a = f.project("deep");
    std::string path = a;
    for (int i = 0; i < 14; ++i) path += "/level";
    REQUIRE(jce_fs_host_create_directory(path.c_str()));
    char result[1024];
    REQUIRE(jce_editor_project_context_select((path + "/asset.jbundle").c_str(), nullptr, true, result, sizeof(result)));
    CHECK(std::string(result) == a);
}
TEST_CASE("invalid or malformed explicit roots fall back to content project only")
{
    Fixture f;
    const auto valid = f.project("valid"), bad = f.project("malformed", "{");
    char result[1024];
    for (const auto &active : {bad, f.root + "/missing"}) {
        REQUIRE(jce_editor_project_context_select((valid + "/asset.jbundle").c_str(), active.c_str(), true, result, sizeof(result)));
        CHECK(std::string(result) == valid);
    }
    REQUIRE(jce_fs_host_create_directory((valid + "/not_project").c_str()));
    CHECK_FALSE(jce_editor_project_context_resolve((valid + "/not_project").c_str(), result, sizeof(result)));
    CHECK(result[0] == '\0');
}
TEST_CASE("legacy filename is not canonical and missing projects terminate")
{
    Fixture f;
    const auto legacy = f.root + "/project.jce";
    const char data[] = "{\"name\":\"legacy\"}";
    REQUIRE(jce_fs_host_write_all(legacy.c_str(), data, sizeof(data) - 1));
    char result[1024];
    CHECK_FALSE(jce_editor_project_context_resolve(legacy.c_str(), result, sizeof(result)));
    CHECK_FALSE(jce_editor_project_context_select((f.root + "/none.json").c_str(), nullptr, false, result, sizeof(result)));
    CHECK(result[0] == '\0');
    CHECK_FALSE(jce_editor_project_context_select("/", nullptr, false, result, sizeof(result)));
}
TEST_CASE("overflow and invalid buffers fail without overwriting adjacent storage")
{
    Fixture f;
    const auto a = f.project("long");
    char out[] = {'x', 'y', 'z'};
    CHECK_FALSE(jce_editor_project_context_resolve(a.c_str(), out, 1));
    CHECK(out[0] == '\0'); CHECK(out[1] == 'y'); CHECK(out[2] == 'z');
    CHECK_FALSE(jce_editor_project_context_resolve(a.c_str(), nullptr, 3));
    CHECK_FALSE(jce_editor_project_context_resolve(a.c_str(), out, 0));
    CHECK_FALSE(jce_editor_project_context_resolve(std::string(1100, 'x').c_str(), out, sizeof(out)));
    CHECK(out[0] == '\0');
}
TEST_CASE("active context priority does not become a fallback when output is too small")
{
    Fixture f;
    const auto a = f.project("authoring"), b = f.project("b");
    char out[2] = {'x', 'y'};
    CHECK_FALSE(jce_editor_project_context_select((b + "/bundle.jbundle").c_str(), a.c_str(), true, out, sizeof(out)));
    CHECK(out[0] == '\0'); CHECK(out[1] == 'y');
}

/* Exact dialog/scene admission bodies. The public resolver and filesystem are
 * real; these publication ports model refusal without claiming GUI/ECS rollback.
 * The extracted setter stops at its admitted boundary, before subsystem binding. */
namespace {
std::string active_project;
bool publication_ok = false;
bool pending_dirty = true;
unsigned publications = 0, profiles = 0, favorites = 0, errors = 0;
constexpr int JCE_CONSOLE_ERROR = 2;
void reset_publication(const std::string &active)
{
    active_project = active;
    publication_ok = false;
    pending_dirty = true;
    publications = profiles = favorites = errors = 0;
}
void jce_editor_build_profile_snapshot(void) { ++profiles; }
void jce_editor_favorites_snapshot(void) { ++favorites; }
void jce_editor_console_log_level(int level, const char *message)
{
    CHECK(level == JCE_CONSOLE_ERROR);
    CHECK(std::strstr(message, "Project switch refused:") != nullptr);
    ++errors;
}
}
extern "C" const char *jce_editor_current_project_root(void)
{
    return active_project.c_str();
}
extern "C" bool jce_editor_pstate_flush_now(void)
{
    ++publications;
    if (publication_ok) pending_dirty = false;
    return publication_ok;
}
#include JCE_TEST_EDITOR_PROJECT_PREFLIGHT_BODY

TEST_CASE("same-project Scene and explicit Bundle context do not force failed publication")
{
    Fixture f;
    const auto a = f.project("a"), b = f.project("physical");
    reset_publication(a);
    CHECK(prepare_scene_project_switch((a + "/assets/other.scene").c_str()));
    CHECK(prepare_scene_project_switch((b + "/content.jbundle").c_str(), true));
    CHECK(set_current_project_root((a + "/jce_project.json").c_str()));
    std::string windows_form = a + "/";
    for (char &c : windows_form) if (c == '/') c = '\\';
    CHECK(set_current_project_root(windows_form.c_str()));
    CHECK(publications == 0);
    CHECK(profiles == 0); CHECK(favorites == 0); CHECK(errors == 0);
    CHECK(pending_dirty); CHECK(active_project == a);
}
TEST_CASE("loose Scene preflight and real B both require outgoing publication")
{
    Fixture f;
    const auto a = f.project("a"), b = f.project("b");
    reset_publication(a);
    CHECK_FALSE(prepare_scene_project_switch((f.root + "/unowned.scene").c_str()));
    CHECK(publications == 1); CHECK(pending_dirty);
    CHECK_FALSE(prepare_scene_project_switch((b + "/assets/main.scene").c_str()));
    CHECK(publications == 2); CHECK(profiles == 2); CHECK(favorites == 2);
    CHECK(errors == 2); CHECK(active_project == a); CHECK(pending_dirty);
    CHECK_FALSE(set_current_project_root(nullptr));
    CHECK(publications == 3); CHECK(errors == 3);
    CHECK(active_project == a); CHECK(pending_dirty);
}
TEST_CASE("retry admits B only after publication and empty identity stays inert")
{
    Fixture f;
    const auto a = f.project("a"), b = f.project("b");
    reset_publication(a);
    CHECK_FALSE(set_current_project_root((b + "/jce_project.json").c_str()));
    CHECK(publications == 1); CHECK(pending_dirty);
    publication_ok = true;
    CHECK(set_current_project_root((b + "/jce_project.json").c_str()));
    CHECK(publications == 2); CHECK_FALSE(pending_dirty);
    CHECK(active_project == a); /* Admission prefix does not commit a root. */
    reset_publication("");
    CHECK(set_current_project_root(""));
    CHECK(publications == 0); CHECK(profiles == 0); CHECK(errors == 0);
}
TEST_CASE("oversized dialog root rejects before outgoing overlay mutation")
{
    Fixture f;
    const auto a = f.project("a");
    reset_publication(a);
    CHECK_FALSE(set_current_project_root(std::string(512, 'x').c_str()));
    CHECK(errors == 1); CHECK(publications == 0);
    CHECK(profiles == 0); CHECK(favorites == 0); CHECK(pending_dirty);
    CHECK(active_project == a);
}

TEST_CASE("project identity follows actual Windows and POSIX case policy")
{
    Fixture f;
    const auto a = f.project("Case");
    const auto other = f.root + "/case";
    char resolved[1024];
#if JCE_PLATFORM_WINDOWS
    CHECK(jce_editor_project_path_equal(a.c_str(), other.c_str()));
    REQUIRE(jce_editor_project_context_resolve(other.c_str(), resolved, sizeof(resolved)));
    reset_publication(a);
    CHECK(prepare_scene_project_switch((other + "/scene.json").c_str()));
    CHECK(set_current_project_root((other + "/jce_project.json").c_str()));
    CHECK(publications == 0); CHECK(profiles == 0); CHECK(errors == 0);
#else
    REQUIRE(f.project("case") == other); /* A distinct real POSIX project. */
    CHECK_FALSE(jce_editor_project_path_equal(a.c_str(), other.c_str()));
    REQUIRE(jce_editor_project_context_resolve(other.c_str(), resolved, sizeof(resolved)));
    reset_publication(a);
    CHECK_FALSE(prepare_scene_project_switch((other + "/scene.json").c_str()));
    CHECK_FALSE(set_current_project_root((other + "/jce_project.json").c_str()));
    CHECK(publications == 2); CHECK(profiles == 2); CHECK(errors == 2);
#endif
    CHECK(pending_dirty); CHECK(active_project == a);
    CHECK(jce_editor_project_path_equal(nullptr, ""));
    CHECK_FALSE(jce_editor_project_path_equal(a.c_str(), nullptr));
}
