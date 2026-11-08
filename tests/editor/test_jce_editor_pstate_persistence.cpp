/* Actual project-state owner and native atomic JSON/file IO; no Editor window. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include "core/jce_editor_project_state.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_path.h>
#include <cstring>
#include <string>

namespace {
struct Project {
    JceTestOwnedDirectory directory{"_ut_editor_pstate_retry"};
    std::string root = directory.root, previous;
    Project()
    {
        previous = jce_editor_current_project_root();
        REQUIRE(jce_editor_current_project_root_set(root.c_str()));
        REQUIRE(jce_editor_pstate_active());
    }
    std::string path(const char *leaf) const { return root + "/" + leaf; }
    ~Project()
    {
        CHECK(jce_editor_pstate_flush_now());
        CHECK(jce_editor_current_project_root_set(previous.c_str()));
        jce_editor_pstate_flush_tick(0);
    }
};
void require_state(const std::string &path, int mode)
{
    JceJson *file = jce_json_parse_file(path.c_str());
    CHECK(file != nullptr);
    if (!file) return; /* Preserve both ordinary missing-retry witnesses. */
    CHECK(jce_json_get_int(file, "_schema", 0) == 1);
    JceJson *kv = jce_json_get(file, "kv");
    CHECK(jce_json_get_int(kv, "view_mode", -1) == mode);
    CHECK(std::strcmp(jce_json_get_string(kv, "bookmark_label", ""), "project state") == 0);
    JceJson *scene = jce_json_get(jce_json_get(file, "scenes"), "scenes/demo.scene");
    CHECK(jce_json_get_int(scene, "grid", -1) == 3);
    jce_json_free(file);
}
void remove_owned_blocker(const Project &project, const std::string &path)
{
    REQUIRE(path == project.path(".jce/editor-state.json"));
    REQUIRE(jce_fs_host_exists_dir(path.c_str()));
    REQUIRE(jce_fs_host_remove_recursive(path.c_str()));
}
}

TEST_CASE("actual project-state timed and forced writes retain dirty state after publication failure")
{
    Project project;
    const auto state = project.path(".jce/editor-state.json");
    jce_editor_pstate_set_int("view_mode", 3);
    jce_editor_pstate_set_str("bookmark_label", "project state");
    jce_editor_pstate_scene_set_int("scenes/demo.scene", "grid", 3);
    jce_editor_pstate_flush_tick(-1.0f);
    jce_editor_pstate_flush_tick(0.49f);
    CHECK_FALSE(jce_fs_host_exists_file(state.c_str()));
    jce_editor_pstate_flush_tick(0.02f);
    require_state(state, 3);

    INFO("timed failed publication preserves the same pending state and debounce");
    const auto timed_before = project.path("timed-before.json");
    REQUIRE(jce_fs_host_rename(state.c_str(), timed_before.c_str()));
    REQUIRE(jce_fs_host_create_directory(state.c_str()));
    jce_editor_pstate_set_int("view_mode", 7);
    jce_editor_pstate_flush_tick(0.51f);
    CHECK(jce_fs_host_exists_dir(state.c_str()));
    require_state(timed_before, 3);
    remove_owned_blocker(project, state);
    jce_editor_pstate_flush_tick(0.49f);
    CHECK_FALSE(jce_fs_host_exists_file(state.c_str()));
    jce_editor_pstate_flush_tick(0.02f); /* No setter: same failed write retries. */
    require_state(state, 7);

    INFO("independent ordinary forced-write failure and immediate recovery");
    jce_editor_pstate_set_int("view_mode", 9);
    CHECK(jce_editor_pstate_flush_now());
    require_state(state, 9);
    const auto forced_before = project.path("forced-before.json");
    REQUIRE(jce_fs_host_rename(state.c_str(), forced_before.c_str()));
    REQUIRE(jce_fs_host_create_directory(state.c_str()));
    jce_editor_pstate_set_int("view_mode", 11);
    CHECK_FALSE(jce_editor_pstate_flush_now());
    CHECK(jce_fs_host_exists_dir(state.c_str()));
    require_state(forced_before, 9);
    remove_owned_blocker(project, state);
    jce_editor_pstate_flush_tick(0.01f);
    CHECK_FALSE(jce_fs_host_exists_file(state.c_str()));
    CHECK(jce_editor_pstate_flush_now()); /* No setter: retry retained dirty state. */
    require_state(state, 11);
}

TEST_CASE("actual root switch refuses failed outgoing publication and keeps A and B isolated")
{
    Project outgoing;
    const auto state_a = outgoing.path(".jce/editor-state.json");
    jce_editor_pstate_set_int("view_mode", 3);
    jce_editor_pstate_set_str("bookmark_label", "project state");
    jce_editor_pstate_scene_set_int("scenes/demo.scene", "grid", 3);
    REQUIRE(jce_editor_pstate_flush_now());

    JceTestOwnedDirectory incoming{"_ut_editor_pstate_incoming"};
    const auto state_b = incoming.root + "/.jce/editor-state.json";
    REQUIRE(jce_editor_current_project_root_set(incoming.root.c_str()));
    jce_editor_pstate_set_int("view_mode", 23);
    jce_editor_pstate_set_str("bookmark_label", "project state");
    jce_editor_pstate_scene_set_int("scenes/demo.scene", "grid", 3);
    REQUIRE(jce_editor_pstate_flush_now());
    REQUIRE(jce_editor_current_project_root_set(outgoing.root.c_str()));
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 3);

    const auto before = outgoing.path("switch-before.json");
    REQUIRE(jce_fs_host_rename(state_a.c_str(), before.c_str()));
    REQUIRE(jce_fs_host_create_directory(state_a.c_str()));
    jce_editor_pstate_set_int("view_mode", 7);
    INFO("A publication is blocked by an owned directory; no B identity may commit");
    CHECK_FALSE(jce_editor_current_project_root_set(incoming.root.c_str()));
    CHECK(std::strcmp(jce_editor_current_project_root(), outgoing.root.c_str()) == 0);
    CHECK(jce_editor_pstate_active());
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 7);
    require_state(before, 3);
    require_state(state_b, 23);
    CHECK_FALSE(jce_editor_current_project_root_set(incoming.root.c_str()));
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 7);

    INFO("same-root and oversized requests do not discard or alias the pending table");
    CHECK(jce_editor_current_project_root_set(jce_editor_current_project_root()));
    CHECK_FALSE(jce_editor_current_project_root_set(std::string(512, 'x').c_str()));
    CHECK(std::strcmp(jce_editor_current_project_root(), outgoing.root.c_str()) == 0);
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 7);

    remove_owned_blocker(outgoing, state_a);
    REQUIRE(jce_editor_current_project_root_set(incoming.root.c_str()));
    require_state(state_a, 7); /* The same pending values, no new setter. */
    require_state(state_b, 23);
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 23);
    REQUIRE(jce_editor_current_project_root_set(outgoing.root.c_str()));
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 7);
}

TEST_CASE("actual project close refuses failed publication then retries without losing identity")
{
    Project project;
    const auto state = project.path(".jce/editor-state.json");
    jce_editor_pstate_set_int("view_mode", 9);
    jce_editor_pstate_set_str("bookmark_label", "project state");
    jce_editor_pstate_scene_set_int("scenes/demo.scene", "grid", 3);
    REQUIRE(jce_editor_pstate_flush_now());
    const auto before = project.path("close-before.json");
    REQUIRE(jce_fs_host_rename(state.c_str(), before.c_str()));
    REQUIRE(jce_fs_host_create_directory(state.c_str()));
    jce_editor_pstate_set_int("view_mode", 11);
    CHECK_FALSE(jce_editor_current_project_root_set(nullptr));
    CHECK_FALSE(jce_editor_current_project_root_set(""));
    CHECK(std::strcmp(jce_editor_current_project_root(), project.root.c_str()) == 0);
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 11);
    require_state(before, 9);
    remove_owned_blocker(project, state);
    REQUIRE(jce_editor_current_project_root_set(nullptr));
    CHECK(jce_editor_current_project_root()[0] == '\0');
    CHECK_FALSE(jce_editor_pstate_active());
    require_state(state, 11);
    REQUIRE(jce_editor_current_project_root_set(project.root.c_str()));
    CHECK(jce_editor_pstate_get_int("view_mode", -1) == 11);
}
