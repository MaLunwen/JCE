/*
 * jce_app.h  Application lifecycle interface.
 *
 * All game/demo logic lives behind this interface.
 * main.c only calls create/destroy/update/event  it never
 * touches rendering primitives or game state directly.
 */

#ifndef JCE_APP_H
#define JCE_APP_H

#include <SDL3/SDL.h>
#include "graphics/jce_texture.h"
#include "audio/jce_audio.h"

/* Forward declarations  avoids pulling heavy headers into main.c. */
typedef struct JceWindow   JceWindow;
typedef struct JceInput    JceInput;
typedef struct JceRenderer JceRenderer;
typedef struct PakArchive  PakArchive;
typedef struct JceConfig   JceConfig;
typedef struct JceApp      JceApp;

typedef struct JcePreloadedAssets {
    JceTexture tex_demo;       /* "textures/texture.jpg" */
    JceTexture tex_cube;       /* "textures/chalet.jpg" */
    JceTexture tex_ground;     /* "textures/texture.jpg" (wrap) */
    JceSound   snd_bounce;     /* "sounds/bounce.wav" */
    JceSound   snd_music;      /* "sounds/Aria Math - C418.ogg" */
    bool       has_preloaded;  /* true if any above are valid */
} JcePreloadedAssets;

/* Subsystem handles, passed into app at creation time. */
typedef struct JceAppContext {
    JceWindow            *window;
    JceInput             *input;
    JceAudio             *audio;
    JceRenderer          *renderer;
    PakArchive           *pak;
    const JceConfig      *config;
    JcePreloadedAssets    preloaded;  /* optional pre-loaded assets */
} JceAppContext;

/* Create the app: load assets, set icon, start music, etc.
   The context is copied internally  caller need not keep it alive. */
JceApp *jce_app_create(const JceAppContext *ctx);

/* Destroy the app: release app-owned resources. */
void    jce_app_destroy(JceApp *app);

/* Called once per frame between renderer begin/end. */
void    jce_app_update(JceApp *app);

/* Called for each SDL event (after input system has processed it). */
void    jce_app_event(JceApp *app, const SDL_Event *event);

/* Returns true when the app has requested to quit (e.g. ESC pressed). */
bool    jce_app_should_quit(const JceApp *app);

#endif /* JCE_APP_H */
