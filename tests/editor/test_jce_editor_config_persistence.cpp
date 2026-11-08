/* Actual config/category writer and host filesystem; no Editor window. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_path.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void set_profile(const char *value)
{
#if JCE_PLATFORM_WINDOWS
    REQUIRE(_putenv_s("JCE_EDITOR_PROFILE_DIR", value ? value : "") == 0);
#else
    REQUIRE((value ? setenv("JCE_EDITOR_PROFILE_DIR", value, 1)
                   : unsetenv("JCE_EDITOR_PROFILE_DIR")) == 0);
#endif
}
struct Profile {
    JceTestOwnedDirectory directory{"_ut_editor_config_persistence"};
    std::string root = directory.root, previous;
    bool had_previous;
    Profile()
    {
        const char *old = std::getenv("JCE_EDITOR_PROFILE_DIR");
        had_previous = old != nullptr;
        previous = old ? old : "";
        set_profile(root.c_str());
    }
    std::string path(const char *name) const { return root + "/" + name; }
    ~Profile()
    {
        jce_editor_config_flush_now();
        set_profile(had_previous ? previous.c_str() : nullptr);
    }
};
struct JsonFile {
    JceJson *root;
    explicit JsonFile(const std::string &path) : root(jce_json_parse_file(path.c_str()))
    { REQUIRE(root != nullptr); }
    ~JsonFile() { jce_json_free(root); }
};
void require_mask(const std::string &path, uint32_t low, uint32_t high)
{
    JceJson *root = jce_json_parse_file(path.c_str());
    CHECK(root != nullptr);
    if (!root) return; /* Keep both failed-publication negative witnesses reachable. */
    CHECK(static_cast<uint32_t>(jce_json_get_int(root, "panels_visible_mask", 0)) == low);
    CHECK(static_cast<uint32_t>(jce_json_get_int(root, "panels_visible_mask_hi", 0)) == high);
    jce_json_free(root);
}
}

TEST_CASE("actual config persists categories and retries failed debounced writes")
{
    /* Config is process-owned. One ordered lifecycle avoids a fake reset or
       another loader; each readback uses the real public JSON/file owner. */
    Profile profile;
    const auto legacy = profile.path("editor-config.json");
    const auto prefs = profile.path("editor-preferences.json");
    const auto session = profile.path("editor-session.json");
    const char initial[] =
        "{\"language\":\"zh_cn\",\"theme\":\"Light\",\"font_size\":19,"
        "\"panels_visible_mask\":-2147483647,\"panels_visible_mask_hi\":1073741825,"
        "\"ui_state_int\":{\"panels.visible_count\":62}}";
    REQUIRE(jce_fs_host_write_all(legacy.c_str(), initial, sizeof(initial) - 1));
    JceEditorConfig config;

    INFO("control 1: actual legacy loader retains signed low/high visibility bits");
    REQUIRE(jce_editor_config_load(&config));
    CHECK(std::strcmp(config.language, "zh_cn") == 0);
    CHECK(config.font_size == 19);
    CHECK(config.panels_visible_mask == UINT32_C(0x80000001));
    CHECK(config.panels_visible_mask_hi == UINT32_C(0x40000001));
    CHECK(jce_editor_config_get_ui_int_or(&config, "panels.visible_count", -1) == 62);

    INFO("control 2: quiet-period debounce does not publish early");
    config.font_size = 22;
    config.panels_visible_mask_hi = UINT32_C(0x80000001);
    const uint64_t before = jce_editor_config_generation();
    REQUIRE(jce_editor_config_save(&config));
    CHECK(jce_editor_config_generation() == before + 1);
    jce_editor_config_flush_tick(-1.0f);
    jce_editor_config_flush_tick(0.49f);
    CHECK_FALSE(jce_fs_host_exists_file(prefs.c_str()));
    CHECK_FALSE(jce_fs_host_exists_file(session.c_str()));
    jce_editor_config_flush_tick(0.02f);

    INFO("control 3: real category files and migration retirement agree");
    {
        JsonFile file(prefs);
        CHECK(jce_json_get_int(file.root, "font_size", -1) == 22);
        CHECK(std::strcmp(jce_json_get_string(file.root, "language", ""), "zh_cn") == 0);
        CHECK_FALSE(jce_json_has(file.root, "panels_visible_mask"));
    }
    require_mask(session, UINT32_C(0x80000001), UINT32_C(0x80000001));
    CHECK_FALSE(jce_fs_host_exists_file(legacy.c_str()));
    CHECK(jce_fs_host_exists_file((legacy + ".migrated").c_str()));
    {
        JsonFile file(session);
        CHECK_FALSE(jce_json_has(file.root, "font_size"));
        CHECK(jce_json_get_int(jce_json_get(file.root, "ui_state_int"),
                               "panels.visible_count", -1) == 62);
    }

    INFO("control 4: multiple edits coalesce to the last live state");
    config.font_size = 23;
    REQUIRE(jce_editor_config_save(&config));
    config.font_size = 24;
    REQUIRE(jce_editor_config_save(&config));
    jce_editor_config_flush_tick(0.49f);
    { JsonFile file(prefs); CHECK(jce_json_get_int(file.root, "font_size", -1) == 22); }
    jce_editor_config_flush_tick(0.02f);
    { JsonFile file(prefs); CHECK(jce_json_get_int(file.root, "font_size", -1) == 24); }

    INFO("control 5: failed timed flush preserves pending write until recovery");
    const auto saved_session = profile.path("session-before-failure.json");
    REQUIRE(jce_fs_host_rename(session.c_str(), saved_session.c_str()));
    REQUIRE(jce_fs_host_create_directory(session.c_str()));
    config.panels_visible_mask = UINT32_C(0x10000000);
    config.panels_visible_mask_hi = UINT32_C(0x20000000);
    REQUIRE(jce_editor_config_save(&config));
    jce_editor_config_flush_tick(0.51f);
    CHECK(jce_fs_host_exists_dir(session.c_str()));
    require_mask(saved_session, UINT32_C(0x80000001), UINT32_C(0x80000001));
    REQUIRE(jce_fs_host_remove_recursive(session.c_str()));
    /* No additional save: failure recovery must retry the same dirty state. */
    jce_editor_config_flush_tick(0.51f);
    require_mask(session, UINT32_C(0x10000000), UINT32_C(0x20000000));

    INFO("control 6: failed forced shutdown-style flush also remains retryable");
    REQUIRE(jce_fs_host_remove_file(session.c_str()));
    REQUIRE(jce_fs_host_create_directory(session.c_str()));
    config.panels_visible_mask_hi = UINT32_C(0x60000000);
    REQUIRE(jce_editor_config_save(&config));
    jce_editor_config_flush_now();
    CHECK(jce_fs_host_exists_dir(session.c_str()));
    REQUIRE(jce_fs_host_remove_recursive(session.c_str()));
    jce_editor_config_flush_now();
    require_mask(session, UINT32_C(0x10000000), UINT32_C(0x60000000));
}
