#include <jce/application/jce_args.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_startup_scene_accepts_split_option(void)
{
    char arg0[] = "game";
    char arg1[] = "--scene";
    char arg2[] = "scenes/black_hole_lab.scene.json";
    char *argv[] = {arg0, arg1, arg2};
    char out[128];

    jce_args_stash(3, argv);

    TEST_ASSERT_TRUE(jce_args_get_startup_scene(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("scenes/black_hole_lab.scene.json", out);
}

static void test_startup_scene_accepts_joined_option(void)
{
    char arg0[] = "game";
    char arg1[] = "--scene=scenes/black_hole_lab.scene.json";
    char *argv[] = {arg0, arg1};
    char out[128];

    jce_args_stash(2, argv);

    TEST_ASSERT_TRUE(jce_args_get_startup_scene(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("scenes/black_hole_lab.scene.json", out);
}

static void test_startup_scene_rejects_host_path_escape(void)
{
    char arg0[] = "game";
    char arg1[] = "--scene=../outside.scene.json";
    char *argv[] = {arg0, arg1};
    char out[128] = "unchanged";

    jce_args_stash(2, argv);

    TEST_ASSERT_FALSE(jce_args_get_startup_scene(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("unchanged", out);
}

static void test_shader_dev_dir_accepts_joined_option(void)
{
    char arg0[] = "game";
    char arg1[] = "--shader-dir=D:/build/project-shaders";
    char *argv[] = {arg0, arg1};
    char out[128];

    jce_args_stash(2, argv);

    TEST_ASSERT_TRUE(jce_args_get_shader_dev_dir(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("D:/build/project-shaders", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_startup_scene_accepts_split_option);
    RUN_TEST(test_startup_scene_accepts_joined_option);
    RUN_TEST(test_startup_scene_rejects_host_path_escape);
    RUN_TEST(test_shader_dev_dir_accepts_joined_option);
    return UNITY_END();
}
