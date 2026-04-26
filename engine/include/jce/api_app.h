/*
 * api_app.h  Layer 6 — Application.
 *
 * Engine lifecycle, application interface, configuration,
 * camera controller, entry point, subsystem registry, screenshots.
 */

#ifndef JCE_API_APP_H
#define JCE_API_APP_H

#include <jce/application/jce_engine.h>
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_config.h>
#include <jce/application/jce_camera_controller.h>
#include <jce/application/jce_subsystem.h>
#include <jce/application/jce_screenshot.h>

/* NOTE: jce_main.h is intentionally excluded.
 * It defines SDL_MAIN_USE_CALLBACKS and generates entry-point functions
 * via the JCE_MAIN() macro — it must be included in exactly ONE .c file
 * (the application entry point), never from an umbrella header. */

#endif /* JCE_API_APP_H */
