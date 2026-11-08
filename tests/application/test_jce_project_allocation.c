/* Complete project owner; only its allocations have counted failure ports.
 * JSON parsing and host reads use the actual public implementations. This is
 * not a process-wide OOM, filesystem failure or editor UI certificate. */
#include "unity.h"
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_str.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { ALLOCATION_CAPACITY = 128 };
static struct {
    void *blocks[ALLOCATION_CAPACITY];
    unsigned calls, fail_call, count, live, failed, invalid_free;
} allocation;

static void *track_allocation(void *pointer)
{
    if (pointer) {
        if (allocation.count == ALLOCATION_CAPACITY) {
            jce_free(pointer);
            return NULL;
        }
        allocation.blocks[allocation.count++] = pointer;
        ++allocation.live;
    }
    return pointer;
}

static bool refuse_allocation(void)
{
    if (++allocation.calls != allocation.fail_call) return false;
    ++allocation.failed;
    return true;
}

static void *project_test_malloc(size_t size)
{
    return refuse_allocation() ? NULL : track_allocation(jce_malloc(size));
}

static char *JCE_CALL project_test_strdup(const char *source)
{
    if (!source) return NULL;
    return refuse_allocation() ? NULL : (char *)track_allocation(jce_strdup(source));
}

static void project_test_free(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < allocation.count; ++i) {
        if (allocation.blocks[i] != pointer) continue;
        allocation.blocks[i] = NULL;
        --allocation.live;
        jce_free(pointer);
        return;
    }
    ++allocation.invalid_free;
}

#define jce_malloc project_test_malloc
#define jce_free project_test_free
#define jce_strdup project_test_strdup
#ifndef JCE_TEST_PROJECT_OWNER
#define JCE_TEST_PROJECT_OWNER "application/jce_project.c"
#endif
#include JCE_TEST_PROJECT_OWNER
#undef jce_strdup
#undef jce_free
#undef jce_malloc

#define TEST_DIR "_ut_project_allocation"
static const char full_manifest[] =
    "{\"schema\":3,\"name\":\"complete\",\"version\":\"2.0\",\"target\":\"Complete\","
    "\"exe\":\"Complete.exe\",\"sdk\":\"sdk/test\",\"platform\":\"win\","
    "\"assets\":[\"assets\",\"shaders\"],\"variants\":[\"debug\",\"release\",\"dist\"],"
    "\"source_assets\":\"source\",\"cooked_assets\":\"cache/cooked\","
    "\"startup_scene\":\"scenes/main.scene\",\"bundles\":[\"a.jbundle\",\"b.jbundle\"],"
    "\"script_modules\":[\"native/a.dll\",\"native/b.dll\"],"
    "\"script_assemblies\":[\"managed/a.dll\",\"managed/b.dll\"],"
    "\"ui_default_font\":\"fonts/default.ttf\",\"ui_font_fallbacks\":\"fonts/fallback.ttf\"}";

static void release_fixture_leaks(void)
{
    for (unsigned i = 0; i < allocation.count; ++i) {
        if (!allocation.blocks[i]) continue;
        jce_free(allocation.blocks[i]);
        allocation.blocks[i] = NULL;
    }
    allocation.live = 0;
}

void setUp(void)
{
    memset(&allocation, 0, sizeof(allocation));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(TEST_DIR));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(TEST_DIR "/jce_project.json", full_manifest,
                                        sizeof(full_manifest) - 1));
}

void tearDown(void)
{
    release_fixture_leaks();
    TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(TEST_DIR));
}

static unsigned successful_load_count(const char *path)
{
    memset(&allocation, 0, sizeof(allocation));
    JceProject *project = jce_project_load(path);
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_STRING("complete", project->name);
    TEST_ASSERT_EQUAL_STRING("sdk/test", project->sdk_path);
    TEST_ASSERT_EQUAL_STRING("source", project->source_assets);
    TEST_ASSERT_EQUAL_STRING("fonts/fallback.ttf", project->ui_font_fallbacks);
    TEST_ASSERT_EQUAL_INT(2, project->asset_dirs_count);
    TEST_ASSERT_EQUAL_INT(3, project->build_variants_count);
    TEST_ASSERT_EQUAL_INT(2, project->bundles_count);
    TEST_ASSERT_EQUAL_INT(2, project->script_modules_count);
    TEST_ASSERT_EQUAL_INT(2, project->script_assemblies_count);
    const unsigned calls = allocation.calls;
    jce_project_free(project);
    TEST_ASSERT_EQUAL_UINT(0, allocation.live);
    TEST_ASSERT_EQUAL_UINT(0, allocation.invalid_free);
    return calls;
}

static void test_load_rejects_each_owned_allocation_failure_without_partial_project(void)
{
    const unsigned total = successful_load_count(TEST_DIR);
    unsigned partial = 0, leaked = 0, invalid = 0, refused = 0;
    for (unsigned fault = 1; fault <= total; ++fault) {
        memset(&allocation, 0, sizeof(allocation));
        allocation.fail_call = fault;
        JceProject *project = jce_project_load(TEST_DIR);
        if (project) { ++partial; jce_project_free(project); }
        leaked += allocation.live;
        invalid += allocation.invalid_free;
        refused += allocation.failed;
        release_fixture_leaks();
    }
    printf("project load allocation controls: attempts=%u partial=%u leaked=%u invalid_free=%u refused=%u\n",
           total, partial, leaked, invalid, refused);
    TEST_ASSERT_EQUAL_UINT(total, refused);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, partial, "allocation refusal published an incomplete project");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, leaked, "allocation refusal leaked project-owned storage");
    TEST_ASSERT_EQUAL_UINT(0, invalid);
}

static void test_manifest_path_loading_uses_the_same_complete_owner(void)
{
    TEST_ASSERT_TRUE(successful_load_count(TEST_DIR "/jce_project.json") > 10);
}

static void test_absent_optional_fields_and_legacy_defaults_remain_valid(void)
{
    const char minimal[] = "{\"name\":\"minimal\"}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(TEST_DIR "/jce_project.json", minimal, sizeof(minimal) - 1));
    JceProject *project = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_STRING("minimal", project->name);
    TEST_ASSERT_EQUAL_STRING("minimal", project->target_name);
    TEST_ASSERT_EQUAL_STRING("assets", project->source_assets);
    TEST_ASSERT_EQUAL_STRING("resources/_cooked", project->cooked_assets);
    TEST_ASSERT_NULL(project->sdk_path);
    TEST_ASSERT_NULL(project->startup_scene);
    TEST_ASSERT_NULL(project->ui_default_font);
    TEST_ASSERT_NULL(project->ui_font_fallbacks);
    TEST_ASSERT_EQUAL_INT(0, project->script_modules_count);
    TEST_ASSERT_EQUAL_INT(0, project->script_assemblies_count);
    jce_project_free(project);
    TEST_ASSERT_EQUAL_UINT(0, allocation.live);
    TEST_ASSERT_EQUAL_UINT(0, allocation.invalid_free);
}

static void test_new_rejects_each_owned_allocation_failure_and_frees_partial_lists(void)
{
    JceProject *project = jce_project_new(TEST_DIR, "new-project");
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_STRING("new-project", project->name);
    TEST_ASSERT_EQUAL_INT(1, project->asset_dirs_count);
    TEST_ASSERT_EQUAL_INT(2, project->build_variants_count);
    const unsigned total = allocation.calls;
    jce_project_free(project);
    TEST_ASSERT_EQUAL_UINT(0, allocation.live);
    unsigned partial = 0, leaked = 0, invalid = 0, refused = 0;
    for (unsigned fault = 1; fault <= total; ++fault) {
        memset(&allocation, 0, sizeof(allocation));
        allocation.fail_call = fault;
        project = jce_project_new(TEST_DIR, "new-project");
        if (project) { ++partial; jce_project_free(project); }
        leaked += allocation.live;
        invalid += allocation.invalid_free;
        refused += allocation.failed;
        release_fixture_leaks();
    }
    printf("project new allocation controls: attempts=%u partial=%u leaked=%u invalid_free=%u refused=%u\n",
           total, partial, leaked, invalid, refused);
    TEST_ASSERT_EQUAL_UINT(total, refused);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, partial, "allocation refusal published an incomplete new project");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, leaked, "allocation refusal leaked a partially constructed list");
    TEST_ASSERT_EQUAL_UINT(0, invalid);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_load_rejects_each_owned_allocation_failure_without_partial_project);
    RUN_TEST(test_manifest_path_loading_uses_the_same_complete_owner);
    RUN_TEST(test_absent_optional_fields_and_legacy_defaults_remain_valid);
    RUN_TEST(test_new_rejects_each_owned_allocation_failure_and_frees_partial_lists);
    return UNITY_END();
}
