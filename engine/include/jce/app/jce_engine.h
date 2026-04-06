/*
 * jce_engine.h  Engine bootstrap and lifecycle.
 *
 * Owns all subsystems (window, renderer, audio, input, PAK, app).
 * main.c delegates every SDL callback to this module.
 */

#ifndef JCE_ENGINE_H
#define JCE_ENGINE_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceEngine  JceEngine;
typedef struct JceAppDesc JceAppDesc;

/* Register the application descriptor (IApp callbacks).
   Must be called before jce_engine_create().
   If not called, engine falls back to direct jce_app_* calls. */
void           jce_engine_set_app_desc(const JceAppDesc *desc);

/* Set optional config file path override used by jce_engine_create.
   Pass NULL or empty string to clear override. */
void           jce_engine_set_config_path(const char *path);

/* Create the engine: init logger, load config, open PAK, create
   window/renderer/audio/input, async-load assets, create app. */
JceEngine     *jce_engine_create(int argc, char *argv[]);

/* Route an SDL event to input + app. Returns SDL_APP_SUCCESS on quit. */
SDL_AppResult  jce_engine_event(JceEngine *e, const SDL_Event *event);

/* Run one frame: begin_frame, app_update, end_frame, input_update. */
SDL_AppResult  jce_engine_iterate(JceEngine *e);

/* Shut down everything in reverse order. */
void           jce_engine_destroy(JceEngine *e);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ENGINE_H */
