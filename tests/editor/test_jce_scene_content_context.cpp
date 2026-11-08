#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "scene/jce_scene_content_context.h"

TEST_CASE("isolated bundle context never inherits stale host project paths")
{
    const JceEditorSceneContentPaths paths =
        jce_editor_scene_content_paths(
            true, "D:/old/caged_kingdom", "resources/assets",
            "resources/_cooked");

    CHECK(paths.isolated);
    CHECK(paths.render_settings_primary == "render_settings.json");
    CHECK(paths.render_settings_fallback.empty());
    CHECK(paths.particle_asset_root.empty());
}

TEST_CASE("source context uses manifest asset roots")
{
    const JceEditorSceneContentPaths paths =
        jce_editor_scene_content_paths(
            false, "D:/projects/elemental", "content/source", "content/cooked");

    CHECK_FALSE(paths.isolated);
    CHECK(paths.render_settings_primary ==
          "D:/projects/elemental/content/source/render_settings.json");
    CHECK(paths.render_settings_fallback ==
          "D:/projects/elemental/content/cooked/render_settings.json");
    CHECK(paths.particle_asset_root ==
          "D:/projects/elemental/content/source");
}

TEST_CASE("loose content is independent of project layout and isolated Bundle paths")
{
    const auto loose = jce_editor_scene_content_paths(
        false, nullptr, nullptr, nullptr, "D:/loose/content");
    CHECK(loose.particle_asset_root == "D:/loose/content");
    CHECK(loose.render_settings_primary == "D:/loose/content/render_settings.json");
    CHECK(loose.render_settings_fallback.empty());
    const auto manifest = jce_editor_scene_content_paths(false, "D:/project",
        "author", "cooked", "D:/loose/content");
    CHECK(manifest.particle_asset_root == "D:/project/author");
    const auto isolated = jce_editor_scene_content_paths(true, "D:/project",
        "author", "cooked", "D:/loose/content");
    CHECK(isolated.particle_asset_root.empty());
    CHECK(isolated.render_settings_primary == "render_settings.json");
}
