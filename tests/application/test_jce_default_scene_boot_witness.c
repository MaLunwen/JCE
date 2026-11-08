/* Actual stock scene-load owner; only service/diagnostic boundaries are ports. */
#include <jce/application/jce_project.h>
#include <jce/application/jce_runtime_boot.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_trace.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_scene_serial.h>
#include "unity.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static union { long double align; char byte; } scene_marker, fs_marker, bundle_marker[2];
static JceScene *s_scene;
static JceFileSystem *s_runtime_fs;
static JceBundleFile *s_bundle_files[2];
static int s_bundle_count;
static char s_boot_scene_path[1024];
static const char *env_scene, *env_perf, *env_frame, *expected_path;
static const char *bundle_paths[2];
static bool create_ok, load_ok, trace_on, packaged_allowed, load_succeeded;
static unsigned create_calls, load_calls, destroy_calls, witness_count;
static unsigned env_reads, trace_reads;
static bool witness_after_load, witness_after_publication;
static char loaded_path[1024], witnessed_path[1024];

static char *test_getenv(const char *name)
{
    ++env_reads;
    if (strcmp(name, "JCE_STARTUP_SCENE") == 0) return (char *)env_scene;
    if (strcmp(name, "JCE_PERF_LOG") == 0) return (char *)env_perf;
    if (strcmp(name, "JCE_KPI_FRAME_LOG") == 0) return (char *)env_frame;
    TEST_FAIL_MESSAGE("unexpected environment input in the actual scene loader");
    return NULL;
}

JceScene *jce_scene_create(void)
{
    ++create_calls;
    return create_ok ? (JceScene *)&scene_marker : NULL;
}
void jce_scene_destroy(JceScene *scene)
{
    TEST_ASSERT_EQUAL_PTR((JceScene *)&scene_marker, scene);
    ++destroy_calls;
}
bool jce_scene_serial_load_vfs(JceScene *scene, const JceFileSystem *fs, const char *path)
{
    TEST_ASSERT_EQUAL_PTR((JceScene *)&scene_marker, scene);
    TEST_ASSERT_EQUAL_PTR((JceFileSystem *)&fs_marker, fs);
    ++load_calls;
    snprintf(loaded_path, sizeof(loaded_path), "%s", path);
    load_succeeded = load_ok && expected_path && strcmp(path, expected_path) == 0;
    return load_succeeded;
}
const char *jce_bundle_file_scene_path(const JceBundleFile *bundle)
{
    for (unsigned i = 0; i < 2; ++i)
        if (bundle == (JceBundleFile *)&bundle_marker[i]) return bundle_paths[i];
    TEST_FAIL_MESSAGE("unknown Bundle ownership");
    return NULL;
}
bool JCE_CALL jce_trace_enabled(void)
{
    ++trace_reads;
    return trace_on;
}
void JCE_CALL jce_log_write(JceLogLevel level, const char *tag,
                           const char *file, int line, const char *format, ...)
{
    static const char prefix[] = "startup scene loaded (runtime assets): ";
    char message[1536];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (strncmp(message, prefix, sizeof(prefix) - 1) != 0) return;
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO, level);
    TEST_ASSERT_EQUAL_STRING("app", tag);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(line > 0);
    ++witness_count;
    snprintf(witnessed_path, sizeof(witnessed_path), "%s", message + sizeof(prefix) - 1);
    witness_after_load = load_succeeded;
    witness_after_publication = strcmp(s_boot_scene_path, loaded_path) == 0;
}

#define getenv test_getenv
#define JCE_DEFAULT_MAIN_PACKAGED_SCENE_ALLOWED(path) (packaged_allowed && (path)[0])
#include JCE_TEST_DEFAULT_SCENE_BOOT_OWNER
#undef getenv

void setUp(void)
{
    s_scene = NULL;
    s_runtime_fs = (JceFileSystem *)&fs_marker;
    s_bundle_count = 0;
    memset(s_boot_scene_path, 0, sizeof(s_boot_scene_path));
    memset(loaded_path, 0, sizeof(loaded_path));
    memset(witnessed_path, 0, sizeof(witnessed_path));
    bundle_paths[0] = bundle_paths[1] = NULL;
    env_scene = env_perf = env_frame = NULL;
    expected_path = "scenes/project.scene.json";
    create_ok = load_ok = packaged_allowed = true;
    trace_on = load_succeeded = false;
    create_calls = load_calls = destroy_calls = witness_count = env_reads = trace_reads = 0;
    witness_after_load = witness_after_publication = false;
    for (unsigned i = 0; i < 2; ++i)
        s_bundle_files[i] = (JceBundleFile *)&bundle_marker[i];
}
void tearDown(void) {}

static void expect_witness(bool diagnostic)
{
#if defined(JCE_LOG_NONE)
    (void)diagnostic;
    TEST_ASSERT_EQUAL_UINT(0, witness_count);
    TEST_ASSERT_EQUAL_UINT(0, trace_reads);
    TEST_ASSERT_EQUAL_UINT(1, env_reads); /* Only the original scene override. */
#else
# if defined(JCE_DIST)
    TEST_ASSERT_EQUAL_UINT(diagnostic ? 1 : 0, witness_count);
# else
    (void)diagnostic;
    TEST_ASSERT_EQUAL_UINT(1, witness_count);
# endif
    if (witness_count) {
        TEST_ASSERT_TRUE(witness_after_load);
        TEST_ASSERT_TRUE(witness_after_publication);
        TEST_ASSERT_EQUAL_STRING(expected_path, witnessed_path);
    }
#endif
}
static bool load_project(void)
{
    JceProject project = {0};
    project.startup_scene = (char *)expected_path;
    return jce_default_load_startup_scene(&project, NULL);
}
static void test_success_preserves_default_dist_silence(void)
{
    TEST_ASSERT_TRUE(load_project());
    TEST_ASSERT_EQUAL_STRING(expected_path, s_boot_scene_path);
    TEST_ASSERT_EQUAL_UINT(1, load_calls);
    expect_witness(false);
}
static void test_existing_perf_flag_policy_survives_dist_compilation(void)
{
    const char *flags[] = {"1", "enabled", "false", "off", "0", "0disabled", "", NULL};
    const bool enabled[] = {true, true, true, true, false, false, false, false};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        setUp();
        env_perf = flags[i];
        TEST_ASSERT_TRUE(load_project());
        expect_witness(enabled[i]);
    }
}
static void test_raw_frame_path_observes_without_periodic_perf(void)
{
    const char *paths[] = {"frames.csv", "0", "", NULL};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        setUp();
        env_perf = "0";
        env_frame = paths[i];
        TEST_ASSERT_TRUE(load_project());
        expect_witness(i < 2);
    }
}
static void test_actual_core_trace_state_is_consumed(void)
{
    trace_on = true;
    env_perf = "0";
    TEST_ASSERT_TRUE(load_project());
    expect_witness(true);
}
static void test_project_then_boot_then_bundle_precedence(void)
{
    JceProject project = {0};
    JceRuntimeBootManifest boot = {0};
    project.startup_scene = (char *)expected_path;
    snprintf(boot.startup_scene, sizeof(boot.startup_scene), "scenes/boot.scene.json");
    env_perf = "1";
    TEST_ASSERT_TRUE(jce_default_load_startup_scene(&project, &boot));
    expect_witness(true);
    setUp(); env_perf = "1"; expected_path = boot.startup_scene;
    TEST_ASSERT_TRUE(jce_default_load_startup_scene(NULL, &boot));
    expect_witness(true);
    setUp(); env_perf = "1"; expected_path = "scenes/bundle.scene.json";
    s_bundle_count = 2; bundle_paths[0] = ""; bundle_paths[1] = expected_path;
    TEST_ASSERT_TRUE(jce_default_load_startup_scene(NULL, NULL));
    expect_witness(true);
}
static void test_actual_override_is_the_witnessed_loaded_path(void)
{
    JceProject project = {0};
    project.startup_scene = "scenes/project.scene.json";
    expected_path = env_scene = "scenes/override % actual.scene.json";
    env_perf = "1";
    TEST_ASSERT_TRUE(jce_default_load_startup_scene(&project, NULL));
    TEST_ASSERT_EQUAL_STRING(expected_path, loaded_path);
    expect_witness(true);
}
static void test_failed_and_wrong_loads_never_publish_success(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        setUp(); env_perf = "1";
        snprintf(s_boot_scene_path, sizeof(s_boot_scene_path), "previous.scene.json");
        if (i == 0) s_runtime_fs = NULL;
        if (i == 1) create_ok = false;
        if (i == 2) load_ok = false;
        if (i == 3) env_scene = "scenes/wrong.scene.json";
        TEST_ASSERT_FALSE(load_project());
        TEST_ASSERT_EQUAL_UINT(0, witness_count);
        TEST_ASSERT_EQUAL_STRING("previous.scene.json", s_boot_scene_path);
        TEST_ASSERT_EQUAL_UINT(i >= 2 ? 1 : 0, destroy_calls);
        TEST_ASSERT_NULL(s_scene);
    }
}
static void test_missing_and_packaged_refused_scenes_are_not_witnesses(void)
{
    env_perf = "1";
#if JCE_DEFAULT_MAIN_PACKAGED_ONLY
    TEST_ASSERT_FALSE(jce_default_load_startup_scene(NULL, NULL));
    packaged_allowed = false;
    TEST_ASSERT_FALSE(load_project());
#else
    TEST_ASSERT_TRUE(jce_default_load_startup_scene(NULL, NULL));
#endif
    TEST_ASSERT_EQUAL_UINT(0, witness_count);
    TEST_ASSERT_EQUAL_UINT(0, create_calls);
    TEST_ASSERT_EQUAL_UINT(0, load_calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_success_preserves_default_dist_silence);
    RUN_TEST(test_existing_perf_flag_policy_survives_dist_compilation);
    RUN_TEST(test_raw_frame_path_observes_without_periodic_perf);
    RUN_TEST(test_actual_core_trace_state_is_consumed);
    RUN_TEST(test_project_then_boot_then_bundle_precedence);
    RUN_TEST(test_actual_override_is_the_witnessed_loaded_path);
    RUN_TEST(test_failed_and_wrong_loads_never_publish_success);
    RUN_TEST(test_missing_and_packaged_refused_scenes_are_not_witnesses);
    return UNITY_END();
}
