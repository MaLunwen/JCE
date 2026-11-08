/*
 * jce_editor_headless_build_driver.h
 *
 * UI-free lifecycle driver for same-executable editor builds.  The host
 * supplies build-manager operations; the driver owns terminal result
 * publication and process-exit notification.
 */

#ifndef JCE_EDITOR_HEADLESS_BUILD_DRIVER_H
#define JCE_EDITOR_HEADLESS_BUILD_DRIVER_H

#include "jce_build_manager.h"
#include <jce/application/jce_engine.h>
#include "jce_editor_headless_build_request.h"

#include <string>

struct JceEditorHeadlessBuildDriverOps {
    bool (*start)(void *user,
                  const JceEditorHeadlessBuildRequest *request,
                  std::string *error) = nullptr;
    void (*get_status)(void *user, JceBuildStatus *out) = nullptr;
    void (*get_log)(void *user, JceBuildLogView *out) = nullptr;
    void (*request_quit)(void *user) = nullptr;
};

struct JceEditorHeadlessBuildDriver {
    JceEditorHeadlessBuildRequest request;
    JceEditorHeadlessBuildDriverOps ops;
    void *user = nullptr;
    bool active = false;
    bool completed = false;
    bool result_published = false;
    JceBuildStatus result{};
};

bool jce_editor_headless_build_driver_start(
    JceEditorHeadlessBuildDriver *driver,
    const JceEditorHeadlessBuildRequest *request,
    JceEditorHeadlessBuildDriverOps ops,
    void *user);

void jce_editor_headless_build_driver_poll(
    JceEditorHeadlessBuildDriver *driver);

bool jce_editor_headless_build_driver_active(
    const JceEditorHeadlessBuildDriver *driver);

/* False until a terminal attempt; publication failures are FAILED with a
 * nonzero overall exit code even if the compiler/build manager succeeded. */
bool jce_editor_headless_build_driver_get_result(
    const JceEditorHeadlessBuildDriver *driver, JceBuildStatus *out);

/* The host consumes this exact decision through request_quit_with_result. */
JceAppResult jce_editor_headless_build_driver_app_result(
    const JceEditorHeadlessBuildDriver *driver);

#endif /* JCE_EDITOR_HEADLESS_BUILD_DRIVER_H */
