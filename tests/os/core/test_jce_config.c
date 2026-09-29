/*
 * test_jce_config.c — Unit tests for jce_config.h
 *
 * Layer: L1.  Defaults + INI loader (via tmp host file).
 */

#include "unity.h"

#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_filesystem.h>

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_have_expected_values);
    RUN_TEST(test_load_missing_file_returns_false);
    RUN_TEST(test_load_overrides_keys);
    RUN_TEST(test_load_null_args);
    return UNITY_END();
}
