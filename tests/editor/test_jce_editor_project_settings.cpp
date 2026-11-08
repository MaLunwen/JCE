/*
 * test_jce_editor_project_settings.cpp — defaults + cached snapshot.
 *
 * Persistence (load/save) round-trips against `.jce/project-settings.json`
 * which would pollute the working directory; we skip those paths and
 * cover the pure-memory contracts: defaults(), apply(), current().
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_project_settings.h"
#include <jce/os/core/jce_defs.h>

#include <cstring>

TEST_CASE("project_settings defaults populate sane top-level values")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);

    /* Audio */
    CHECK(s.audio.master_volume == doctest::Approx(1.0f));
    CHECK(s.audio.doppler_factor == doctest::Approx(1.0f));
    CHECK(s.audio.sample_rate == 48000);
    CHECK(s.audio.pause_on_focus_loss == true);
    CHECK(s.audio.disable_audio == false);

    /* Editor */
    CHECK(s.editor.default_behavior_mode == 0);
    CHECK(s.editor.version_control_mode == 1);

    /* Graphics */
    /* 1 = Linear.  The default is not decoration: it is what
     * jce_texture_set_colour_space is handed, and it decides both the hardware
     * sRGB decode on albedo/emissive and the output encode exponent.  There is
     * no srgb_write beside it any more -- it was the encode half of this same
     * decision and could not hold a value this one did not imply. */
    CHECK(s.graphics.color_space == 1);
    CHECK(s.graphics.hdr == true);
    CHECK(s.graphics.default_msaa == 4);
    CHECK(s.graphics.anisotropic_textures == 1);

    /* Input */
    CHECK(s.input.dead_zone == doctest::Approx(0.19f));
    CHECK(s.input.gravity == doctest::Approx(3.0f));
    CHECK(s.input.sensitivity == doctest::Approx(1.0f));
    CHECK(s.input.enable_gamepad == true);
    CHECK(s.input.treat_keyboard_as_dpad == false);
}

TEST_CASE("project_settings defaults configure physics gravity and full collision matrix")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);

    CHECK(s.physics.gravity[0] == doctest::Approx(0.0f));
    CHECK(s.physics.gravity[1] == doctest::Approx(-9.81f));
    CHECK(s.physics.gravity[2] == doctest::Approx(0.0f));
    CHECK(s.physics.default_solver_iterations == 6);
    CHECK(s.physics.queries_hit_triggers == true);
    CHECK(s.physics.auto_simulation == true);
    for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
        CHECK(s.physics.layer_collision_matrix[i] == 0xFFFFFFFFu);

    CHECK(s.physics2d.gravity[1] == doctest::Approx(-9.81f));
    CHECK(s.physics2d.velocity_iterations == 8);
    CHECK(s.physics2d.position_iterations == 3);
    CHECK(s.physics2d.auto_simulation == true);
    CHECK(s.physics2d.auto_sync_transforms == false);
    for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
        CHECK(s.physics2d.layer_collision_matrix[i] == 0xFFFFFFFFu);
}

TEST_CASE("project_settings defaults populate player identity and resolution")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);

    CHECK(std::strcmp(s.player.company_name, "DefaultCompany") == 0);
    CHECK(std::strcmp(s.player.product_name, "JCEProject") == 0);
    CHECK(std::strcmp(s.player.version, "0.1.0") == 0);
    CHECK(s.player.default_screen_width == 1280);
    CHECK(s.player.default_screen_height == 720);
    CHECK(s.player.show_splash == true);
    CHECK(s.player.fullscreen_default == false);
}

TEST_CASE("project_settings defaults seed three quality levels with current=High")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);

    REQUIRE(s.quality.count == 3);
    CHECK(s.quality.current_level == 2);
    CHECK(std::strcmp(s.quality.levels[0].name, "Low") == 0);
    CHECK(std::strcmp(s.quality.levels[1].name, "Medium") == 0);
    CHECK(std::strcmp(s.quality.levels[2].name, "High") == 0);
    CHECK(s.quality.levels[0].anti_aliasing == 0);
    CHECK(s.quality.levels[2].anti_aliasing == 4);
    CHECK(s.quality.levels[2].soft_particles == true);
    CHECK(s.quality.levels[0].soft_particles == false);
}

TEST_CASE("project_settings defaults seed builtin tags, sorting layers, and layer names")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);

    REQUIRE(s.tags_layers.tag_count == 7);
    CHECK(std::strcmp(s.tags_layers.tags[0], "Untagged") == 0);
    CHECK(std::strcmp(s.tags_layers.tags[6], "GameController") == 0);

    CHECK(s.tags_layers.sorting_layer_count == 1);
    CHECK(std::strcmp(s.tags_layers.sorting_layers[0], "Default") == 0);

    CHECK(std::strcmp(s.tags_layers.layers[0], "Default") == 0);
    CHECK(std::strcmp(s.tags_layers.layers[1], "TransparentFX") == 0);
    CHECK(std::strcmp(s.tags_layers.layers[2], "IgnoreRaycast") == 0);
    CHECK(std::strcmp(s.tags_layers.layers[4], "Water") == 0);
    CHECK(std::strcmp(s.tags_layers.layers[5], "UI") == 0);
    /* Slot 3 is intentionally empty (memset zero). */
    CHECK(s.tags_layers.layers[3][0] == '\0');
}

TEST_CASE("project_settings defaults populate Time and Rendering blocks")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);

    CHECK(s.time.fixed_timestep == doctest::Approx(0.02f));
    CHECK(s.time.max_allowed_timestep == doctest::Approx(0.3333f));
    CHECK(s.time.time_scale == doctest::Approx(1.0f));
    CHECK(s.time.maximum_particle_timestep_ms == 30);

    CHECK(s.rendering.exposure == doctest::Approx(1.0f));
    CHECK(s.rendering.gamma == doctest::Approx(2.2f));
    CHECK(s.rendering.bloom_threshold == doctest::Approx(1.0f));
    CHECK(s.rendering.fxaa_span_max == doctest::Approx(8.0f));
    CHECK(s.rendering.fog_enabled == false);
    CHECK(s.rendering.fog_density == doctest::Approx(0.02f));
    CHECK(s.rendering.ambient_intensity == doctest::Approx(1.0f));
    for (int i = 0; i < 6; i++)
        CHECK(s.rendering.postfx_enabled[i] == false);

    CHECK(s.presets.count == 0);
}

TEST_CASE("apply caches the snapshot for current(), overwrite replaces it")
{
    JceProjectSettings s;
    jce_project_settings_defaults(&s);
    s.audio.master_volume = 0.42f;
    s.player.default_screen_width = 4242;

    jce_project_settings_apply(&s);
    const JceProjectSettings *cur = jce_project_settings_current();
    REQUIRE(cur != nullptr);
    CHECK(cur->audio.master_volume == doctest::Approx(0.42f));
    CHECK(cur->player.default_screen_width == 4242);

    JceProjectSettings s2;
    jce_project_settings_defaults(&s2);
    s2.audio.master_volume = 0.99f;
    jce_project_settings_apply(&s2);

    cur = jce_project_settings_current();
    REQUIRE(cur != nullptr);
    CHECK(cur->audio.master_volume == doctest::Approx(0.99f));
    CHECK(cur->player.default_screen_width == 1280); /* reset by defaults() */
}


TEST_CASE("project root changes retire the cached settings snapshot")
{
    jce_project_settings_set_root("_ut_project_settings_root_a");
    JceProjectSettings settings;
    jce_project_settings_defaults(&settings);
    settings.audio.master_volume = 0.23f;
    settings.time.fixed_timestep = 0.125f;
    settings.physics.gravity[1] = -2.5f;
    jce_project_settings_apply(&settings);
    REQUIRE(jce_project_settings_current() != nullptr);

    jce_project_settings_set_root("_ut_project_settings_root_b");
    CHECK(jce_project_settings_current() == nullptr);

    /* Selecting A again cannot resurrect its old process-wide snapshot. */
    jce_project_settings_set_root("_ut_project_settings_root_a");
    CHECK(jce_project_settings_current() == nullptr);

    settings.audio.master_volume = 0.76f;
    jce_project_settings_apply(&settings);
    REQUIRE(jce_project_settings_current() != nullptr);
    CHECK(jce_project_settings_current()->audio.master_volume == doctest::Approx(0.76f));
    jce_project_settings_set_root(nullptr);
}

TEST_CASE("selecting the same normalized project root preserves edited settings")
{
    jce_project_settings_set_root("_ut_project_settings_same_root");
    JceProjectSettings settings;
    jce_project_settings_defaults(&settings);
    settings.audio.master_volume = 0.37f;
    settings.time.fixed_timestep = 0.0625f;
    jce_project_settings_apply(&settings);
    const JceProjectSettings *before = jce_project_settings_current();
    REQUIRE(before != nullptr);

    jce_project_settings_set_root("_ut_project_settings_same_root");
    const JceProjectSettings *after = jce_project_settings_current();
    REQUIRE(after != nullptr);
    CHECK(after == before);
    CHECK(after->audio.master_volume == doctest::Approx(0.37f));
    CHECK(after->time.fixed_timestep == doctest::Approx(0.0625f));

#if JCE_PLATFORM_WINDOWS
    /* Match the editor's identity rule for already-normalized Windows roots. */
    jce_project_settings_set_root("_UT_PROJECT_SETTINGS_SAME_ROOT");
    after = jce_project_settings_current();
    REQUIRE(after != nullptr);
    CHECK(after->audio.master_volume == doctest::Approx(0.37f));
#endif
    jce_project_settings_set_root(nullptr);
}

TEST_CASE("closing a project clears its settings without loading another root")
{
    JceProjectSettings settings;
    jce_project_settings_defaults(&settings);
    settings.audio.disable_audio = true;
    settings.time.fixed_timestep = 0.125f;
    jce_project_settings_set_root("_ut_project_settings_close");
    jce_project_settings_apply(&settings);
    REQUIRE(jce_project_settings_current() != nullptr);

    SUBCASE("null closes the explicit root")
    {
        jce_project_settings_set_root(nullptr);
        CHECK(jce_project_settings_current() == nullptr);
        jce_project_settings_set_root("");
        CHECK(jce_project_settings_current() == nullptr);
    }
    SUBCASE("empty closes the explicit root")
    {
        jce_project_settings_set_root("");
        CHECK(jce_project_settings_current() == nullptr);
        jce_project_settings_set_root(nullptr);
        CHECK(jce_project_settings_current() == nullptr);
    }

    /* No-project is one identity. Explicit apply remains a supported operation;
     * the setter must not implicitly read CWD or discard same-owner edits. */
    jce_project_settings_defaults(&settings);
    settings.audio.master_volume = 0.63f;
    jce_project_settings_apply(&settings);
    jce_project_settings_set_root(nullptr);
    jce_project_settings_set_root("");
    REQUIRE(jce_project_settings_current() != nullptr);
    CHECK(jce_project_settings_current()->audio.master_volume == doctest::Approx(0.63f));
    jce_project_settings_set_root("_ut_project_settings_new_owner");
    CHECK(jce_project_settings_current() == nullptr);
    jce_project_settings_set_root(nullptr);
}
