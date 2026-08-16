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
#include "jce_editor_headless_build_request.h"

#include <string>

struct JceEditorHeadlessBuildDriverOps {
    bool (*start)(void *user,
                  const JceEditorHeadlessBuildRequest *request,
                  std::string *error) = nullptr;
    void (*get_status)(void *user, JceBuildStatus *out) = nullptr;
    void (*request_quit)(void *user) = nullptr;
};

struct JceEditorHeadlessBuildDriver {
    JceEditorHeadlessBuildRequest request;
    JceEditorHeadlessBuildDriverOps ops;
    void *user = nullptr;
    bool active = false;
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

#endif /* JCE_EDITOR_HEADLESS_BUILD_DRIVER_H */
