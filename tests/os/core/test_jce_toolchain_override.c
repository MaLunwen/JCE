/*
 * Exact production toolchain owner; filesystem admission uses real temporary
 * files. Only environment lookup and bounded discovery processes are controlled.
 * Marker presence is plausible discovery, not compiler/SDK/ABI execution proof.
 */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_str.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifndef JCE_TEST_TOOLCHAIN_FILE_PREFIX
#define JCE_TEST_TOOLCHAIN_FILE_PREFIX "_ut_toolchain_override"
#endif

static char g_root[1024];
static char g_path_env[1024];
static unsigned g_spawn_count;
static char g_spawn_exe[1024];
static char g_spawn_args[128];
static bool g_clear_public_override;
static char g_programfiles_x86[1024], g_query_stdout[1100], g_query_stderr[1100];
static bool g_query_enabled, g_require_captured_stderr, g_query_version, g_query_timeout;
static bool g_stderr_captured;
static int g_query_exit, g_version_exit;
static size_t g_stdout_offset, g_stderr_offset;
static unsigned g_process_destroyed, g_process_killed, g_stderr_reads;
static Uint64 g_query_ticks;
static int g_process_token;

static const char *controlled_getenv(const char *name)
{
    if (strcmp(name, "PATH") == 0 && g_path_env[0]) return g_path_env;
    if (strcmp(name, "ProgramFiles(x86)") == 0 && g_programfiles_x86[0])
        return g_programfiles_x86;
    return NULL;
}

static JceProcess *controlled_spawn(const JceProcessConfig *config)
{
    ++g_spawn_count;
    jce_strlcpy(g_spawn_exe, config->executable_path, sizeof g_spawn_exe);
    jce_strlcpy(g_spawn_args, config->arguments ? config->arguments : "",
                sizeof g_spawn_args);
    /* No marker fixture is executed. A windowed parent may have no valid
     * inherited stderr; model that boundary at the actual spawn contract. */
    if (!g_query_enabled || (g_require_captured_stderr && !config->capture_stderr))
        return NULL;
    g_query_version = config->arguments &&
        strstr(config->arguments, "catalog_productDisplayVersion");
    g_stderr_captured = config->capture_stderr;
    g_stdout_offset = g_stderr_offset = 0;
    return (JceProcess *)&g_process_token;
}

static size_t controlled_stdout(JceProcess *process, char *out, size_t capacity)
{
    (void)process;
    const char *text = g_query_version ? "18.7.3\r\n" : g_query_stdout;
    size_t remaining = strlen(text) - g_stdout_offset;
    size_t count = remaining < capacity ? remaining : capacity;
    memcpy(out, text + g_stdout_offset, count); g_stdout_offset += count;
    return count;
}
static size_t controlled_stderr(JceProcess *process, char *out, size_t capacity)
{
    (void)process; ++g_stderr_reads;
    if (!g_stderr_captured) return 0;
    size_t remaining = strlen(g_query_stderr) - g_stderr_offset;
    size_t count = remaining < capacity ? remaining : capacity;
    memcpy(out, g_query_stderr + g_stderr_offset, count); g_stderr_offset += count;
    return count;
}
static bool controlled_exit(JceProcess *process, int *code)
{
    (void)process;
    if (g_query_timeout) return false;
    if (code) *code = g_query_version ? g_version_exit : g_query_exit;
    return true;
}
static bool controlled_kill(JceProcess *process) { (void)process; ++g_process_killed; return true; }
static void controlled_destroy(JceProcess *process) { (void)process; ++g_process_destroyed; }
static Uint64 controlled_ticks(void) { g_query_ticks += 1000u; return g_query_ticks; }
static void controlled_delay(Uint32 ms) { (void)ms; }

#define SDL_getenv controlled_getenv
#define jce_process_spawn controlled_spawn
#define jce_process_read_stdout controlled_stdout
#define jce_process_read_stderr controlled_stderr
#define jce_process_poll_exit controlled_exit
#define jce_process_force_kill controlled_kill
#define jce_process_destroy controlled_destroy
#define SDL_GetTicks controlled_ticks
#define SDL_Delay controlled_delay
#define jce_toolchain_kind_name controlled_jce_toolchain_kind_name
#define jce_toolchain_kind_from_name controlled_jce_toolchain_kind_from_name
#define jce_toolchain_refresh controlled_jce_toolchain_refresh
#define jce_toolchain_get controlled_jce_toolchain_get
#define jce_toolchain_set_override controlled_jce_toolchain_set_override
#ifndef JCE_TEST_TOOLCHAIN_OWNER
#define JCE_TEST_TOOLCHAIN_OWNER "os/core/jce_toolchain.c"
#endif
#include JCE_TEST_TOOLCHAIN_OWNER
#undef jce_process_spawn
#undef jce_process_read_stdout
#undef jce_process_read_stderr
#undef jce_process_poll_exit
#undef jce_process_force_kill
#undef jce_process_destroy
#undef SDL_GetTicks
#undef SDL_Delay
#undef SDL_getenv

void setUp(void)
{
    char cwd[1024];
    char desired[1024];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof cwd));
    TEST_ASSERT_TRUE(jce_path_join(desired, sizeof desired, cwd,
                                  JCE_TEST_TOOLCHAIN_FILE_PREFIX));
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(desired, g_root, sizeof g_root));
    jce_path_canonicalise_inplace(g_root);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
    g_path_env[0] = 0;
    g_spawn_count = 0;
    g_spawn_exe[0] = 0;
    g_spawn_args[0] = 0;
    g_clear_public_override = false;
    g_programfiles_x86[0] = g_query_stdout[0] = g_query_stderr[0] = 0;
    g_query_enabled = g_require_captured_stderr = g_query_version = g_query_timeout = false;
    g_stderr_captured = false;
    g_query_exit = g_version_exit = 0; g_query_ticks = 0;
    g_process_destroyed = g_process_killed = g_stderr_reads = 0;
}

void tearDown(void)
{
    if (g_clear_public_override) jce_toolchain_set_override(JCE_TOOLCHAIN_CMAKE, NULL);
    g_path_env[0] = 0;
    /* Only the owned unique child selected by setUp is removed. */
    if (g_root[0]) TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(g_root));
    g_root[0] = 0;
}

static void owned_path(char *out, size_t cap, const char *leaf)
{
    TEST_ASSERT_TRUE(jce_path_join(out, cap, g_root, leaf));
}

static void owned_directory(char *out, size_t cap, const char *leaf)
{
    owned_path(out, cap, leaf);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out));
}

static void owned_file(char *out, size_t cap, const char *leaf, const char *data)
{
    owned_path(out, cap, leaf);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(out, data, strlen(data)));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(out));
}

static JceToolchain override_result(JceToolchainKind kind, const char *path)
{
    JceToolchain result = {0};
    result.kind = kind; /* Same initialization as public refresh. */
    probe_override(kind, path, &result);
    return result;
}

static void assert_rejected(JceToolchainKind kind, const char *path)
{
    const unsigned before = g_spawn_count;
    JceToolchain result = override_result(kind, path);
    TEST_ASSERT_FALSE(result.present);
    TEST_ASSERT_FALSE(result.from_override);
    TEST_ASSERT_EQUAL_STRING("", result.path);
    TEST_ASSERT_EQUAL_STRING("", result.version);
    TEST_ASSERT_EQUAL_UINT(before, g_spawn_count);
}

static void assert_present_root(JceToolchainKind kind, const char *input,
                                const char *root)
{
    JceToolchain result = override_result(kind, input);
    TEST_ASSERT_TRUE(result.present);
    TEST_ASSERT_TRUE(result.from_override);
    TEST_ASSERT_EQUAL_INT(kind, result.kind);
    TEST_ASSERT_EQUAL_STRING(root, result.path);
}

static void test_empty_directory_is_not_any_toolchain(void)
{
    char root[1024];
    owned_directory(root, sizeof root, "empty");
    for (int kind = 0; kind < JCE_TOOLCHAIN_COUNT; ++kind)
        assert_rejected((JceToolchainKind)kind, root);
}

static void test_arbitrary_file_is_not_any_toolchain(void)
{
    char file[1024];
    owned_file(file, sizeof file, "notes.txt", "not a toolchain\n");
    for (int kind = 0; kind < JCE_TOOLCHAIN_COUNT; ++kind)
        assert_rejected((JceToolchainKind)kind, file);
}

static void test_missing_path_is_not_any_toolchain(void)
{
    char missing[1024];
    owned_path(missing, sizeof missing, "missing");
    for (int kind = 0; kind < JCE_TOOLCHAIN_COUNT; ++kind)
        assert_rejected((JceToolchainKind)kind, missing);
}

static void test_executable_directories_use_existing_install_probe(void)
{
    const JceToolchainKind kinds[] = {
        JCE_TOOLCHAIN_CMAKE, JCE_TOOLCHAIN_NINJA,
        JCE_TOOLCHAIN_GCC, JCE_TOOLCHAIN_CLANG
    };
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
        char leaf[128], dir[1024], file[1024];
        const char *name = jce_toolchain_kind_name(kinds[i]);
        SDL_snprintf(leaf, sizeof leaf, "%s/bin", name);
        owned_directory(dir, sizeof dir, leaf);
        SDL_snprintf(leaf, sizeof leaf, "%s/bin/%s%s", name, name, TC_EXE_SUFFIX);
        owned_file(file, sizeof file, leaf, "marker only\n");
        const unsigned before = g_spawn_count;
        JceToolchain result = override_result(kinds[i], dir);
        TEST_ASSERT_TRUE(result.present);
        TEST_ASSERT_TRUE(result.from_override);
        TEST_ASSERT_EQUAL_STRING(dir, result.path);
        TEST_ASSERT_EQUAL_STRING("", result.version);
        TEST_ASSERT_EQUAL_UINT(before + 1, g_spawn_count);
        char normalized[1024];
        TEST_ASSERT_TRUE(jce_path_to_canonical(normalized, sizeof normalized,
                                               g_spawn_exe));
        TEST_ASSERT_EQUAL_STRING(file, normalized);
        TEST_ASSERT_EQUAL_STRING("--version", g_spawn_args);
    }
}

static void test_direct_executables_keep_parent_bin_root(void)
{
    const JceToolchainKind kinds[] = {
        JCE_TOOLCHAIN_CMAKE, JCE_TOOLCHAIN_NINJA,
        JCE_TOOLCHAIN_GCC, JCE_TOOLCHAIN_CLANG
    };
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
        char leaf[128], dir[1024], file[1024];
        const char *name = jce_toolchain_kind_name(kinds[i]);
        SDL_snprintf(leaf, sizeof leaf, "%s/bin", name);
        owned_directory(dir, sizeof dir, leaf);
        SDL_snprintf(leaf, sizeof leaf, "%s/bin/%s%s", name, name, TC_EXE_SUFFIX);
        owned_file(file, sizeof file, leaf, "marker only\n");
        assert_present_root(kinds[i], file, dir);
    }
}

static void test_wrong_file_beside_valid_executable_is_rejected(void)
{
    const JceToolchainKind kinds[] = {
        JCE_TOOLCHAIN_CMAKE, JCE_TOOLCHAIN_NINJA,
        JCE_TOOLCHAIN_GCC, JCE_TOOLCHAIN_CLANG
    };
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
        char leaf[128], file[1024];
        const char *name = jce_toolchain_kind_name(kinds[i]);
        SDL_snprintf(leaf, sizeof leaf, "%s/%s%s", name, name, TC_EXE_SUFFIX);
        owned_file(file, sizeof file, leaf, "marker only\n");
        SDL_snprintf(leaf, sizeof leaf, "%s/notes.txt", name);
        owned_file(file, sizeof file, leaf, "arbitrary sibling\n");
        assert_rejected(kinds[i], file);
    }
}

static void test_expected_executable_name_directory_is_rejected(void)
{
    char dir[1024], leaf[128], file[1024];
    owned_directory(dir, sizeof dir, "bin");
    SDL_snprintf(leaf, sizeof leaf, "bin/cmake%s", TC_EXE_SUFFIX);
    owned_directory(file, sizeof file, leaf);
    assert_rejected(JCE_TOOLCHAIN_CMAKE, dir);
    assert_rejected(JCE_TOOLCHAIN_CMAKE, file);
}

static void test_msvc_root_and_direct_vcvarsall_keep_vs_root(void)
{
    char root[1024], marker[1024];
    owned_directory(root, sizeof root, "Visual Studio");
    owned_file(marker, sizeof marker,
               "Visual Studio/VC/Auxiliary/Build/vcvarsall.bat", "marker only\n");
    assert_present_root(JCE_TOOLCHAIN_MSVC, root, root);
    assert_present_root(JCE_TOOLCHAIN_MSVC, marker, root);
    TEST_ASSERT_EQUAL_UINT(0, g_spawn_count);
}

static void test_msvc_wrong_layout_and_sibling_file_are_rejected(void)
{
    char root[1024], file[1024];
    owned_directory(root, sizeof root, "wrong-vs");
    owned_file(file, sizeof file, "wrong-vs/vcvarsall.bat", "wrong layout\n");
    assert_rejected(JCE_TOOLCHAIN_MSVC, root);
    assert_rejected(JCE_TOOLCHAIN_MSVC, file);
    owned_file(file, sizeof file,
               "vs/VC/Auxiliary/Build/vcvarsall.bat", "marker only\n");
    owned_file(file, sizeof file,
               "vs/VC/Auxiliary/Build/notes.txt", "arbitrary sibling\n");
    assert_rejected(JCE_TOOLCHAIN_MSVC, file);
}

static void test_ndk_root_uses_existing_marker_and_version_read(void)
{
    char root[1024], marker[1024];
    owned_directory(root, sizeof root, "ndk");
    owned_file(marker, sizeof marker, "ndk/source.properties",
               "Pkg.Desc = Android NDK\nPkg.Revision = 26.2.11394342\n");
    JceToolchain result = override_result(JCE_TOOLCHAIN_NDK, root);
    TEST_ASSERT_TRUE(result.present);
    TEST_ASSERT_TRUE(result.from_override);
    TEST_ASSERT_EQUAL_STRING(root, result.path);
    TEST_ASSERT_EQUAL_STRING("26.2.11394342", result.version);
    TEST_ASSERT_EQUAL_UINT(0, g_spawn_count);
    /* SDK input is its root; a file is not silently reinterpreted as a root. */
    assert_rejected(JCE_TOOLCHAIN_NDK, marker);
}

static void test_emsdk_existing_primary_and_python_markers_are_reused(void)
{
    char root[1024], marker[1024], leaf[128];
    owned_directory(root, sizeof root, "emsdk-primary");
    SDL_snprintf(leaf, sizeof leaf, "emsdk-primary/emsdk%s",
                 TC_EXE_SUFFIX[0] ? ".bat" : "");
    owned_file(marker, sizeof marker, leaf, "marker only\n");
    owned_file(marker, sizeof marker, "emsdk-primary/emscripten-releases-tags.json",
               "{\"latest\":\"3.1.64\"}\n");
    JceToolchain result = override_result(JCE_TOOLCHAIN_EMSDK, root);
    TEST_ASSERT_TRUE(result.present);
    TEST_ASSERT_TRUE(result.from_override);
    TEST_ASSERT_EQUAL_STRING(root, result.path);
    TEST_ASSERT_EQUAL_STRING("3.1.64", result.version);
    owned_directory(root, sizeof root, "emsdk-python");
    owned_file(marker, sizeof marker, "emsdk-python/emsdk.py", "marker only\n");
    assert_present_root(JCE_TOOLCHAIN_EMSDK, root, root);
    assert_rejected(JCE_TOOLCHAIN_EMSDK, marker);
    TEST_ASSERT_EQUAL_UINT(0, g_spawn_count);
}

static void test_xcode_developer_root_requires_compiler_marker(void)
{
    char root[1024], marker[1024];
    owned_directory(root, sizeof root, "Developer");
    owned_file(marker, sizeof marker, "Developer/usr/bin/clang", "marker only\n");
    assert_present_root(JCE_TOOLCHAIN_XCODE, root, root);
    assert_rejected(JCE_TOOLCHAIN_XCODE, marker);
    TEST_ASSERT_EQUAL_UINT(0, g_spawn_count);
}

static void test_sdk_marker_directories_are_not_files(void)
{
    char root[1024], marker[1024];
    owned_directory(root, sizeof root, "ndk");
    owned_directory(marker, sizeof marker, "ndk/source.properties");
    assert_rejected(JCE_TOOLCHAIN_NDK, root);
    owned_directory(root, sizeof root, "emsdk");
    owned_directory(marker, sizeof marker, "emsdk/emsdk.py");
    assert_rejected(JCE_TOOLCHAIN_EMSDK, root);
    owned_directory(root, sizeof root, "Developer");
    owned_directory(marker, sizeof marker, "Developer/usr/bin/clang");
    assert_rejected(JCE_TOOLCHAIN_XCODE, root);
    owned_directory(root, sizeof root, "vs");
    owned_directory(marker, sizeof marker, "vs/VC/Auxiliary/Build/vcvarsall.bat");
    assert_rejected(JCE_TOOLCHAIN_MSVC, root);
}

static void set_auto_cmake_path(char *dir, size_t cap)
{
    char leaf[128], marker[1024];
    owned_directory(dir, cap, "auto/bin");
    SDL_snprintf(leaf, sizeof leaf, "auto/bin/cmake%s", TC_EXE_SUFFIX);
    owned_file(marker, sizeof marker, leaf, "marker only\n");
    jce_strlcpy(g_path_env, dir, sizeof g_path_env);
    g_clear_public_override = true;
}

static void test_bad_directory_override_keeps_public_auto_fallback(void)
{
    char automatic[1024], empty[1024];
    set_auto_cmake_path(automatic, sizeof automatic);
    owned_directory(empty, sizeof empty, "empty");
    jce_toolchain_set_override(JCE_TOOLCHAIN_CMAKE, empty);
    const JceToolchain *result = jce_toolchain_get(JCE_TOOLCHAIN_CMAKE);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(result->present);
    TEST_ASSERT_FALSE(result->from_override);
    TEST_ASSERT_EQUAL_STRING(automatic, result->path);
}

static void test_bad_file_then_valid_override_and_clear_are_truthful(void)
{
    char automatic[1024], chosen_dir[1024], chosen_file[1024], wrong_file[1024];
    char leaf[128];
    set_auto_cmake_path(automatic, sizeof automatic);
    owned_directory(chosen_dir, sizeof chosen_dir, "chosen/bin");
    SDL_snprintf(leaf, sizeof leaf, "chosen/bin/cmake%s", TC_EXE_SUFFIX);
    owned_file(chosen_file, sizeof chosen_file, leaf, "marker only\n");
    owned_file(wrong_file, sizeof wrong_file, "chosen/bin/notes.txt", "arbitrary\n");
    jce_toolchain_set_override(JCE_TOOLCHAIN_CMAKE, wrong_file);
    const JceToolchain *result = jce_toolchain_get(JCE_TOOLCHAIN_CMAKE);
    TEST_ASSERT_TRUE(result->present);
    TEST_ASSERT_FALSE(result->from_override);
    TEST_ASSERT_EQUAL_STRING(automatic, result->path);
    jce_toolchain_set_override(JCE_TOOLCHAIN_CMAKE, chosen_file);
    result = jce_toolchain_get(JCE_TOOLCHAIN_CMAKE);
    TEST_ASSERT_TRUE(result->present);
    TEST_ASSERT_TRUE(result->from_override);
    TEST_ASSERT_EQUAL_STRING(chosen_dir, result->path);
    jce_toolchain_set_override(JCE_TOOLCHAIN_CMAKE, NULL);
    result = jce_toolchain_get(JCE_TOOLCHAIN_CMAKE);
    TEST_ASSERT_TRUE(result->present);
    TEST_ASSERT_FALSE(result->from_override);
    TEST_ASSERT_EQUAL_STRING(automatic, result->path);
}

static void test_query_drains_both_pipes_after_the_stdout_prefix_is_full(void)
{
    memset(g_query_stdout, 'x', sizeof g_query_stdout - 1u);
    g_query_stdout[sizeof g_query_stdout - 1u] = 0;
    memcpy(g_query_stdout, "first\n", 6u);
    memset(g_query_stderr, 'e', sizeof g_query_stderr - 1u);
    g_query_stderr[sizeof g_query_stderr - 1u] = 0;
    g_query_enabled = true;
    char line[16];
    TEST_ASSERT_TRUE(tc_capture_first_line("controlled-query", NULL, 10000,
                                          line, sizeof line));
    TEST_ASSERT_EQUAL_STRING("first", line);
    TEST_ASSERT_EQUAL_UINT(sizeof g_query_stdout - 1u, g_stdout_offset);
    TEST_ASSERT_EQUAL_UINT(sizeof g_query_stderr - 1u, g_stderr_offset);
    TEST_ASSERT_EQUAL_UINT(1, g_process_destroyed);
    TEST_ASSERT_EQUAL_UINT(0, g_process_killed);
    TEST_ASSERT_TRUE(g_stderr_reads >= 5u);
}

#if JCE_PLATFORM_WINDOWS
static void make_auto_msvc(char root[1024])
{
    char marker[1024];
    owned_directory(g_programfiles_x86, sizeof g_programfiles_x86, "program files x86");
    owned_file(marker, sizeof marker,
        "program files x86/Microsoft Visual Studio/Installer/vswhere.exe", "marker only\n");
    owned_directory(root, 1024, "Visual Studio/18/Enterprise");
    owned_file(marker, sizeof marker,
        "Visual Studio/18/Enterprise/VC/Auxiliary/Build/vcvarsall.bat", "marker only\n");
    SDL_snprintf(g_query_stdout, sizeof g_query_stdout, "\r\n%s\r\n", root);
    g_query_enabled = g_require_captured_stderr = true;
}

static void test_automatic_msvc_owns_both_streams_without_developer_path(void)
{
    char root[1024]; make_auto_msvc(root);
    jce_strlcpy(g_query_stderr, "optional discovery diagnostics\n", sizeof g_query_stderr);
    jce_toolchain_refresh();
    const JceToolchain *result = jce_toolchain_get(JCE_TOOLCHAIN_MSVC);
    TEST_ASSERT_TRUE(result->present); TEST_ASSERT_FALSE(result->from_override);
    TEST_ASSERT_EQUAL_STRING(root, result->path);
    TEST_ASSERT_EQUAL_STRING("18.7.3", result->version);
    TEST_ASSERT_EQUAL_UINT(2, g_spawn_count); TEST_ASSERT_EQUAL_UINT(2, g_process_destroyed);
    TEST_ASSERT_EQUAL_UINT(0, g_process_killed);
    TEST_ASSERT_TRUE(g_stderr_reads >= 2u);
    TEST_ASSERT_NOT_NULL(strstr(g_spawn_args, "catalog_productDisplayVersion"));
}

static void test_msvc_failed_or_unfinished_query_never_supplies_a_root(void)
{
    char root[1024]; make_auto_msvc(root); g_require_captured_stderr = false;
    const char *good = g_query_stdout;
    char saved[1100]; jce_strlcpy(saved, good, sizeof saved);
    for (unsigned mode = 0; mode < 4u; ++mode) {
        jce_strlcpy(g_query_stdout, saved, sizeof g_query_stdout);
        g_query_exit = mode == 0u ? 1 : 0; g_query_timeout = mode == 1u;
        if (mode == 2u) g_query_stdout[0] = 0;
        if (mode == 3u) jce_strlcpy(g_query_stdout, "\r\n\r\n", sizeof g_query_stdout);
        JceToolchain result = {0}; g_process_destroyed = g_process_killed = 0;
        probe_msvc(&result);
        TEST_ASSERT_FALSE(result.present);
        TEST_ASSERT_EQUAL_UINT(1, g_process_destroyed);
        TEST_ASSERT_EQUAL_UINT(mode == 1u ? 1u : 0u, g_process_killed);
    }
}

static void test_optional_msvc_version_failure_preserves_verified_install_root(void)
{
    char root[1024]; make_auto_msvc(root); g_version_exit = 1;
    JceToolchain result = {0}; probe_msvc(&result);
    TEST_ASSERT_TRUE(result.present); TEST_ASSERT_EQUAL_STRING(root, result.path);
    TEST_ASSERT_EQUAL_STRING("", result.version);
    TEST_ASSERT_EQUAL_UINT(2, g_spawn_count); TEST_ASSERT_EQUAL_UINT(2, g_process_destroyed);
}

static void test_stderr_cannot_become_the_msvc_root_and_invalid_output_does_not_spawn(void)
{
    char root[1024]; make_auto_msvc(root); g_require_captured_stderr = false;
    g_query_stdout[0] = 0; jce_strlcpy(g_query_stderr, root, sizeof g_query_stderr);
    JceToolchain result = {0}; probe_msvc(&result); TEST_ASSERT_FALSE(result.present);
    TEST_ASSERT_EQUAL_UINT(1, g_process_destroyed);
    const unsigned prior = g_spawn_count;
    TEST_ASSERT_FALSE(tc_capture_first_line("unused", NULL, 10, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(prior, g_spawn_count);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_directory_is_not_any_toolchain);
    RUN_TEST(test_arbitrary_file_is_not_any_toolchain);
    RUN_TEST(test_missing_path_is_not_any_toolchain);
    RUN_TEST(test_executable_directories_use_existing_install_probe);
    RUN_TEST(test_direct_executables_keep_parent_bin_root);
    RUN_TEST(test_wrong_file_beside_valid_executable_is_rejected);
    RUN_TEST(test_expected_executable_name_directory_is_rejected);
    RUN_TEST(test_msvc_root_and_direct_vcvarsall_keep_vs_root);
    RUN_TEST(test_msvc_wrong_layout_and_sibling_file_are_rejected);
    RUN_TEST(test_ndk_root_uses_existing_marker_and_version_read);
    RUN_TEST(test_emsdk_existing_primary_and_python_markers_are_reused);
    RUN_TEST(test_xcode_developer_root_requires_compiler_marker);
    RUN_TEST(test_sdk_marker_directories_are_not_files);
    RUN_TEST(test_bad_directory_override_keeps_public_auto_fallback);
    RUN_TEST(test_bad_file_then_valid_override_and_clear_are_truthful);
    RUN_TEST(test_query_drains_both_pipes_after_the_stdout_prefix_is_full);
#if JCE_PLATFORM_WINDOWS
    RUN_TEST(test_automatic_msvc_owns_both_streams_without_developer_path);
    RUN_TEST(test_msvc_failed_or_unfinished_query_never_supplies_a_root);
    RUN_TEST(test_optional_msvc_version_failure_preserves_verified_install_root);
    RUN_TEST(test_stderr_cannot_become_the_msvc_root_and_invalid_output_does_not_spawn);
#endif
    return UNITY_END();
}
