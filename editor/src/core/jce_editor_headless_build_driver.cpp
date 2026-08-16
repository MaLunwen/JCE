/*
 * jce_editor_headless_build_driver.cpp
 *
 * Publishes exactly one terminal JSON result for a headless build.  Result
 * files use the host filesystem's sibling-temp atomic replacement helper.
 */

#include "jce_editor_headless_build_driver.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
}

#include <cstdio>
#include <cstring>

namespace {

const char *state_name(JceBuildState state)
{
    switch (state) {
    case JCE_BUILD_IDLE:
        return "idle";
    case JCE_BUILD_RUNNING:
        return "running";
    case JCE_BUILD_SUCCEEDED:
        return "succeeded";
    case JCE_BUILD_FAILED:
        return "failed";
    }
    return "failed";
}

const char *stage_name(JceBuildStage stage)
{
    switch (stage) {
    case JCE_BUILD_STAGE_NONE:
        return "none";
    case JCE_BUILD_STAGE_CONFIGURE:
        return "configure";
    case JCE_BUILD_STAGE_COMPILE:
        return "compile";
    case JCE_BUILD_STAGE_CONAN_INSTALL:
        return "conan_install";
    case JCE_BUILD_STAGE_PREPARE_ASSETS:
        return "prepare_assets";
    }
    return "unknown";
}

bool write_result(const JceEditorHeadlessBuildRequest &request,
                  const JceBuildStatus &status)
{
    JceJson *root = jce_json_object();
    if (!root)
        return false;

    jce_json_set_string(root, "schema",
                        "jce.editor.headless-build-result.v1");
    jce_json_set_string(root, "state", state_name(status.state));
    jce_json_set_string(root, "stage", stage_name(status.stage));
    jce_json_set_int(root, "exit_code", status.exit_code);
    jce_json_set_string(root, "error", status.last_error);
    jce_json_set_string(root, "artifact_path", status.artifact_path);
    jce_json_set_string(root, "package_path", status.package_path);
    jce_json_set_string(root, "asset_bom_path", status.asset_bom_path);
    jce_json_set_string(root, "dist_audit_path", status.dist_audit_path);

    char *text = jce_json_print(root, true);
    jce_json_free(root);
    if (!text)
        return false;

    const bool ok = jce_fs_host_write_all_atomic(
        request.result_path.c_str(), text,
        static_cast<uint64_t>(std::strlen(text)));
    jce_json_free_string(text);
    return ok;
}

void finish(JceEditorHeadlessBuildDriver *driver,
            const JceBuildStatus &status)
{
    (void)write_result(driver->request, status);
    driver->active = false;
    if (driver->ops.request_quit)
        driver->ops.request_quit(driver->user);
}

} // namespace

bool jce_editor_headless_build_driver_start(
    JceEditorHeadlessBuildDriver *driver,
    const JceEditorHeadlessBuildRequest *request,
    JceEditorHeadlessBuildDriverOps ops,
    void *user)
{
    if (!driver || !request)
        return false;

    *driver = JceEditorHeadlessBuildDriver{};
    driver->request = *request;
    driver->ops = ops;
    driver->user = user;

    std::string error;
    if (!ops.start || !ops.get_status || !ops.request_quit ||
        !ops.start(user, request, &error)) {
        JceBuildStatus status{};
        status.state = JCE_BUILD_FAILED;
        status.stage = JCE_BUILD_STAGE_NONE;
        status.exit_code = -1;
        if (error.empty())
            error = "headless build host operations are unavailable";
        std::snprintf(status.last_error, sizeof(status.last_error), "%s",
                      error.c_str());
        finish(driver, status);
        return false;
    }

    driver->active = true;
    return true;
}

void jce_editor_headless_build_driver_poll(
    JceEditorHeadlessBuildDriver *driver)
{
    if (!driver || !driver->active)
        return;

    JceBuildStatus status{};
    driver->ops.get_status(driver->user, &status);
    if (status.state != JCE_BUILD_SUCCEEDED &&
        status.state != JCE_BUILD_FAILED) {
        return;
    }
    finish(driver, status);
}

bool jce_editor_headless_build_driver_active(
    const JceEditorHeadlessBuildDriver *driver)
{
    return driver && driver->active;
}
