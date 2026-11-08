/* Actual cooked-placement owner and real VFS controls; no GPU or window. */
#include "middleware/scene/jce_sr_internal.h"
#include "middleware/scene/jce_sr_foliage_cooked.h"
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_alloc.h>
#include "unity.h"
#include <stdio.h>
#include <string.h>

static char root[1024], mounted_root[1024], old_path[1024];
static JceSceneRenderer *renderer;
static JceFileSystem *fs;
static unsigned resolve_calls;
static JceVegetationScatterComponent scatter;

static bool resolve_old_project(const char *path, char *out, int capacity, void *user)
{
    (void)path;
    (void)user;
    ++resolve_calls;
    return jce_path_to_canonical(out, (size_t)capacity, old_path);
}

static void write_placement(const char *path, uint32_t count, uint32_t seed)
{
    JceFoliageInstance instances[2];
    memset(instances, 0, sizeof instances);
    instances[0].pos[0] = 10.0f;
    instances[1].pos[0] = 20.0f;
    instances[0].scale = instances[1].scale = 1.0f;
    const size_t size = jce_foliage_cook_size(count);
    void *blob = jce_malloc(size);
    TEST_ASSERT_NOT_NULL(blob);
    TEST_ASSERT_TRUE(jce_foliage_cook(instances, count, seed, blob, size, NULL));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, blob, (uint64_t)size));
    jce_free(blob);
}

void setUp(void)
{
    char cwd[1024];
    static unsigned ordinal;
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof cwd));
    char name[80];
    snprintf(name, sizeof name, "_ut_foliage_cooked_vfs_%u", ++ordinal);
    TEST_ASSERT_TRUE(jce_path_join(root, sizeof root, cwd, name));
    TEST_ASSERT_TRUE(jce_path_join(mounted_root, sizeof mounted_root, root, "mounted"));
    TEST_ASSERT_TRUE(jce_path_join(old_path, sizeof old_path, root, "old_project.bin"));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(mounted_root));
    write_placement(old_path, 1u, 11u);
    renderer = JCE_CALLOC(1u, sizeof *renderer);
    TEST_ASSERT_NOT_NULL(renderer);
    renderer->has_cbs = true;
    renderer->cbs.resolve_path = resolve_old_project;
    resolve_calls = 0u;
    memset(&scatter, 0, sizeof scatter);
    scatter.seed = 11u;
    strcpy(scatter.baked_placement_path, "roots.bin");
    fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "/", mounted_root));
}

void tearDown(void)
{
    jce_fs_set_active(NULL);
    jce_fs_destroy(fs);
    if (renderer) {
        jce_free(renderer->foliage_cache[0].insts);
        jce_free(renderer);
    }
    jce_fs_host_remove_recursive(root);
}

static void mount_placement(uint32_t count, uint32_t seed)
{
    char path[1024];
    TEST_ASSERT_TRUE(jce_path_join(path, sizeof path, mounted_root, "roots.bin"));
    write_placement(path, count, seed);
}

static void test_current_mount_wins_over_previous_project_callback(void)
{
    mount_placement(2u, 11u);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_TRUE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(2u, renderer->foliage_cache[0].inst_count);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, renderer->foliage_cache[0].insts[1].pos[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, resolve_calls);
}

static void test_isolated_miss_cannot_resolve_old_absolute_project_file(void)
{
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_FALSE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, resolve_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, renderer->foliage_cache[0].inst_count);
}

static void test_overlay_miss_retains_development_callback(void)
{
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
    TEST_ASSERT_TRUE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(1u, renderer->foliage_cache[0].inst_count);
    TEST_ASSERT_EQUAL_UINT32(1u, resolve_calls);
}

static void test_invalid_mounted_seed_does_not_fall_back_to_valid_old_project(void)
{
    mount_placement(2u, 99u);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
    TEST_ASSERT_FALSE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, resolve_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, renderer->foliage_cache[0].inst_count);
}

static void test_valid_empty_mount_is_not_replaced_by_nonempty_old_project(void)
{
    mount_placement(0u, 11u);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_TRUE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, renderer->foliage_cache[0].inst_count);
    TEST_ASSERT_EQUAL_UINT32(0u, resolve_calls);
}

static void test_oversized_mount_is_rejected_before_callback_or_large_allocation(void)
{
    const size_t size = jce_foliage_cook_size(JCE_FOLIAGE_MAX_INSTANCES) + 1u;
    void *bytes = JCE_CALLOC(1u, size);
    TEST_ASSERT_NOT_NULL(bytes);
    char path[1024];
    TEST_ASSERT_TRUE(jce_path_join(path, sizeof path, mounted_root, "roots.bin"));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, bytes, (uint64_t)size));
    jce_free(bytes);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
    TEST_ASSERT_FALSE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, resolve_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, renderer->foliage_cache[0].inst_cap);
}

static void test_explicit_absolute_tooling_path_stays_host_owned(void)
{
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_TRUE(jce_path_to_canonical(scatter.baked_placement_path,
        sizeof scatter.baked_placement_path, old_path));
    TEST_ASSERT_TRUE(sr_foliage_try_load_cooked(renderer, &scatter, 0));
    TEST_ASSERT_EQUAL_UINT32(1u, renderer->foliage_cache[0].inst_count);
    TEST_ASSERT_EQUAL_UINT32(1u, resolve_calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_current_mount_wins_over_previous_project_callback);
    RUN_TEST(test_isolated_miss_cannot_resolve_old_absolute_project_file);
    RUN_TEST(test_overlay_miss_retains_development_callback);
    RUN_TEST(test_invalid_mounted_seed_does_not_fall_back_to_valid_old_project);
    RUN_TEST(test_valid_empty_mount_is_not_replaced_by_nonempty_old_project);
    RUN_TEST(test_oversized_mount_is_rejected_before_callback_or_large_allocation);
    RUN_TEST(test_explicit_absolute_tooling_path_stays_host_owned);
    return UNITY_END();
}
