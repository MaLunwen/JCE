/* Actual project adapter and public project/JSON/host IO; no renderer or UI. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_project.h"
#include "jce_test_owned_directory.h"
#include <jce/os/core/jce_json.h>
#include <cstring>

namespace {
const char manifest[] =
    "{\"schema\":3,\"name\":\"initial\",\"version\":\"1.0\",\"target\":\"Initial\","
    "\"exe\":\"Initial.exe\",\"sdk\":\"sdk/old\",\"platform\":\"win\","
    "\"source_assets\":\"assets\",\"cooked_assets\":\"resources/_cooked\","
    "\"startup_scene\":\"scenes/start.scene\",\"bundles\":[\"dist/original.jbundle\"],"
    "\"native_script_factories\":[{\"symbol\":\"initial_factory\"}],"
    "\"native_script_refs\":{\"controller\":\"src/controller.c\"},"
    "\"future_settings\":{\"levels\":[1,true,null,{\"value\":\"retained\"}]}}";

struct Project {
    JceTestOwnedDirectory directory{"_ut_editor_project_persistence"};
    std::string path = directory.path("jce_project.json");
    Project()
    {
        REQUIRE(jce_fs_host_write_all(path.c_str(), manifest, sizeof(manifest) - 1));
        jce_editor_project_set_root(directory.root.c_str());
        REQUIRE(jce_editor_project_get() != nullptr);
    }
    ~Project() { jce_editor_project_set_root(nullptr); }
    std::string bytes() const
    {
        uint64_t size = 0;
        char *text = static_cast<char *>(jce_fs_host_read_all(path.c_str(), &size));
        REQUIRE(text != nullptr);
        std::string result(text, static_cast<size_t>(size));
        jce_fs_buffer_free(text);
        return result;
    }
};

void retained_metadata(const char *path)
{
    JceJson *root = jce_json_parse_file(path);
    REQUIRE(root != nullptr);
    CHECK(jce_json_is_array(jce_json_get(root, "native_script_factories")));
    CHECK(jce_json_is_object(jce_json_get(root, "native_script_refs")));
    JceJson *future = jce_json_get(root, "future_settings");
    REQUIRE(future != nullptr);
    CHECK(jce_json_array_size(jce_json_get(future, "levels")) == 4);
    jce_json_free(root);
}
}

TEST_CASE("SDK update preserves unknown authoring nodes and borrowed project root")
{
    Project project;
    const JceProject *before = jce_editor_project_get();
    const char *borrowed_root = before->project_root;
    const char *borrowed_name = before->name;
    REQUIRE(jce_editor_project_update_field("sdk_path", "sdk/current"));
    const JceProject *after = jce_editor_project_get();
    REQUIRE(after == before);
    CHECK(after->project_root == borrowed_root);
    CHECK(after->name == borrowed_name);
    CHECK(std::strcmp(borrowed_root, project.directory.root.c_str()) == 0);
    CHECK(std::strcmp(after->sdk_path, "sdk/current") == 0);
    JceProject *saved = jce_project_load(project.directory.root.c_str());
    REQUIRE(saved != nullptr);
    CHECK(std::strcmp(saved->sdk_path, "sdk/current") == 0);
    jce_project_free(saved);
    retained_metadata(project.path.c_str());
}

TEST_CASE("atomic publication failure preserves disk and every cached field")
{
    Project project;
    const auto original = project.bytes();
    const JceProject *before = jce_editor_project_get();
    const char *sdk = before->sdk_path;
    const char *root = before->project_root;
    REQUIRE(jce_fs_host_create_directory((project.path + ".tmp").c_str()));
    CHECK_FALSE(jce_editor_project_update_field("sdk_path", "sdk/rejected"));
    CHECK(jce_editor_project_get() == before);
    REQUIRE(before->sdk_path == sdk);
    CHECK(before->project_root == root);
    CHECK(std::strcmp(sdk, "sdk/old") == 0);
    CHECK(project.bytes() == original);
}

TEST_CASE("corrupt existing manifest cannot be silently replaced by an SDK update")
{
    Project project;
    const char corrupt[] = "{\"name\":\"broken\",";
    REQUIRE(jce_fs_host_write_all(project.path.c_str(), corrupt, sizeof(corrupt) - 1));
    const JceProject *before = jce_editor_project_get();
    const char *sdk = before->sdk_path;
    CHECK_FALSE(jce_editor_project_update_field("sdk_path", "sdk/rejected"));
    REQUIRE(jce_editor_project_get() == before);
    REQUIRE(before->sdk_path == sdk);
    CHECK(project.bytes() == corrupt);
}

TEST_CASE("bundle replacement and clear preserve unrelated metadata and cache identity")
{
    Project project;
    const JceProject *before = jce_editor_project_get();
    const char *root = before->project_root;
    const char *paths[] = {"dist/a.jbundle", "dist/b.jbundle"};
    REQUIRE(jce_editor_project_set_bundles(paths, 2));
    REQUIRE(jce_editor_project_get() == before);
    CHECK(before->project_root == root);
    REQUIRE(before->bundles_count == 2);
    CHECK(std::strcmp(before->bundles[1], paths[1]) == 0);
    retained_metadata(project.path.c_str());
    REQUIRE(jce_editor_project_set_bundles(nullptr, 0));
    CHECK(jce_editor_project_get() == before);
    CHECK(before->bundles_count == 0);
    CHECK(before->bundles == nullptr);
    JceJson *saved = jce_json_parse_file(project.path.c_str());
    REQUIRE(saved != nullptr);
    CHECK_FALSE(jce_json_has(saved, "bundles"));
    jce_json_free(saved);
    retained_metadata(project.path.c_str());
}

TEST_CASE("failed bundle publication retains old cached array and source bytes")
{
    Project project;
    const auto original = project.bytes();
    const JceProject *before = jce_editor_project_get();
    char **bundles = before->bundles;
    REQUIRE(jce_fs_host_create_directory((project.path + ".tmp").c_str()));
    const char *paths[] = {"dist/rejected.jbundle"};
    CHECK_FALSE(jce_editor_project_set_bundles(paths, 1));
    CHECK(jce_editor_project_get() == before);
    CHECK(before->bundles == bundles);
    REQUIRE(before->bundles_count == 1);
    CHECK(std::strcmp(before->bundles[0], "dist/original.jbundle") == 0);
    CHECK(project.bytes() == original);
}

TEST_CASE("invalid field and missing project refuse without writes")
{
    Project project;
    const auto original = project.bytes();
    const JceProject *before = jce_editor_project_get();
    CHECK_FALSE(jce_editor_project_update_field("not_a_project_field", "value"));
    CHECK(jce_editor_project_get() == before);
    CHECK(project.bytes() == original);
    jce_editor_project_set_root(nullptr);
    CHECK_FALSE(jce_editor_project_update_field("sdk_path", "sdk/rejected"));
    CHECK_FALSE(jce_editor_project_set_bundles(nullptr, 0));
    CHECK(project.bytes() == original);
}


TEST_CASE("external known field changes refuse an unrelated SDK save without altering disk or cache")
{
    Project project;
    const JceProject *before = jce_editor_project_get();
    const char *sdk = before->sdk_path;
    JceJson *edited = jce_json_parse_file(project.path.c_str());
    REQUIRE(edited != nullptr);
    jce_json_remove(edited, "startup_scene");
    jce_json_set_string(edited, "startup_scene", "scenes/externally-edited.scene");
    REQUIRE(jce_json_write_file(project.path.c_str(), edited, true, true));
    const auto external = project.bytes();
    CHECK_FALSE(jce_editor_project_update_field("sdk_path", "sdk/rejected"));
    CHECK(jce_editor_project_get() == before);
    CHECK(before->sdk_path == sdk);
    CHECK(project.bytes() == external);
}

TEST_CASE("external Bundle-list changes refuse both SDK and Bundle replacement until reload")
{
    Project project;
    const JceProject *before = jce_editor_project_get();
    char **bundles = before->bundles;
    JceJson *edited = jce_json_parse_file(project.path.c_str());
    REQUIRE(edited != nullptr);
    jce_json_remove(edited, "bundles");
    JceJson *array = jce_json_array();
    REQUIRE(array != nullptr);
    jce_json_array_push_string(array, "dist/external.jbundle");
    jce_json_set_child(edited, "bundles", array);
    REQUIRE(jce_json_write_file(project.path.c_str(), edited, true, true));
    const auto external = project.bytes();
    const char *requested[] = {"dist/rejected.jbundle"};
    CHECK_FALSE(jce_editor_project_update_field("sdk_path", "sdk/rejected"));
    CHECK_FALSE(jce_editor_project_set_bundles(requested, 1));
    CHECK(jce_editor_project_get() == before);
    CHECK(before->bundles == bundles);
    CHECK(project.bytes() == external);
    jce_editor_project_set_root(project.directory.root.c_str());
    REQUIRE(jce_editor_project_get() != nullptr);
    REQUIRE(jce_editor_project_set_bundles(requested, 1));
    REQUIRE(jce_editor_project_get()->bundles_count == 1);
    CHECK(std::strcmp(jce_editor_project_get()->bundles[0], requested[0]) == 0);
}

TEST_CASE("external unknown extensions and pure JSON formatting remain safely editable")
{
    Project project;
    JceJson *edited = jce_json_parse_file(project.path.c_str());
    REQUIRE(edited != nullptr);
    JceJson *future = jce_json_get(edited, "future_settings");
    REQUIRE(future != nullptr);
    jce_json_set_bool(future, "external_note", true);
    REQUIRE(jce_json_write_file(project.path.c_str(), edited, false, true));
    REQUIRE(jce_fs_host_append(project.path.c_str(), " \n\t", 3));
    REQUIRE(jce_editor_project_update_field("sdk_path", "sdk/current"));
    edited = jce_json_parse_file(project.path.c_str());
    REQUIRE(edited != nullptr);
    future = jce_json_get(edited, "future_settings");
    REQUIRE(future != nullptr);
    CHECK(jce_json_get_bool(future, "external_note", false));
    CHECK(jce_json_array_size(jce_json_get(future, "levels")) == 4);
    jce_json_free(edited);
    REQUIRE(jce_editor_project_update_field("sdk_path", "sdk/second"));
    retained_metadata(project.path.c_str());
}

TEST_CASE("an intentional clear with loader defaults does not create a false source conflict")
{
    Project project;
    REQUIRE(jce_editor_project_update_field("version", ""));
    REQUIRE(jce_editor_project_update_field("sdk_path", "sdk/after-clear"));
    const JceProject *current = jce_editor_project_get();
    REQUIRE(current != nullptr);
    CHECK(current->version == nullptr);
    CHECK(std::strcmp(current->sdk_path, "sdk/after-clear") == 0);
    JceProject *disk = jce_project_load(project.directory.root.c_str());
    REQUIRE(disk != nullptr);
    CHECK(std::strcmp(disk->version, "0.1.0") == 0);
    jce_project_free(disk);
}
