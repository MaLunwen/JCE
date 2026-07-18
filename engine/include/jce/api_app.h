/*
 * api_app.h  Layer 6 — Application.
 *
 * Engine lifecycle, application interface, configuration,
 * camera controller, entry point, subsystem registry, screenshots.
 */

#ifndef JCE_API_APP_H
#define JCE_API_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_args.h>
#include <jce/application/jce_camera_controller.h>
#include <jce/os/core/jce_config.h>
#include <jce/application/jce_engine.h>
#include <jce/application/jce_lifecycle.h>
#include <jce/application/jce_project.h>
#include <jce/application/jce_runtime_boot.h>
#include <jce/application/jce_cook.h>
#include <jce/application/jce_runtime.h>
#include <jce/application/jce_screenshot.h>
#include <jce/application/jce_subsystem.h>

/* NOTE: jce_main.h is intentionally excluded.
 * It defines SDL_MAIN_USE_CALLBACKS and generates entry-point functions
 * via the JCE_MAIN() macro — it must be included in exactly ONE .c file
 * (the application entry point), never from an umbrella header. */



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_APP_H */
