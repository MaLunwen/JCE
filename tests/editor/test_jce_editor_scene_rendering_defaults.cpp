#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_scene_rendering_defaults.h"
#include "core/jce_editor_project.h"
#include "core/jce_project_settings.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

extern "C" const JceProject *jce_editor_project_get(void)
{
    return nullptr;
}

TEST_CASE("project rendering defaults seed new scenes without overwriting authored scenes")
{
    JceProjectSettings ps;
    jce_project_settings_defaults(&ps);
    ps.rendering.ambient_color[0] = 0.25f;
    ps.rendering.ambient_color[1] = 0.35f;
    ps.rendering.ambient_color[2] = 0.45f;
    ps.rendering.fog_enabled = true;
    ps.rendering.fog_density = 0.08f;
    ps.rendering.postfx_enabled[1] = true;
    ps.rendering.bloom_intensity = 1.25f;
    ps.quality.current_level = 2;
    ps.quality.levels[2].shadow_distance = 321.0f;
    ps.quality.levels[2].shadow_resolution = 3;
    ps.quality.levels[2].shadow_cascades = 4;
    jce_project_settings_apply(&ps);

    JceScene *scene = jce_scene_create();
    REQUIRE(scene != nullptr);
    CHECK_FALSE(jce_scene_has_rendering_settings(scene));

    jce_editor_scene_ensure_rendering_settings(scene);
    const JceSceneRenderingSettings *r =
        jce_scene_get_rendering_settings(scene);
    REQUIRE(r != nullptr);
    CHECK(r->ambient_color[0] == doctest::Approx(0.25f));
    CHECK(r->ambient_color[1] == doctest::Approx(0.35f));
    CHECK(r->ambient_color[2] == doctest::Approx(0.45f));
    CHECK(r->fog_enabled == true);
    CHECK(r->fog_mode == JCE_SCENE_FOG_EXP);
    CHECK(r->fog_density == doctest::Approx(0.08f));
    CHECK(r->postfx_enabled[1] == true);
    CHECK(r->bloom_intensity == doctest::Approx(1.25f));
    CHECK(r->shadow_distance == doctest::Approx(321.0f));
    CHECK(r->shadow_resolution == 4096);
    CHECK(r->cascade_count == 4);

    JceSceneRenderingSettings authored =
        jce_scene_rendering_settings_default();
    authored.ambient_color[0] = 0.91f;
    authored.shadow_distance = 42.0f;
    jce_scene_set_rendering_settings(scene, &authored);

    ps.rendering.ambient_color[0] = 0.11f;
    ps.quality.levels[2].shadow_distance = 999.0f;
    jce_project_settings_apply(&ps);
    jce_editor_scene_ensure_rendering_settings(scene);
    r = jce_scene_get_rendering_settings(scene);
    REQUIRE(r != nullptr);
    CHECK(r->ambient_color[0] == doctest::Approx(0.91f));
    CHECK(r->shadow_distance == doctest::Approx(42.0f));

    jce_scene_destroy(scene);
}
