/*
 * test_jce_editor_config.cpp — pure-logic coverage for editor config.
 *
 * We avoid touching the real .jce/editor-config.json on disk; only the
 * defaults() / add_recent() / add_recent_scene() helpers are exercised.
 * They are 100% memory-resident and have no I/O.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_config.h"

extern "C" {
#include <jce/os/core/jce_defs.h>
}

#include <cstring>

TEST_CASE("config defaults populate sane values")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    CHECK(std::strcmp(cfg.language, "en") == 0);
    CHECK(cfg.font_size == 24);
    CHECK(cfg.ui_scale == doctest::Approx(1.0f));
    CHECK(std::strcmp(cfg.theme, "Dark") == 0);
#if JCE_PLATFORM_WINDOWS
    CHECK(std::strcmp(cfg.renderer, "D3D11") == 0);
#else
    CHECK(std::strcmp(cfg.renderer, "Auto") == 0);
#endif
    CHECK(cfg.last_project[0] == '\0');
    CHECK(cfg.last_scene_path[0] == '\0');
    CHECK(cfg.recent_count == 0);
    CHECK(cfg.recent_scene_count == 0);
    CHECK(cfg.autosave_interval == 0);
    CHECK(cfg.startup_mode == 0);
    CHECK(cfg.recent_max == 10);
    CHECK(cfg.view_mode == 0);
    CHECK(cfg.show_grid == true);
    CHECK(cfg.asset_browser_view_mode == 0);
    CHECK(cfg.run_mode == 0);
    /* The shipped defaults must not name any one game: which binary Run
     * launches belongs to the opened project (jce_project.json's
     * target_name, or Project Settings > Build > CMake Target).  Until
     * 2026-08-27 these two fields read "CagedKingdom" /
     * "...caged_kingdom.exe", so a fresh editor proposed to run somebody
     * else's build -- and this test asserted that it did. */
    CHECK(cfg.game_target_name[0] == '\0');
    CHECK(cfg.game_executable_path[0] == '\0');
    /* The working directory stays a generic preset output dir -- it names
     * no project, so it is still a useful default. */
    CHECK(cfg.game_working_directory[0] != '\0');
    CHECK(std::strstr(cfg.game_working_directory, "caged_kingdom") == nullptr);
    CHECK(cfg.invert_scroll_zoom == false);
    CHECK(cfg.invert_drag_y == false);
    CHECK(cfg.touchpad_h_invert == true);
    CHECK(cfg.panels_visible_mask == JCE_EDITOR_PANELS_MASK_UNSET);
    CHECK(cfg.panels_visible_mask_hi == 0u);
    CHECK(cfg.ui_int_state_count == 0);
}

TEST_CASE("ui int state stores and updates stable keys")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    CHECK(jce_editor_config_get_ui_int_or(&cfg,
                                          "panel.animation.current_tab",
                                          7) == 7);

    CHECK(jce_editor_config_set_ui_int(&cfg,
                                       "panel.animation.current_tab",
                                       4));
    CHECK(cfg.ui_int_state_count == 1);
    CHECK(jce_editor_config_get_ui_int_or(&cfg,
                                          "panel.animation.current_tab",
                                          0) == 4);

    CHECK(jce_editor_config_set_ui_int(&cfg,
                                       "panel.animation.current_tab",
                                       2));
    CHECK(cfg.ui_int_state_count == 1);
    CHECK(jce_editor_config_get_ui_int_or(&cfg,
                                          "panel.animation.current_tab",
                                          0) == 2);
}

TEST_CASE("ui int state rejects empty keys and caps storage")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    CHECK_FALSE(jce_editor_config_set_ui_int(&cfg, nullptr, 1));
    CHECK_FALSE(jce_editor_config_set_ui_int(&cfg, "", 1));

    for (int i = 0; i < JCE_EDITOR_UI_INT_STATE_MAX; i++) {
        char key[64];
        std::snprintf(key, sizeof(key), "panel.test.%02d", i);
        CHECK(jce_editor_config_set_ui_int(&cfg, key, i));
    }

    CHECK(cfg.ui_int_state_count == JCE_EDITOR_UI_INT_STATE_MAX);
    CHECK_FALSE(jce_editor_config_set_ui_int(&cfg,
                                             "panel.test.overflow",
                                             99));
}

TEST_CASE("add_recent ignores null/empty input")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent(&cfg, nullptr);
    CHECK(cfg.recent_count == 0);
    jce_editor_config_add_recent(&cfg, "");
    CHECK(cfg.recent_count == 0);
}

TEST_CASE("add_recent prepends in MRU order")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent(&cfg, "/proj/a");
    jce_editor_config_add_recent(&cfg, "/proj/b");
    jce_editor_config_add_recent(&cfg, "/proj/c");

    REQUIRE(cfg.recent_count == 3);
    CHECK(std::strcmp(cfg.recent_projects[0], "/proj/c") == 0);
    CHECK(std::strcmp(cfg.recent_projects[1], "/proj/b") == 0);
    CHECK(std::strcmp(cfg.recent_projects[2], "/proj/a") == 0);
}

TEST_CASE("add_recent moves duplicates to front without growing list")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent(&cfg, "/proj/a");
    jce_editor_config_add_recent(&cfg, "/proj/b");
    jce_editor_config_add_recent(&cfg, "/proj/c");
    jce_editor_config_add_recent(&cfg, "/proj/a"); /* dup → moves to front */

    REQUIRE(cfg.recent_count == 3);
    CHECK(std::strcmp(cfg.recent_projects[0], "/proj/a") == 0);
    CHECK(std::strcmp(cfg.recent_projects[1], "/proj/c") == 0);
    CHECK(std::strcmp(cfg.recent_projects[2], "/proj/b") == 0);
}

TEST_CASE("add_recent caps the list at 10 entries")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    for (int i = 0; i < 15; i++) {
        char path[32];
        std::snprintf(path, sizeof(path), "/proj/%d", i);
        jce_editor_config_add_recent(&cfg, path);
    }
    CHECK(cfg.recent_count == 10);
    /* Most recent push wins front; oldest (0..4) evicted. */
    CHECK(std::strcmp(cfg.recent_projects[0], "/proj/14") == 0);
    CHECK(std::strcmp(cfg.recent_projects[9], "/proj/5") == 0);
}

TEST_CASE("add_recent_scene rejects non-scene files")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent_scene(&cfg, nullptr);
    jce_editor_config_add_recent_scene(&cfg, "");
    jce_editor_config_add_recent_scene(&cfg, "/a/b/foo.txt");
    jce_editor_config_add_recent_scene(&cfg, "/a/b/foo.json"); /* not .scene.json */
    jce_editor_config_add_recent_scene(&cfg, "tiny");
    CHECK(cfg.recent_scene_count == 0);
}

TEST_CASE("add_recent_scene accepts .scene.json case-insensitively")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent_scene(&cfg, "/proj/x.scene.json");
    jce_editor_config_add_recent_scene(&cfg, "/proj/Y.SCENE.JSON");

    REQUIRE(cfg.recent_scene_count == 2);
    CHECK(std::strcmp(cfg.recent_scene_paths[0], "/proj/Y.SCENE.JSON") == 0);
    CHECK(std::strcmp(cfg.recent_scene_paths[1], "/proj/x.scene.json") == 0);
}

TEST_CASE("add_recent_scene normalizes backslashes to forward slashes")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent_scene(&cfg, "C:\\proj\\sub\\a.scene.json");
    REQUIRE(cfg.recent_scene_count == 1);
    CHECK(std::strcmp(cfg.recent_scene_paths[0],
                      "C:/proj/sub/a.scene.json") == 0);

    /* Same logical path with forward slashes should dedupe, not append. */
    jce_editor_config_add_recent_scene(&cfg, "C:/proj/sub/a.scene.json");
    CHECK(cfg.recent_scene_count == 1);
}

TEST_CASE("add_recent_scene caps at 10 and evicts oldest")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    for (int i = 0; i < 12; i++) {
        char path[64];
        std::snprintf(path, sizeof(path), "/p/scene_%02d.scene.json", i);
        jce_editor_config_add_recent_scene(&cfg, path);
    }
    CHECK(cfg.recent_scene_count == 10);
    CHECK(std::strcmp(cfg.recent_scene_paths[0],
                      "/p/scene_11.scene.json") == 0);
    CHECK(std::strcmp(cfg.recent_scene_paths[9],
                      "/p/scene_02.scene.json") == 0);
}

TEST_CASE("add_recent_scene dedup moves entry to front")
{
    JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);

    jce_editor_config_add_recent_scene(&cfg, "/a.scene.json");
    jce_editor_config_add_recent_scene(&cfg, "/b.scene.json");
    jce_editor_config_add_recent_scene(&cfg, "/c.scene.json");
    jce_editor_config_add_recent_scene(&cfg, "/a.scene.json");

    REQUIRE(cfg.recent_scene_count == 3);
    CHECK(std::strcmp(cfg.recent_scene_paths[0], "/a.scene.json") == 0);
    CHECK(std::strcmp(cfg.recent_scene_paths[1], "/c.scene.json") == 0);
    CHECK(std::strcmp(cfg.recent_scene_paths[2], "/b.scene.json") == 0);
}
