/*
 * test_jce_config.c — Unit tests for jce_config.h
 *
 * Layer: L1.  Defaults + INI loader (via tmp host file).
 */

#include "unity.h"

#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_filesystem.h>

#include "application/jce_engine_window_policy.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static char g_ini_path[1024];

void setUp(void)
{
    char base[1024];
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_ini_path, sizeof(g_ini_path), "%s/_ut_jce.ini", base);
    (void)jce_fs_host_remove_file(g_ini_path);
}

void tearDown(void)
{
    (void)jce_fs_host_remove_file(g_ini_path);
}

/* ---- defaults -------------------------------------------------------- */

static void test_defaults_have_expected_values(void)
{
    JceConfig c = jce_config_defaults();
    TEST_ASSERT_EQUAL_INT(640, c.window_width);
    TEST_ASSERT_EQUAL_INT(480, c.window_height);
    TEST_ASSERT_FALSE(c.fullscreen);
    TEST_ASSERT_FALSE(c.maximized);
    TEST_ASSERT_TRUE(c.resizable);
    TEST_ASSERT_TRUE(c.vsync);
    TEST_ASSERT_EQUAL_STRING("JCE", c.window_title);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c.master_volume);
}

/* ---- load ------------------------------------------------------------ */

static void test_load_missing_file_returns_false(void)
{
    JceConfig c = jce_config_defaults();
    TEST_ASSERT_FALSE(jce_config_load(&c, g_ini_path));
    TEST_ASSERT_EQUAL_INT(640, c.window_width); /* unchanged */
}

static void test_load_overrides_keys(void)
{
    const char *ini =
        "# comment\n"
        "[window]\n"
        "width = 1024\n"
        "height = 768\n"
        "title = Foo\n"
        "fullscreen = true\n"
        "maximized = true\n"
        "resizable = false\n"
        "[renderer]\n"
        "vsync = false\n"
        "clear_color = 12345678\n"
        "[audio]\n"
        "master_volume = 0.5\n"
        "[logging]\n"
        "level = debug\n"
        "colors = false\n";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(g_ini_path, ini, strlen(ini)));

    JceConfig c = jce_config_defaults();
    TEST_ASSERT_TRUE(jce_config_load(&c, g_ini_path));

    TEST_ASSERT_EQUAL_INT (1024, c.window_width);
    TEST_ASSERT_EQUAL_INT (768,  c.window_height);
    TEST_ASSERT_EQUAL_STRING("Foo", c.window_title);
    TEST_ASSERT_TRUE      (c.fullscreen);
    TEST_ASSERT_TRUE      (c.maximized);
    TEST_ASSERT_FALSE     (c.resizable);
    TEST_ASSERT_FALSE     (c.vsync);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, c.master_volume);
    TEST_ASSERT_FALSE     (c.log_colors);
}

static void test_load_null_args(void)
{
    JceConfig c = jce_config_defaults();
    TEST_ASSERT_FALSE(jce_config_load(NULL, g_ini_path));
    TEST_ASSERT_FALSE(jce_config_load(&c,   NULL));
}

/* ---- application defaults and resolved window policy ---------------- */

static JceConfig application_window_defaults(void)
{
    JceConfig cfg = jce_config_defaults();
    JceAppDesc desc = {0};
    desc.window_width = 1600;
    desc.window_height = 900;
    desc.maximized = true;
    jce_engine_window_policy_seed(&cfg, &desc);
    return cfg;
}

static void test_missing_file_preserves_application_window_defaults(void)
{
    JceConfig c = application_window_defaults();
    TEST_ASSERT_FALSE(jce_config_load(&c, g_ini_path));
    TEST_ASSERT_EQUAL_INT(1600, c.window_width);
    TEST_ASSERT_EQUAL_INT(900, c.window_height);
    TEST_ASSERT_TRUE(c.maximized);
    TEST_ASSERT_FALSE(c.fullscreen);
    TEST_ASSERT_TRUE(c.resizable);
}

static void test_partial_config_preserves_missing_window_defaults(void)
{
    const char *ini = "[window]\nwidth = 800\n[renderer]\nvsync = false\n";
    JceConfig c = application_window_defaults();
    c.fullscreen = true;
    c.resizable = false;
    TEST_ASSERT_TRUE(jce_fs_host_write_all(g_ini_path, ini, strlen(ini)));
    TEST_ASSERT_TRUE(jce_config_load(&c, g_ini_path));
    TEST_ASSERT_EQUAL_INT(800, c.window_width);
    TEST_ASSERT_EQUAL_INT(900, c.window_height);
    TEST_ASSERT_TRUE(c.maximized);
    TEST_ASSERT_TRUE(c.fullscreen);
    TEST_ASSERT_FALSE(c.resizable);
    TEST_ASSERT_FALSE(c.vsync);
}

static void test_unrelated_config_preserves_application_window_defaults(void)
{
    const char *ini = "[audio]\nmaster_volume = 0.25\n";
    JceConfig c = application_window_defaults();
    TEST_ASSERT_TRUE(jce_fs_host_write_all(g_ini_path, ini, strlen(ini)));
    TEST_ASSERT_TRUE(jce_config_load(&c, g_ini_path));
    TEST_ASSERT_EQUAL_INT(1600, c.window_width);
    TEST_ASSERT_EQUAL_INT(900, c.window_height);
    TEST_ASSERT_TRUE(c.maximized);
    TEST_ASSERT_FALSE(c.fullscreen);
    TEST_ASSERT_TRUE(c.resizable);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, c.master_volume);
}

static void test_explicit_false_overrides_application_window_defaults(void)
{
    const char *variants[] = {
        "[window]\nwidth = 640\nheight = 480\nmaximized = false\n"
        "fullscreen = false\nresizable = false\n",
        "[window]\nresizable = false\nfullscreen = false\n"
        "maximized = false\nheight = 480\nwidth = 640\n"
    };
    size_t i;
    for (i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        JceConfig c = application_window_defaults();
        c.fullscreen = true;
        TEST_ASSERT_TRUE(jce_fs_host_write_all(g_ini_path, variants[i],
                                               strlen(variants[i])));
        TEST_ASSERT_TRUE(jce_config_load(&c, g_ini_path));
        TEST_ASSERT_EQUAL_INT(640, c.window_width);
        TEST_ASSERT_EQUAL_INT(480, c.window_height);
        TEST_ASSERT_FALSE(c.maximized);
        TEST_ASSERT_FALSE(c.fullscreen);
        TEST_ASSERT_FALSE(c.resizable);
        TEST_ASSERT_FALSE(jce_engine_window_policy_maximized(&c));
    }
}

static void test_size_only_config_does_not_infer_normal_window_mode(void)
{
    const char *ini = "[window]\nwidth = 640\nheight = 480\n";
    JceConfig c = application_window_defaults();
    TEST_ASSERT_TRUE(jce_fs_host_write_all(g_ini_path, ini, strlen(ini)));
    TEST_ASSERT_TRUE(jce_config_load(&c, g_ini_path));
    TEST_ASSERT_EQUAL_INT(640, c.window_width);
    TEST_ASSERT_EQUAL_INT(480, c.window_height);
    TEST_ASSERT_TRUE(c.maximized);
    TEST_ASSERT_TRUE(jce_engine_window_policy_maximized(&c));
}

static void test_window_descriptor_defaults_are_representable_and_independent(void)
{
    JceConfig c = jce_config_defaults();
    JceAppDesc desc = {0};
    c.window_width = 720;
    c.window_height = 540;
    c.fullscreen = true;
    c.resizable = false;
    c.maximized = true;
    jce_engine_window_policy_seed(&c, &desc);
    TEST_ASSERT_EQUAL_INT(720, c.window_width);
    TEST_ASSERT_EQUAL_INT(540, c.window_height);
    TEST_ASSERT_FALSE(c.maximized);
    TEST_ASSERT_TRUE(c.fullscreen);
    TEST_ASSERT_FALSE(c.resizable);

    desc.window_height = 480;
    desc.maximized = true;
    if ((uint64_t)UINT32_MAX > (uint64_t)INT_MAX) {
        desc.window_width = UINT32_MAX;
        jce_engine_window_policy_seed(&c, &desc);
        TEST_ASSERT_EQUAL_INT(720, c.window_width);
        TEST_ASSERT_EQUAL_INT(480, c.window_height);
    }
    desc.window_width = 1024;
    jce_engine_window_policy_seed(&c, &desc);
    TEST_ASSERT_EQUAL_INT(1024, c.window_width);
    TEST_ASSERT_EQUAL_INT(480, c.window_height);
    TEST_ASSERT_TRUE(c.maximized);
    TEST_ASSERT_TRUE(c.fullscreen);
    TEST_ASSERT_FALSE(c.resizable);

    jce_engine_window_policy_seed(&c, NULL);
    jce_engine_window_policy_seed(NULL, &desc);
    TEST_ASSERT_EQUAL_INT(1024, c.window_width);
    TEST_ASSERT_TRUE(c.maximized);
}

static void test_fullscreen_preserves_windowed_restore_policy(void)
{
    JceConfig c = application_window_defaults();
    c.fullscreen = true;
    TEST_ASSERT_FALSE(jce_engine_window_policy_maximized(&c));
    TEST_ASSERT_TRUE(c.maximized);
    TEST_ASSERT_EQUAL_INT(1600, c.window_width);
    TEST_ASSERT_EQUAL_INT(900, c.window_height);
    c.fullscreen = false;
    TEST_ASSERT_TRUE(jce_engine_window_policy_maximized(&c));
    c.maximized = false;
    TEST_ASSERT_FALSE(jce_engine_window_policy_maximized(&c));
    TEST_ASSERT_FALSE(jce_engine_window_policy_maximized(NULL));
}

static void test_window_mode_save_roundtrip_keeps_restore_bounds_and_false(void)
{
    JceConfig saved = application_window_defaults();
    JceConfig loaded = jce_config_defaults();
    saved.window_width = 640;
    saved.window_height = 480;
    saved.resizable = false;
    TEST_ASSERT_TRUE(jce_config_save(&saved, g_ini_path));
    TEST_ASSERT_TRUE(jce_config_load(&loaded, g_ini_path));
    TEST_ASSERT_EQUAL_INT(640, loaded.window_width);
    TEST_ASSERT_EQUAL_INT(480, loaded.window_height);
    TEST_ASSERT_TRUE(loaded.maximized);
    TEST_ASSERT_FALSE(loaded.fullscreen);
    TEST_ASSERT_FALSE(loaded.resizable);

    saved.maximized = false;
    saved.fullscreen = true;
    saved.resizable = true;
    loaded = application_window_defaults();
    loaded.resizable = false;
    TEST_ASSERT_TRUE(jce_config_save(&saved, g_ini_path));
    TEST_ASSERT_TRUE(jce_config_load(&loaded, g_ini_path));
    TEST_ASSERT_EQUAL_INT(640, loaded.window_width);
    TEST_ASSERT_EQUAL_INT(480, loaded.window_height);
    TEST_ASSERT_FALSE(loaded.maximized);
    TEST_ASSERT_TRUE(loaded.fullscreen);
    TEST_ASSERT_TRUE(loaded.resizable);
    TEST_ASSERT_FALSE(jce_engine_window_policy_maximized(&loaded));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_have_expected_values);
    RUN_TEST(test_load_missing_file_returns_false);
    RUN_TEST(test_load_overrides_keys);
    RUN_TEST(test_load_null_args);
    RUN_TEST(test_missing_file_preserves_application_window_defaults);
    RUN_TEST(test_partial_config_preserves_missing_window_defaults);
    RUN_TEST(test_unrelated_config_preserves_application_window_defaults);
    RUN_TEST(test_explicit_false_overrides_application_window_defaults);
    RUN_TEST(test_size_only_config_does_not_infer_normal_window_mode);
    RUN_TEST(test_window_descriptor_defaults_are_representable_and_independent);
    RUN_TEST(test_fullscreen_preserves_windowed_restore_policy);
    RUN_TEST(test_window_mode_save_roundtrip_keeps_restore_bounds_and_false);
    return UNITY_END();
}
