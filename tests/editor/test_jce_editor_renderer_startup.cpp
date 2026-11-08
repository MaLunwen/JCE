/* Exact editor startup request owner + actual config loader; no renderer. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include <jce/os/core/jce_log.h>
#include <cstdarg>
#include <cstdio>
extern "C" void JCE_CALL jce_test_gpu_preference_log_write(
    JceLogLevel level, const char *tag, const char *file, int line, const char *format, ...);

#ifndef JCE_EDITOR_RENDERER_STARTUP_HEADER
#define JCE_EDITOR_RENDERER_STARTUP_HEADER "core/jce_editor_renderer_startup.h"
#endif
// Actual startup logger call node, counted without starting logger threads.
#define jce_log_write jce_test_gpu_preference_log_write
#include JCE_EDITOR_RENDERER_STARTUP_HEADER
#undef jce_log_write
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
unsigned load_calls, override_calls, gpu_calls, gpu_warnings, gpu_infos;
char gpu_message[1024] = {};
int selected_backend = -1;
std::string selected_gpu;
bool source_found;
unsigned resolve_calls, config_path_calls;
std::string selected_config_path;

bool counted_load(JceEditorConfig *config)
{
    ++load_calls;
    source_found = jce_editor_config_load(config);
    return source_found;
}
void JCE_CALL counted_override(int backend)
{
    ++override_calls;
    selected_backend = backend;
}
void JCE_CALL counted_gpu_preference(const char *name)
{
    ++gpu_calls;
    selected_gpu = name ? name : "";
}
bool counted_path(const char *name, char *out, size_t cap)
{
    ++resolve_calls;
    CHECK(name == nullptr);
    return jce_editor_dotjce_path(name, out, cap);
}
void JCE_CALL counted_config_path(const char *path)
{
    ++config_path_calls;
    selected_config_path = path ? path : "";
}
bool unavailable_path(const char *, char *, size_t) { return false; }
bool unterminated_path(const char *, char *out, size_t cap)
{
    std::memset(out, 'x', cap);
    return true;
}
void reset_ports()
{
    load_calls = override_calls = gpu_calls = gpu_warnings = gpu_infos = 0;
    gpu_message[0] = '\0';
    selected_gpu.clear();
    selected_backend = -1;
    source_found = false;
    resolve_calls = config_path_calls = 0;
    selected_config_path.clear();
}
void profile_env(const char *value)
{
#if JCE_PLATFORM_WINDOWS
    REQUIRE(_putenv_s("JCE_EDITOR_PROFILE_DIR", value ? value : "") == 0);
#else
    REQUIRE((value ? setenv("JCE_EDITOR_PROFILE_DIR", value, 1)
                   : unsetenv("JCE_EDITOR_PROFILE_DIR")) == 0);
#endif
}
struct Profile {
    JceTestOwnedDirectory directory{"_ut_editor_renderer_startup"};
    std::string root = directory.root, previous;
    bool had_previous;
    Profile()
    {
        const char *old = std::getenv("JCE_EDITOR_PROFILE_DIR");
        had_previous = old != nullptr;
        previous = old ? old : "";
        profile_env(root.c_str());
    }
    ~Profile()
    {
        /* Dirty test selections may flush only to this owned profile. */
        jce_editor_config_flush_now();
        profile_env(had_previous ? previous.c_str() : nullptr);
    }
};
}

extern "C" void JCE_CALL jce_test_gpu_preference_log_write(
    JceLogLevel level, const char *tag, const char *, int, const char *format, ...)
{
    CHECK(std::strcmp(tag, "editor_startup") == 0);
    if (level == JCE_LOG_LEVEL_WARN) ++gpu_warnings;
    else if (level == JCE_LOG_LEVEL_INFO) ++gpu_infos;
    else CHECK(false);
    va_list args;
    va_start(args, format);
    std::vsnprintf(gpu_message, sizeof(gpu_message), format, args);
    va_end(args);
}

TEST_CASE("startup consumes actual defaults and saved request before device creation")
{
    Profile profile;
    JceEditorConfig config;

    {
        INFO("control 1: fresh absent profile still requests platform default");
        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK_FALSE(source_found);
        CHECK(load_calls == 1);
        CHECK(override_calls == 1);
#if JCE_PLATFORM_WINDOWS
        CHECK(selected_backend == JCE_BACKEND_D3D11);
#else
        CHECK(selected_backend == JCE_BACKEND_AUTO);
#endif
        CHECK_FALSE(jce_editor_config_load(&config));
        CHECK(selected_backend == jce_editor_renderer_backend_from_name(config.renderer));
    }
    {
        INFO("control 2: cached missing-source config remains usable");
        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK_FALSE(source_found);
        CHECK(load_calls == 1);
        CHECK(override_calls == 1);
        CHECK(selected_backend == jce_editor_renderer_backend_from_name(config.renderer));
    }
    {
        INFO("control 3: saved explicit D3D12 request stays explicit");
        std::strcpy(config.renderer, "D3D12");
        REQUIRE(jce_editor_config_save(&config));
        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK(source_found);
        CHECK(load_calls == 1);
        CHECK(override_calls == 1);
        CHECK(selected_backend == JCE_BACKEND_D3D12);
    }
    {
        INFO("control 4: saved Auto is requested as Auto");
        std::strcpy(config.renderer, "Auto");
        REQUIRE(jce_editor_config_save(&config));
        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK(source_found);
        CHECK(load_calls == 1);
        CHECK(override_calls == 1);
        CHECK(selected_backend == JCE_BACKEND_AUTO);
    }
    {
        INFO("control 5: unknown saved name never becomes a different request");
        std::strcpy(config.renderer, "unknown");
        REQUIRE(jce_editor_config_save(&config));
        reset_ports();
        CHECK_FALSE(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK(source_found);
        CHECK(load_calls == 1);
        CHECK(override_calls == 0);
        CHECK(selected_backend == -1);
    }
    {
        INFO("control 6: existing aliases and public enum identities are retained");
        struct Alias { const char *name; JceRendererBackend backend; };
        const Alias aliases[] = {
            {nullptr, JCE_BACKEND_AUTO}, {"", JCE_BACKEND_AUTO}, {"aUtO", JCE_BACKEND_AUTO},
            {"D3D11", JCE_BACKEND_D3D11}, {"Direct3D11", JCE_BACKEND_D3D11},
            {"dx11", JCE_BACKEND_D3D11}, {"D3D12", JCE_BACKEND_D3D12},
            {"Direct3D12", JCE_BACKEND_D3D12}, {"DX12", JCE_BACKEND_D3D12},
            {"Vulkan", JCE_BACKEND_VULKAN}, {"vk", JCE_BACKEND_VULKAN},
            {"OpenGL", JCE_BACKEND_OPENGL}, {"gl", JCE_BACKEND_OPENGL},
            {"OpenGL ES", JCE_BACKEND_OPENGLES}, {"OpenGLES", JCE_BACKEND_OPENGLES},
            {"gles", JCE_BACKEND_OPENGLES}, {"Metal", JCE_BACKEND_METAL}
        };
        for (const Alias &alias : aliases)
            CHECK(jce_editor_renderer_backend_from_name(alias.name) == alias.backend);
        CHECK(jce_editor_renderer_backend_from_name("unknown") == -1);
        CHECK(jce_editor_renderer_backend_from_name("D3D11garbage") == -1);
    }
    {
        INFO("control 7: incomplete owner ports reject before config IO");
        reset_ports();
        CHECK_FALSE(jce_editor_apply_startup_renderer(nullptr, counted_override, counted_gpu_preference));
        CHECK_FALSE(jce_editor_apply_startup_renderer(counted_load, nullptr, counted_gpu_preference));
        CHECK_FALSE(jce_editor_apply_startup_renderer(counted_load, counted_override, nullptr));
        CHECK(load_calls == 0);
        CHECK(override_calls == 0);
        CHECK(gpu_calls == 0);
    }
}

TEST_CASE("Editor runtime INI binds to actual profile instead of the game config")
{
    Profile profile;
    const std::string runtime = profile.root + "/editor-runtime.ini";
    REQUIRE_FALSE(jce_fs_host_exists_file(runtime.c_str()));

    reset_ports();
    CHECK(jce_editor_bind_startup_config(counted_path, counted_config_path));
    CHECK(resolve_calls == 1);
    CHECK(config_path_calls == 1);
    CHECK(selected_config_path == runtime);
    CHECK_FALSE(jce_fs_host_exists_file(runtime.c_str()));

    /* Existing runtime choices stay the engine's own bytes; binding neither
     * copies a game INI nor overwrites its remembered window/backend values. */
    const char saved[] = "[window]\nwidth=1200\nheight=800\nfullscreen=false\n"
                         "[renderer]\nbackend_resolved=D3D11\n";
    REQUIRE(jce_fs_host_write_all(runtime.c_str(), saved, sizeof(saved) - 1));
    reset_ports();
    CHECK(jce_editor_bind_startup_config(counted_path, counted_config_path));
    CHECK(config_path_calls == 1);
    CHECK(selected_config_path == runtime);
    uint64_t read = 0, total = 0;
    void *bytes = jce_fs_host_read_capped(runtime.c_str(), sizeof(saved), &read, &total);
    REQUIRE(bytes != nullptr);
    CHECK(total == sizeof(saved) - 1);
    CHECK(read == total);
    CHECK(std::memcmp(bytes, saved, sizeof(saved) - 1) == 0);
    jce_fs_buffer_free(bytes);

    /* Actual platform HOME/.jce policy still works without an explicit root.
     * No preferences or runtime file is written to HOME by this control. */
    profile_env(nullptr);
    char home_root[1024] = {}, expected[512] = {};
    REQUIRE(jce_editor_dotjce_path(nullptr, home_root, sizeof(home_root)));
    REQUIRE(jce_editor_config_profile_path(home_root, "editor-runtime.ini", expected, sizeof(expected)));
    reset_ports();
    CHECK(jce_editor_bind_startup_config(counted_path, counted_config_path));
    CHECK(config_path_calls == 1);
    CHECK(selected_config_path == expected);
    CHECK(jce_path_is_absolute(selected_config_path.c_str()));
    profile_env(profile.root.c_str());
}

TEST_CASE("Invalid or oversized profile rejects without an engine config setter")
{
    Profile profile;
    const std::string oversized = profile.root + "/" + std::string(510, 'x');
    for (const std::string &invalid : {std::string("relative-profile"),
                                      std::string("D:relative-profile"), oversized}) {
        INFO(invalid);
        profile_env(invalid.c_str());
        reset_ports();
        CHECK_FALSE(jce_editor_bind_startup_config(counted_path, counted_config_path));
        CHECK(resolve_calls == 1);
        CHECK(config_path_calls == 0);
        CHECK(selected_config_path.empty());
    }
    profile_env(profile.root.c_str());
    reset_ports();
    CHECK_FALSE(jce_editor_bind_startup_config(nullptr, counted_config_path));
    CHECK_FALSE(jce_editor_bind_startup_config(counted_path, nullptr));
    CHECK_FALSE(jce_editor_bind_startup_config(unavailable_path, counted_config_path));
    CHECK_FALSE(jce_editor_bind_startup_config(unterminated_path, counted_config_path));
    CHECK(resolve_calls == 0);
    CHECK(config_path_calls == 0);
}

TEST_CASE("GPU vendor preference consumes only an explicitly saved existing UI-int key")
{
    Profile profile;
    JceEditorConfig config;
    // The real configuration owner caches one process-wide snapshot. The earlier
    // invalid-backend control deliberately saved "unknown"; selecting a new
    // profile does not clear that snapshot. Restore this case's valid-backend,
    // absent-GPU-setting precondition through the actual defaults/save owner.
    jce_editor_config_defaults(&config);
    REQUIRE(jce_editor_config_save(&config));
    int stored = -9;
    CHECK_FALSE(jce_editor_config_get_ui_int(&config, JCE_EDITOR_GPU_ADAPTER_KEY, &stored));
    reset_ports();
    CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
    CHECK(load_calls == 1);
    CHECK(override_calls == 1);
    CHECK(gpu_calls == 0);
    CHECK(selected_gpu.empty()); // Neither Auto nor another vendor overrides ENV.
    CHECK(gpu_warnings == 0);
    CHECK(gpu_infos == 0);

    // Editing a local mirror alone must not become a renderer request.
    REQUIRE(jce_editor_store_gpu_adapter_choice(&config, 1));
    reset_ports();
    CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
    CHECK(gpu_calls == 0);
    const char *names[] = {"auto", "intel", "nvidia", "amd"};
    for (int choice = 0; choice < 4; ++choice) {
        INFO(choice);
        REQUIRE(jce_editor_store_gpu_adapter_choice(&config, choice));
        REQUIRE(jce_editor_config_save(&config));
        jce_editor_config_flush_now();
        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK(load_calls == 1);
        CHECK(override_calls == 1);
        CHECK(gpu_calls == 1);
        CHECK(selected_gpu == names[choice]);
        CHECK(gpu_warnings == 0);
        CHECK(gpu_infos == 1);
        CHECK(std::strstr(gpu_message, "source=editor_saved") != nullptr);
        CHECK(std::strstr(gpu_message, "actual_selection=pending_driver") != nullptr);
        JceEditorConfig loaded;
        REQUIRE(jce_editor_config_load(&loaded));
        REQUIRE(jce_editor_config_get_ui_int(&loaded, JCE_EDITOR_GPU_ADAPTER_KEY, &stored));
        CHECK(stored == choice);

        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(
            counted_load, counted_override, counted_gpu_preference, "nvidia"));
        CHECK(gpu_calls == 1);
        CHECK(selected_gpu == "nvidia");
        CHECK(gpu_infos == 1);
        CHECK(std::strstr(gpu_message, "source=launch_environment") != nullptr);
        REQUIRE(jce_editor_config_load(&loaded));
        REQUIRE(jce_editor_config_get_ui_int(&loaded, JCE_EDITOR_GPU_ADAPTER_KEY, &stored));
        CHECK(stored == choice);
    }
    for (int invalid : {-1, 4, 65536}) {
        INFO(invalid);
        REQUIRE(jce_editor_config_set_ui_int(&config, JCE_EDITOR_GPU_ADAPTER_KEY, invalid));
        REQUIRE(jce_editor_config_save(&config));
        reset_ports();
        CHECK(jce_editor_apply_startup_renderer(counted_load, counted_override, counted_gpu_preference, nullptr));
        CHECK(override_calls == 1); // Bad optional setting cannot prevent editor startup.
        CHECK(gpu_calls == 0);
        CHECK(selected_gpu.empty());
        CHECK(gpu_warnings == 1); // One common-WARN call per startup attempt.
        CHECK(gpu_infos == 0);
        CHECK(std::strstr(gpu_message, "reason=unknown_choice") != nullptr);
        CHECK(std::strstr(gpu_message, "fallback=JCE_GPU_ADAPTER_or_driver_default") != nullptr);
    }
}

TEST_CASE("GPU preference authoring rejects invalid values and full UI-int storage")
{
    JceEditorConfig config;
    jce_editor_config_defaults(&config);
    CHECK_FALSE(jce_editor_store_gpu_adapter_choice(nullptr, 0));
    CHECK_FALSE(jce_editor_store_gpu_adapter_choice(&config, -1));
    CHECK_FALSE(jce_editor_store_gpu_adapter_choice(&config, 4));
    CHECK(config.ui_int_state_count == 0);
    for (int choice = 0; choice < 4; ++choice) {
        REQUIRE(jce_editor_store_gpu_adapter_choice(&config, choice));
        CHECK(config.ui_int_state_count == 1);
        int stored = -1;
        CHECK(jce_editor_config_get_ui_int(&config, JCE_EDITOR_GPU_ADAPTER_KEY, &stored));
        CHECK(stored == choice);
        CHECK(jce_editor_gpu_adapter_name(choice) != nullptr);
    }
    jce_editor_config_defaults(&config);
    for (int i = 0; i < JCE_EDITOR_UI_INT_STATE_MAX; ++i) {
        const std::string key = "fixture.gpu_capacity." + std::to_string(i);
        REQUIRE(jce_editor_config_set_ui_int(&config, key.c_str(), i));
    }
    CHECK_FALSE(jce_editor_store_gpu_adapter_choice(&config, 2));
    CHECK(config.ui_int_state_count == JCE_EDITOR_UI_INT_STATE_MAX);
    CHECK_FALSE(jce_editor_config_get_ui_int(&config, JCE_EDITOR_GPU_ADAPTER_KEY, nullptr));
}
