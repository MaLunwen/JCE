/* Complete production startup shader-root owner with typed counted ports.
 * This fixture creates no backend, reads no environment and executes no GPU work. */
#include "unity.h"
#include <jce/renderer/jce_gfx_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct JcePakArchive { unsigned identity; } JcePakArchive;
typedef struct JceRenderer {
    bool is_fallback;
    struct { char *root; bool bound; } shader_dev;
} JceRenderer;
static struct { JceRenderer *owner; } s_device;
static JceRenderer current_renderer, foreign_renderer;
static const JcePakArchive pak = {71};
static struct {
    bool main_thread, frame_current, fail_allocation, allocation_live;
    unsigned thread_calls, frame_calls, allocations, acquired, frees;
    unsigned pak_loads, overlay_loads;
    size_t allocation_size;
    const JceRenderer *frame_renderer;
    const JcePakArchive *loaded_pak;
    const char *loaded_vs, *loaded_fs, *loaded_root;
    char storage[256];
} controls;

static bool jce_thread_is_main(void)
{
    ++controls.thread_calls;
    return controls.main_thread;
}
static bool jce_renderer_frame_admission_current_internal(const JceRenderer *r)
{
    ++controls.frame_calls;
    controls.frame_renderer = r;
    return controls.frame_current;
}
static void *allocate_root(size_t size)
{
    ++controls.allocations;
    controls.allocation_size = size;
    if (controls.fail_allocation) return NULL;
    TEST_ASSERT_FALSE(controls.allocation_live);
    TEST_ASSERT_TRUE(size <= sizeof controls.storage);
    controls.allocation_live = true;
    ++controls.acquired;
    return controls.storage;
}
static void free_root(void *root)
{
    TEST_ASSERT_EQUAL_PTR(controls.storage, root);
    TEST_ASSERT_TRUE(controls.allocation_live);
    TEST_ASSERT_NULL_MESSAGE(s_device.owner, "detach the borrowed device owner before freeing its root");
    controls.allocation_live = false;
    ++controls.frees;
}
static JceShaderHandle shader_load_program_named(const JcePakArchive *archive,
    const char *vs, const char *fs)
{
    ++controls.pak_loads;
    controls.loaded_pak = archive;
    controls.loaded_vs = vs;
    controls.loaded_fs = fs;
    controls.loaded_root = NULL;
    return (JceShaderHandle){31};
}
static JceShaderHandle shader_load_program_overlay_named(const char *root,
    const JcePakArchive *archive, const char *vs, const char *fs)
{
    ++controls.overlay_loads;
    controls.loaded_pak = archive;
    controls.loaded_vs = vs;
    controls.loaded_fs = fs;
    controls.loaded_root = root;
    return (JceShaderHandle){47};
}
#define JCE_MALLOC(size) allocate_root(size)
#define JCE_FREE(root) free_root(root)
#ifndef JCE_TEST_RENDERER_SHADER_DEV_OWNER
#define JCE_TEST_RENDERER_SHADER_DEV_OWNER "renderer/jce_renderer_shader_dev.inc.h"
#endif
#include JCE_TEST_RENDERER_SHADER_DEV_OWNER
#undef JCE_FREE
#undef JCE_MALLOC

void setUp(void)
{
    memset(&controls, 0, sizeof controls);
    memset(&current_renderer, 0, sizeof current_renderer);
    memset(&foreign_renderer, 0, sizeof foreign_renderer);
    controls.main_thread = controls.frame_current = true;
    s_device.owner = &current_renderer;
}
void tearDown(void)
{
    if (s_device.owner) renderer_shader_dev_release(s_device.owner);
    TEST_ASSERT_FALSE(controls.allocation_live);
    TEST_ASSERT_EQUAL_UINT(controls.acquired, controls.frees);
}
static void expect_pak_pair(const JcePakArchive *archive, const char *vs, const char *fs)
{
    JceShaderHandle result = jce_renderer_shader_load_named_internal(archive, vs, fs);
    TEST_ASSERT_EQUAL_UINT16(31, result.idx);
    TEST_ASSERT_EQUAL_PTR(archive, controls.loaded_pak);
    TEST_ASSERT_EQUAL_PTR(vs, controls.loaded_vs);
    TEST_ASSERT_EQUAL_PTR(fs, controls.loaded_fs);
    TEST_ASSERT_NULL(controls.loaded_root);
    TEST_ASSERT_EQUAL_UINT(0, controls.overlay_loads);
}
static void test_no_owner_preserves_named_pak_delegation(void)
{
    s_device.owner = NULL;
    expect_pak_pair(&pak, "grass", "grass_shadow");
    TEST_ASSERT_EQUAL_UINT(1, controls.pak_loads);
    TEST_ASSERT_EQUAL_UINT(0, controls.frame_calls);
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
}
static void test_unbound_owner_preserves_named_pak_delegation(void)
{
    expect_pak_pair(NULL, "color", "color");
    TEST_ASSERT_EQUAL_PTR(&current_renderer, controls.frame_renderer);
    TEST_ASSERT_EQUAL_UINT(1, controls.pak_loads);
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
}
static void test_null_root_binds_pak_without_allocating(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, NULL));
    TEST_ASSERT_TRUE(current_renderer.shader_dev.bound);
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
    expect_pak_pair(&pak, "grass", "grass_shadow");
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "later-root"));
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
}
static void test_empty_root_binds_pak_without_allocating_or_freeing(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, ""));
    TEST_ASSERT_TRUE(current_renderer.shader_dev.bound);
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
    expect_pak_pair(&pak, "grass", "grass_shadow");
    renderer_shader_dev_release(&current_renderer);
    TEST_ASSERT_NULL(s_device.owner);
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
    TEST_ASSERT_EQUAL_UINT(0, controls.frees);
}
static void test_root_is_copied_and_named_aliases_are_forwarded(void)
{
    char root[] = "owned-root";
    const char *vs = "grass", *fs = "grass_shadow";
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, root));
    TEST_ASSERT_TRUE(root != current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_STRING("owned-root", current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_UINT(sizeof root, controls.allocation_size);
    memset(root, 'x', sizeof root - 1);
    JceShaderHandle result = jce_renderer_shader_load_named_internal(&pak, vs, fs);
    TEST_ASSERT_EQUAL_UINT16(47, result.idx);
    TEST_ASSERT_EQUAL_PTR(&pak, controls.loaded_pak);
    TEST_ASSERT_EQUAL_PTR(vs, controls.loaded_vs);
    TEST_ASSERT_EQUAL_PTR(fs, controls.loaded_fs);
    TEST_ASSERT_EQUAL_PTR(current_renderer.shader_dev.root, controls.loaded_root);
    TEST_ASSERT_EQUAL_STRING("owned-root", controls.loaded_root);
    TEST_ASSERT_EQUAL_UINT(0, controls.pak_loads);
    TEST_ASSERT_EQUAL_UINT(1, controls.overlay_loads);
}
static void test_any_repeat_bind_preserves_the_original_root(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, "owned-root"));
    char *root = current_renderer.shader_dev.root;
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, root));
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "owned-root"));
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "other-root"));
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, ""));
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, NULL));
    TEST_ASSERT_EQUAL_PTR(root, current_renderer.shader_dev.root);
    TEST_ASSERT_TRUE(current_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_STRING("owned-root", root);
    TEST_ASSERT_EQUAL_UINT(1, controls.allocations);
    TEST_ASSERT_EQUAL_UINT(0, controls.frees);
    TEST_ASSERT_EQUAL_PTR(&current_renderer, s_device.owner);
}
static void test_allocation_failure_keeps_unbound_owner_and_can_retry(void)
{
    controls.fail_allocation = true;
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "owned-root"));
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_PTR(&current_renderer, s_device.owner);
    expect_pak_pair(&pak, "color", "color");
    controls.fail_allocation = false;
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, "owned-root"));
    TEST_ASSERT_TRUE(current_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_STRING("owned-root", current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_UINT(2, controls.allocations);
    TEST_ASSERT_EQUAL_UINT(1, controls.acquired);
}
static void test_null_and_foreign_renderer_cannot_bind_or_change_current_owner(void)
{
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(NULL, "root"));
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&foreign_renderer, "root"));
    TEST_ASSERT_FALSE(foreign_renderer.shader_dev.bound);
    TEST_ASSERT_NULL(foreign_renderer.shader_dev.root);
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_PTR(&current_renderer, s_device.owner);
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
    s_device.owner = NULL;
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "root"));
    TEST_ASSERT_NULL(s_device.owner);
}
static void test_fallback_renderer_cannot_bind(void)
{
    current_renderer.is_fallback = true;
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "root"));
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
}
static void test_foreign_api_thread_cannot_bind_or_load(void)
{
    controls.main_thread = false;
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "root"));
    JceShaderHandle result = jce_renderer_shader_load_named_internal(&pak, "grass", "grass_shadow");
    TEST_ASSERT_FALSE(jce_shader_valid(result));
    s_device.owner = NULL;
    result = jce_renderer_shader_load_named_internal(&pak, "color", "color");
    TEST_ASSERT_FALSE(jce_shader_valid(result));
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
    TEST_ASSERT_EQUAL_UINT(0, controls.frame_calls);
    TEST_ASSERT_EQUAL_UINT(0, controls.pak_loads);
    TEST_ASSERT_EQUAL_UINT(0, controls.overlay_loads);
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
}
static void test_lost_epoch_rejects_bind_and_unbound_load(void)
{
    controls.frame_current = false;
    TEST_ASSERT_FALSE(jce_renderer_shader_dev_bind_internal(&current_renderer, "root"));
    JceShaderHandle result = jce_renderer_shader_load_named_internal(&pak, "grass", "grass_shadow");
    TEST_ASSERT_FALSE(jce_shader_valid(result));
    TEST_ASSERT_EQUAL_PTR(&current_renderer, controls.frame_renderer);
    TEST_ASSERT_EQUAL_UINT(0, controls.allocations);
    TEST_ASSERT_EQUAL_UINT(0, controls.pak_loads);
    TEST_ASSERT_EQUAL_UINT(0, controls.overlay_loads);
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
}
static void test_lost_epoch_retains_bound_root_without_loading(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, "owned-root"));
    controls.frame_current = false;
    JceShaderHandle result = jce_renderer_shader_load_named_internal(&pak, "grass", "grass_shadow");
    TEST_ASSERT_FALSE(jce_shader_valid(result));
    TEST_ASSERT_TRUE(current_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_STRING("owned-root", current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_UINT(0, controls.pak_loads);
    TEST_ASSERT_EQUAL_UINT(0, controls.overlay_loads);
    TEST_ASSERT_EQUAL_UINT(0, controls.frees);
}
static void test_only_matching_release_detaches_and_frees_once(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&current_renderer, "owned-root"));
    renderer_shader_dev_release(NULL);
    renderer_shader_dev_release(&foreign_renderer);
    TEST_ASSERT_EQUAL_PTR(&current_renderer, s_device.owner);
    TEST_ASSERT_TRUE(current_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_STRING("owned-root", current_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_UINT(0, controls.frees);
    renderer_shader_dev_release(&current_renderer);
    TEST_ASSERT_NULL(s_device.owner);
    TEST_ASSERT_NULL(current_renderer.shader_dev.root);
    TEST_ASSERT_FALSE(current_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_UINT(1, controls.frees);
    renderer_shader_dev_release(&current_renderer);
    TEST_ASSERT_EQUAL_UINT(1, controls.frees);
    expect_pak_pair(&pak, "grass", "grass_shadow");
}
static void test_old_renderer_release_cannot_detach_new_owner(void)
{
    renderer_shader_dev_release(&current_renderer);
    s_device.owner = &foreign_renderer;
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(&foreign_renderer, "new-root"));
    renderer_shader_dev_release(&current_renderer);
    TEST_ASSERT_EQUAL_PTR(&foreign_renderer, s_device.owner);
    TEST_ASSERT_TRUE(foreign_renderer.shader_dev.bound);
    TEST_ASSERT_EQUAL_STRING("new-root", foreign_renderer.shader_dev.root);
    TEST_ASSERT_EQUAL_UINT(0, controls.frees);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_owner_preserves_named_pak_delegation);
    RUN_TEST(test_unbound_owner_preserves_named_pak_delegation);
    RUN_TEST(test_null_root_binds_pak_without_allocating);
    RUN_TEST(test_empty_root_binds_pak_without_allocating_or_freeing);
    RUN_TEST(test_root_is_copied_and_named_aliases_are_forwarded);
    RUN_TEST(test_any_repeat_bind_preserves_the_original_root);
    RUN_TEST(test_allocation_failure_keeps_unbound_owner_and_can_retry);
    RUN_TEST(test_null_and_foreign_renderer_cannot_bind_or_change_current_owner);
    RUN_TEST(test_fallback_renderer_cannot_bind);
    RUN_TEST(test_foreign_api_thread_cannot_bind_or_load);
    RUN_TEST(test_lost_epoch_rejects_bind_and_unbound_load);
    RUN_TEST(test_lost_epoch_retains_bound_root_without_loading);
    RUN_TEST(test_only_matching_release_detaches_and_frees_once);
    RUN_TEST(test_old_renderer_release_cannot_detach_new_owner);
    return UNITY_END();
}
