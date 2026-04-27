/*
 * jce_engine.h  Engine bootstrap and lifecycle.
 *
 * Owns all subsystems (window, renderer, audio, input, PAK, app).
 * The application entry point delegates every callback to this module.
 *
 * This header is SDL-free; game/application code does not need SDL.
 */

#ifndef JCE_ENGINE_H
#define JCE_ENGINE_H


#include <jce/os/core/jce_defs.h>
JCE_EXTERN_C_BEGIN

/* Application result codes (match SDL_AppResult values). */
typedef enum JceAppResult {
    JCE_APP_CONTINUE = 0,   /* Keep running */
    JCE_APP_SUCCESS  = 1,   /* Quit successfully */
    JCE_APP_FAILURE  = 2    /* Quit with error */
} JceAppResult;

typedef struct JceEngine  JceEngine;
typedef struct JceAppDesc JceAppDesc;

/* Register the application descriptor (IApp callbacks).
   Must be called before jce_engine_create().
   If not called, engine falls back to direct jce_app_* calls. */
JCE_API void JCE_CALL jce_engine_set_app_desc(const JceAppDesc *desc);

/* Set optional config file path override used by jce_engine_create.
   Pass NULL or empty string to clear override. */
JCE_API void JCE_CALL jce_engine_set_config_path(const char *path);

/* Set optional PAK file path for JNI desktop mode.
   Pass NULL or empty string to clear override.
   When set, jce_engine_create() loads the PAK from this file
   instead of embedding it in the binary. */
JCE_API void JCE_CALL jce_engine_set_pak_path(const char *path);

/* Override the renderer backend selection.
   Must be called before jce_engine_create().
   Pass JCE_BACKEND_AUTO (0) to clear override and use config file value.
   Requires jce_config.h for JceRendererBackend enum. */
JCE_API void JCE_CALL jce_engine_set_renderer_override(int backend);

/* Create the engine: init logger, load config, open PAK, create
   window/renderer/audio/input, async-load assets, create app. */
JCE_API JceEngine     *jce_engine_create(int argc, char *argv[]);

/* Route a platform event to input + app.
   Returns JCE_APP_SUCCESS on quit event.
   The event pointer is backend-specific (SDL_Event* internally). */
JCE_API JceAppResult JCE_CALL jce_engine_event(JceEngine *e, const void *event);

/* Run one frame: begin_frame, app_update, end_frame, input_update. */
JCE_API JceAppResult JCE_CALL jce_engine_iterate(JceEngine *e);

/* Shut down everything in reverse order. */
JCE_API void JCE_CALL jce_engine_destroy(JceEngine *e);

JCE_EXTERN_C_END

#endif /* JCE_ENGINE_H */
