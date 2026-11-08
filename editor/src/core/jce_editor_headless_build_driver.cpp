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
#include <jce/os/core/jce_log.h>
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

struct BuildLogReceipt {
    std::string path, error;
    const char *status = "unavailable";
    size_t bytes = 0;
    bool truncated = false, incomplete = false, complete = false;
};

BuildLogReceipt write_build_log(const JceEditorHeadlessBuildDriver &driver)
{
    BuildLogReceipt log;
    if (!driver.ops.get_log) { log.error = "log snapshot callback unavailable"; return log; }
    JceBuildLogView view{};
    driver.ops.get_log(driver.user, &view);
    log.truncated = view.truncated;
    log.incomplete = view.incomplete;
    if (!view.available || !view.text || !view.size || view.size > JCE_BUILD_LOG_CAPACITY) {
        log.error = "build owner snapshot unavailable, empty or outside capacity";
        return log;
    }
    const std::string candidate = driver.request.result_path + ".build.log";
    if (!jce_fs_host_write_all_atomic(candidate.c_str(), view.text, view.size)) {
        log.status = "write_failed";
        log.error = "host_atomic_write: " + candidate;
        return log; // An old or partial destination is not current evidence.
    }
    log.path = candidate;
    log.bytes = view.size;
    log.status = "written";
    log.complete = !log.truncated && !log.incomplete;
    if (!log.complete) log.error = "owner transcript truncated or has an incomplete observed line";
    return log;
}

bool write_result(const JceEditorHeadlessBuildRequest &request,
                  const JceBuildStatus &status, const BuildLogReceipt &log,
                  const char **failure)
{
    JceJson *root = jce_json_object();
    if (!root) {
        *failure = "json_object_allocation";
        return false;
    }

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
    jce_json_set_string(root, "log_format", "jce.build.owner-observed-lines.v1");
    jce_json_set_string(root, "log_path", log.path.c_str());
    jce_json_set_string(root, "log_status", log.status);
    jce_json_set_string(root, "log_error", log.error.c_str());
    jce_json_set_int(root, "log_bytes", static_cast<int>(log.bytes));
    jce_json_set_bool(root, "log_complete", log.complete);
    jce_json_set_bool(root, "log_truncated", log.truncated);
    jce_json_set_bool(root, "log_incomplete", log.incomplete);

    char *text = jce_json_print(root, true);
    jce_json_free(root);
    if (!text) {
        *failure = "json_serialization";
        return false;
    }

    const bool ok = jce_fs_host_write_all_atomic(
        request.result_path.c_str(), text,
        static_cast<uint64_t>(std::strlen(text)));
    jce_json_free_string(text);
    if (!ok) *failure = "host_atomic_write";
    return ok;
}

void finish(JceEditorHeadlessBuildDriver *driver,
            const JceBuildStatus &status, const char *preflight_failure = nullptr)
{
    driver->result = status;
    if (driver->result.state != JCE_BUILD_SUCCEEDED && driver->result.exit_code == 0)
        driver->result.exit_code = 1;
    const char *failure = preflight_failure;
    BuildLogReceipt log;
    // Only an accepted current start owns a snapshot. A rejected start may
    // leave a previous manager transcript alive; never certify it as this run.
    if (!failure && driver->active) log = write_build_log(*driver);
    else log.error = failure
        ? "receipt preflight failed before current-build log capture"
        : "current build start was not accepted; prior snapshot is not evidence";
    if (!log.complete)
        jce_log_write(JCE_LOG_LEVEL_WARN, "editor_headless_build", __FILE__, __LINE__,
            "build log status=%s complete=0 truncated=%u incomplete=%u reason=%s",
            log.status, (unsigned)log.truncated, (unsigned)log.incomplete, log.error.c_str());
    driver->result_published = !failure &&
        write_result(driver->request, driver->result, log, &failure);
    if (!driver->result_published) {
        driver->result.state = JCE_BUILD_FAILED;
        driver->result.exit_code = 1;
        std::snprintf(driver->result.last_error, sizeof(driver->result.last_error),
                      "receipt publication failed (%s): %s",
                      failure ? failure : "unknown", driver->request.result_path.c_str());
        /* The configured receipt is owned by this invocation. A stale PASS
         * must not survive a failed publish; never recurse into a directory. */
        const bool cleared = !driver->request.result_path.empty() &&
            jce_fs_host_remove_file(driver->request.result_path.c_str());
        LOG_ERROR("editor_headless_build",
                  "phase=result_publish_failed path=%s reason=%s stale_cleared=%u "
                  "build_state=%s build_exit=%d build_error=%s",
                  driver->request.result_path.c_str(), failure ? failure : "unknown",
                  (unsigned)cleared, state_name(status.state), status.exit_code,
                  status.last_error);
    }
    driver->completed = true;
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

    /* Clear the previous terminal receipt BEFORE starting any project work.
     * Missing files are accepted by the public host helper. A directory or
     * an old receipt that cannot be cleared is a preflight failure. */
    if (request->result_path.empty() ||
        !jce_fs_host_remove_file(request->result_path.c_str())) {
        JceBuildStatus status{};
        status.state = JCE_BUILD_FAILED;
        status.exit_code = 1;
        finish(driver, status, "result_preflight_reset");
        return false;
    }

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

bool jce_editor_headless_build_driver_get_result(
    const JceEditorHeadlessBuildDriver *driver, JceBuildStatus *out)
{
    if (!driver || !out || !driver->completed) return false;
    *out = driver->result;
    return true;
}

JceAppResult jce_editor_headless_build_driver_app_result(
    const JceEditorHeadlessBuildDriver *driver)
{
    return driver && driver->completed && driver->result_published &&
        driver->result.state == JCE_BUILD_SUCCEEDED && driver->result.exit_code == 0
        ? JCE_APP_SUCCESS : JCE_APP_FAILURE;
}
