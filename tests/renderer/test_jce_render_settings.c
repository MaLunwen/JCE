/* test_jce_render_settings.c
 *
 * Top 5: project-wide render/quality settings carrier.  The editor build emits
 * render_settings.json from Project Settings > Quality; the shipped runtime
 * (drop-in main) loads it and folds the shadow tier + lod bias into the scene
 * render config.  Covers default / JSON round-trip / missing-file fallback.
 */

#include <jce/renderer/jce_render_settings.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RS_PATH      "test_render_settings.json"
#define RS_PATH_LOOK "test_render_settings_look.json"
#define RS_PATH_V1   "test_render_settings_v1.json"

void setUp(void)    {}
void tearDown(void) { remove(RS_PATH); remove(RS_PATH_LOOK); remove(RS_PATH_V1); }

static void test_defaults(void)
{
    JceRenderSettings d = jce_render_settings_default();
    TEST_ASSERT_EQUAL_INT(2, d.shadow_quality);   /* hard+soft */
    TEST_ASSERT_EQUAL_INT(1, d.vsync);
    TEST_ASSERT_EQUAL_INT(0, d.shadow_map_size);  /* 0 = renderer default */
}

static void test_json_roundtrip(void)
{
    JceRenderSettings s = jce_render_settings_default();
    s.shadow_quality  = 1;
    s.shadow_map_size = 4096;
    s.shadow_cascades = 2;
    s.shadow_distance = 120.0f;
    s.lod_bias        = -1.0f;
    s.vsync           = 0;
    s.msaa            = 4;
    TEST_ASSERT_TRUE(jce_render_settings_save_json(RS_PATH, &s));

    JceRenderSettings o;
    TEST_ASSERT_TRUE(jce_render_settings_load_json(RS_PATH, &o));
    TEST_ASSERT_EQUAL_INT(1,    o.shadow_quality);
    TEST_ASSERT_EQUAL_INT(4096, o.shadow_map_size);
    TEST_ASSERT_EQUAL_INT(2,    o.shadow_cascades);
    TEST_ASSERT_TRUE(fabsf(o.shadow_distance - 120.0f) < 1e-3f);
    TEST_ASSERT_TRUE(fabsf(o.lod_bias - (-1.0f))        < 1e-3f);
    TEST_ASSERT_EQUAL_INT(0,    o.vsync);
    TEST_ASSERT_EQUAL_INT(4,    o.msaa);
}

static void test_missing_file_fails(void)
{
    JceRenderSettings o = jce_render_settings_default();
    TEST_ASSERT_FALSE(jce_render_settings_load_json("no_such_render_settings.json", &o));
}

/* ── Task 3: Look Profile project-default round-trip ─────────────────────── */

static void test_look_profile_project_default_round_trip(void)
{
    JceRenderSettings s = jce_render_settings_default();
    s.wrap_factor = 0.30f;
    s.ambient_hemisphere = true;
    s.ambient_ground_color[0] = 0.18f;
    s.rim_intensity = 0.5f;
    s.tonemap_op = 2;   /* AgX */
    s.lut_strength = 0.6f;
    s.toon_character = true;
    s.bloom_knee = 0.4f;
    TEST_ASSERT_TRUE(jce_render_settings_save_json(RS_PATH_LOOK, &s));

    JceRenderSettings out;
    TEST_ASSERT_TRUE(jce_render_settings_load_json(RS_PATH_LOOK, &out));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f, out.wrap_factor);
    TEST_ASSERT_TRUE(out.ambient_hemisphere);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.18f, out.ambient_ground_color[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, out.rim_intensity);
    TEST_ASSERT_EQUAL_INT(2, out.tonemap_op);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.6f, out.lut_strength);
    TEST_ASSERT_TRUE(out.toon_character);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, out.bloom_knee);
}

/* v1-compat: a v1 file without look keys must yield neutral defaults. */
static void test_v1_file_without_look_keeps_neutral_defaults(void)
{
    const char *v1_json = "{\"$schema\":\"jce.rendersettings.v1\",\"shadowQuality\":1}";
    size_t len = strlen(v1_json);
    /* Write the v1 JSON directly via the host FS helper exposed in jce_json.h:
     * use the save helper with a scratch struct (simpler than calling jce_fs directly). */
    FILE *f = fopen(RS_PATH_V1, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(v1_json, 1, len, f);
    fclose(f);

    JceRenderSettings out;
    TEST_ASSERT_TRUE(jce_render_settings_load_json(RS_PATH_V1, &out));
    /* Look Profile fields should stay at neutral defaults. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out.wrap_factor);
    TEST_ASSERT_FALSE(out.ambient_hemisphere);
    TEST_ASSERT_EQUAL_INT(0, out.tonemap_op);   /* JCE_TONEMAP_ACES */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out.lut_strength);
    /* Legacy field should still load. */
    TEST_ASSERT_EQUAL_INT(1, out.shadow_quality);
}

/* ── Texture quality + anisotropic filtering ──────────────────────────
 *
 * Both consumers -- jce_texture_set_quality_mip_bias and
 * jce_texture_set_aniso_override -- existed all along and both had ONLY editor
 * callers, so a designer who set Texture Quality to Quarter to fit a low-end
 * target saw the viewport honour it and shipped Full.  This struct is the
 * thing the build exports and the shipped main applies, so the fields had to
 * land here to travel at all.
 *
 * THE DEFAULTS MATTER MORE THAN THE ROUND TRIP.  Every render_settings.json
 * already on disk predates these keys; if the defaults-first load did not put
 * back the no-op values, this change would silently drop a mip level (or force
 * aniso off) in every project that never opened the Quality tab -- which is
 * the shape of a defect this file's own header records having shipped before
 * ("it was consumed as one for a long time, which cost every shipped game a
 * mip level at the default"). */
static void test_texture_quality_defaults_are_no_ops(void)
{
    JceRenderSettings d = jce_render_settings_default();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, d.texture_quality,
        "0 = Full: the default must not drop a mip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, d.anisotropic,
        "<0 = leave aniso to the GPU tier; 0 would FORCE it off");
}

static void test_texture_quality_round_trips(void)
{
    JceRenderSettings s = jce_render_settings_default();
    s.texture_quality = 2;      /* Quarter */
    s.anisotropic     = 1;      /* forced on */
    TEST_ASSERT_TRUE(jce_render_settings_save_json(RS_PATH, &s));

    JceRenderSettings out;
    TEST_ASSERT_TRUE(jce_render_settings_load_json(RS_PATH, &out));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, out.texture_quality,
        "the authored texture quality must survive the file the build exports");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, out.anisotropic,
        "and so must the anisotropic override");

    /* 0 is a LEVEL, not an absent key: forcing aniso OFF must survive too, and
     * a defaults-first loader that treats 0 as "unset" would restore -1. */
    s.anisotropic = 0;
    TEST_ASSERT_TRUE(jce_render_settings_save_json(RS_PATH, &s));
    TEST_ASSERT_TRUE(jce_render_settings_load_json(RS_PATH, &out));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, out.anisotropic,
        "aniso forced OFF must not read back as the tier default");
}

static void test_a_file_without_the_texture_keys_keeps_the_no_ops(void)
{
    const char *old_json =
        "{\"$schema\":\"jce.rendersettings.v2\",\"shadowQuality\":1,"
        "\"lodBias\":1.5}";
    FILE *f = fopen(RS_PATH_V1, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(old_json, 1, strlen(old_json), f);
    fclose(f);

    JceRenderSettings out;
    TEST_ASSERT_TRUE(jce_render_settings_load_json(RS_PATH_V1, &out));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, out.texture_quality,
        "a document written before these keys existed must load to Full -- "
        "otherwise this change costs every existing project a mip level");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, out.anisotropic,
        "...and must leave aniso to the GPU tier, not force it off");
    TEST_ASSERT_EQUAL_INT(1, out.shadow_quality);   /* and still load the rest */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_json_roundtrip);
    RUN_TEST(test_missing_file_fails);
    RUN_TEST(test_look_profile_project_default_round_trip);
    RUN_TEST(test_v1_file_without_look_keeps_neutral_defaults);
    RUN_TEST(test_texture_quality_defaults_are_no_ops);
    RUN_TEST(test_texture_quality_round_trips);
    RUN_TEST(test_a_file_without_the_texture_keys_keeps_the_no_ops);
    return UNITY_END();
}
