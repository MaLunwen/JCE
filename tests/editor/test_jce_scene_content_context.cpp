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
