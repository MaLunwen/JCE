/*
 * jce_app_interface.h  Application interface (IApp pattern).
 *
 * Defines the contract between engine and game code.
 * The game provides a JceAppDesc containing lifecycle callbacks;
 * the engine provides a JceServices struct with subsystem handles.
 *
 * Inspired by The-Forge's IApp interface.
 * Layer: Application (Layer 5).
 *
 * This header is SDL-free; game/application code does not need SDL.
 */

#ifndef JCE_APP_INTERFACE_H
#define JCE_APP_INTERFACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare subsystems so game headers need not pull them in. */
typedef struct JceWindow       JceWindow;
typedef struct JceInput        JceInput;
typedef struct JceAudio        JceAudio;
typedef struct JceRenderer     JceRenderer;
typedef struct PakArchive      PakArchive;
typedef struct JceConfig       JceConfig;
typedef struct JceAssetManager JceAssetManager;

/* ================================================================== */
/* Services provided by the engine to the application                  */
/* ================================================================== */

typedef struct JceServices {
    JceWindow         *window;
    JceInput          *input;
    JceAudio          *audio;
    JceRenderer       *renderer;
    PakArchive        *pak;
    const JceConfig   *config;
    JceAssetManager   *assets;
} JceServices;

/* ================================================================== */
/* Application descriptor (IApp vtable)                                */
/* ================================================================== */

typedef struct JceAppDesc {
    /* Application name (shown in title bar, logs). */
    const char *name;

    /* Called once after all subsystems are ready.
       svc is valid for the lifetime of the application.
       Return true on success, false to abort startup. */
    bool (*init)(const JceServices *svc, void *user_data);

    /* Called once before subsystems are torn down. */
    void (*exit)(void *user_data);

    /* Called once per frame. dt = seconds since last frame. */
    void (*update)(float dt, void *user_data);

    /* Called between renderer begin_frame / end_frame.
       Perform all draw/submit calls here. */
    void (*draw)(const JceServices *svc, void *user_data);

    /* Called for each platform event (after input system processes it).
       The event pointer is backend-specific (SDL_Event* internally).
       Most apps should use JceInput queries instead, leaving this NULL. */
    void (*on_event)(const void *platform_event, void *user_data);

    /* Optional: called on window resize. NULL = ignored. */
    void (*on_resize)(uint32_t w, uint32_t h, void *user_data);

    /* Optional: return true when app wants to quit. */
    bool (*should_quit)(void *user_data);

    /* Opaque pointer passed to all callbacks.
       Typically points to game state struct. */
    void *user_data;

    /* Start the window maximized. Default: false. */
    bool  maximized;

    /* Default window dimensions. 0 = use config default. */
    uint32_t window_width;
    uint32_t window_height;
} JceAppDesc;

#ifdef __cplusplus
}
#endif

#endif /* JCE_APP_INTERFACE_H */
