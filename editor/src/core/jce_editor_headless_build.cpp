/*
 * jce_editor_headless_build.cpp
 *
 * Adapts the validated headless request to the existing native project build
 * manager.  Asset cooking, authenticated PAK creation, CMake orchestration,
 * package staging, and Dist audit remain owned by that single implementation.
 */

#include "jce_editor_headless_build.h"

#include "jce_build_manager.h"
#include "jce_editor_headless_build_driver.h"
#include "jce_editor_headless_build_request.h"
#include "jce_editor_project.h"
#include "jce_editor_project_state.h"
#include "jce_project_settings.h"

extern "C" {
#include <jce/application/jce_engine.h>
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_log.h>
}

#include <cstdlib>
#include <string>

namespace {

constexpr const char *kEnvironmentFields[] = {
    "JCE_HEADLESS_BUILD_PROJECT",
    "JCE_HEADLESS_BUILD_SDK",
    "JCE_HEADLESS_BUILD_TARGET",
    "JCE_HEADLESS_BUILD_EXE",
    "JCE_HEADLESS_BUILD_VARIANT",
    "JCE_HEADLESS_BUILD_ARCH",
    "JCE_HEADLESS_BUILD_OUT",
    "JCE_HEADLESS_BUILD_RESULT",
    "JCE_HEADLESS_BUILD_CLEAN",
};

struct HeadlessBuildState {
    bool initialized = false;
    bool requested = false;
    bool quit_requested = false;
    JceEditorHeadlessBuildRequest request;
    JceEditorHeadlessBuildDriver driver;
    std::string forced_error;
};

HeadlessBuildState g_headless;

std::string join_bundles(const JceProject *project)
{
    std::string joined;
    if (!project || !project->bundles)
        return joined;
    for (int i = 0; i < project->bundles_count; ++i) {
        const char *bundle = project->bundles[i];
        if (!bundle || !bundle[0])
            continue;
        if (!joined.empty())
            joined.push_back(';');
        joined += bundle;
    }
    return joined;
}

bool start_build(void *,
                 const JceEditorHeadlessBuildRequest *request,
                 std::string *error)
{
    jce_editor_current_project_root_set(request->project.c_str());
    const char *root = jce_editor_current_project_root();
    jce_project_settings_set_root(root);
    jce_editor_project_set_root(root);
    jce_build_manager_set_default_working_dir(root);

    JceProjectSettings settings{};
    (void)jce_project_settings_load(&settings);

    const JceProject *project = jce_editor_project_get();
    if (!project) {
        if (error)
            *error = "cannot load jce_project.json";
        return false;
    }

    const std::string bundles = join_bundles(project);
    const std::string label = "headless-" + request->variant;

    JceBuildProjectConfig config{};
    config.label = label.c_str();
    config.project_dir = request->project.c_str();
    config.sdk_dir = request->sdk.c_str();
    config.target = request->target.c_str();
    config.exe_name = request->exe.c_str();
    config.variant = request->variant.c_str();
    config.arch = request->arch.c_str();
    config.cooked_assets =
        project->cooked_assets && project->cooked_assets[0]
            ? project->cooked_assets
            : nullptr;
    config.bundles = bundles.empty() ? nullptr : bundles.c_str();
    config.clean = request->clean;
    config.package_out_dir = request->package_out.c_str();
    config.app_name = project->name;
    config.app_version = project->version;

    if (jce_build_manager_start_project_build(&config))
        return true;

    JceBuildStatus status{};
    jce_build_manager_get_status(&status);
    if (error) {
        *error = status.last_error[0]
            ? status.last_error
            : "native project build did not start";
    }
    return false;
}

bool start_failure(void *,
                   const JceEditorHeadlessBuildRequest *,
                   std::string *error)
{
    if (error)
        *error = g_headless.forced_error;
    return false;
}

void get_status(void *, JceBuildStatus *out)
{
    jce_build_manager_get_status(out);
}

void request_quit(void *)
{
    g_headless.quit_requested = true;
    jce_engine_request_quit();
}

JceEditorHeadlessBuildDriverOps driver_ops(
    bool (*start)(void *, const JceEditorHeadlessBuildRequest *,
                  std::string *))
{
    JceEditorHeadlessBuildDriverOps ops{};
    ops.start = start;
    ops.get_status = get_status;
    ops.request_quit = request_quit;
    return ops;
}

} // namespace

bool jce_editor_headless_build_environment_present()
{
    for (const char *name : kEnvironmentFields) {
        const char *value = std::getenv(name);
        if (value && value[0])
            return true;
    }
    return false;
}

bool jce_editor_headless_build_initialize()
{
    if (g_headless.initialized)
        return g_headless.requested;

    g_headless = HeadlessBuildState{};
    g_headless.initialized = true;
    g_headless.requested = jce_editor_headless_build_environment_present();
    if (!g_headless.requested)
        return false;

    const JceEditorHeadlessBuildRequestState parsed =
        jce_editor_headless_build_request_parse_environment(
            &g_headless.request);
    if (parsed != JceEditorHeadlessBuildRequestState::Valid) {
        g_headless.forced_error = g_headless.request.error.empty()
            ? "invalid headless build request"
            : g_headless.request.error;
        LOG_ERROR("editor_headless_build", "%s",
                  g_headless.forced_error.c_str());
        (void)jce_editor_headless_build_driver_start(
            &g_headless.driver, &g_headless.request,
            driver_ops(start_failure), nullptr);
        return true;
    }

    LOG_INFO("editor_headless_build",
             "starting project=%s variant=%s arch=%s",
             g_headless.request.project.c_str(),
             g_headless.request.variant.c_str(),
             g_headless.request.arch.c_str());
    (void)jce_editor_headless_build_driver_start(
        &g_headless.driver, &g_headless.request,
        driver_ops(start_build), nullptr);
    return true;
}

void jce_editor_headless_build_poll()
{
    if (!g_headless.initialized || !g_headless.requested)
        return;
    jce_editor_headless_build_driver_poll(&g_headless.driver);
}

void jce_editor_headless_build_shutdown()
{
    if (jce_editor_headless_build_driver_active(&g_headless.driver))
        jce_build_manager_request_stop();
    jce_editor_project_set_root(nullptr);
    jce_project_settings_set_root(nullptr);
    jce_editor_current_project_root_set(nullptr);
    g_headless = HeadlessBuildState{};
}

bool jce_editor_headless_build_should_quit()
{
    return g_headless.quit_requested;
}
