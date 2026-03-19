/*
 * jce_engine.c  Engine bootstrap and lifecycle implementation.
 *
 * Centralises all subsystem creation, async asset preloading,
 * event routing, and per-frame orchestration that previously
 * lived in main.c.
 */

#include "jce_engine.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "jce_log.h"
#include "jce_config.h"
#include "jce_gpu_caps.h"
#include "platform/jce_window.h"
#include "platform/jce_input.h"
#include "audio/jce_audio.h"
#include "renderer/jce_renderer.h"
#include "renderer/jce_texture.h"
#include "resource/pak_loader.h"
#include "resource/jce_async_loader.h"
#include "game/jce_app.h"
#include "embedded_assets.h"

#include <bgfx/c99/bgfx.h>

#define LOG_TAG "engine"

static char g_config_path_override[512];

void jce_engine_set_config_path(const char *path)
{
    if (!path || !path[0]) {
        g_config_path_override[0] = '\0';
        return;
    }

    snprintf(g_config_path_override, sizeof(g_config_path_override), "%s", path);
}

static bool jce_path_exists(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    fclose(fp);
    return true;
}

static void jce_select_config_path(char *out_path, size_t out_size)
{
    const char *cwd_cfg = ".config/jce.ini";
    const char *base = SDL_GetBasePath();

    out_path[0] = '\0';

    if (g_config_path_override[0]) {
        snprintf(out_path, out_size, "%s", g_config_path_override);
        return;
    }

    if (jce_path_exists(cwd_cfg)) {
        snprintf(out_path, out_size, "%s", cwd_cfg);
        return;
    }

    snprintf(out_path, out_size, "%s.config/jce.ini", base ? base : "");
}

/* -- Engine state -------------------------------------------------- */

struct JceEngine {
    JceConfig    config;
    JceGpuCaps   gpu_caps;
    JceWindow   *window;
    JceInput    *input;
    JceAudio    *audio;
    JceRenderer *renderer;
    PakArchive  *pak;
    JceApp      *app;
    bool         backgrounded;  /* true while app is in background */
};

/* -- Fatal error dialog (all platforms) ----------------------------- */

static void fatal_msg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG_ERROR(LOG_TAG, "%s", buf);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "JCE Fatal Error", buf, NULL);
}

/* -- Loading screen helper ----------------------------------------- */

static void render_loading_frame(JceRenderer *r, JceWindow *win,
                                  const char *msg)
{
    jce_renderer_begin_frame(r, win);
    bgfx_dbg_text_printf(2, 2, 0x0f, "%s", msg);
    jce_renderer_end_frame(r);
}

/* -- Create -------------------------------------------------------- */

JceEngine *jce_engine_create(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Logger + config. */
    jce_log_init();
    jce_log_set_thread_name("MAIN");

    JceEngine *e = SDL_calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->config = jce_config_defaults();
    {
        char cfg_path[512];
        jce_select_config_path(cfg_path, sizeof(cfg_path));
        jce_config_load(&e->config, cfg_path);
    }
    jce_log_set_level((JceLogLevel)e->config.log_level);
    jce_log_set_colors(e->config.log_colors);

    SDL_SetAppMetadata("JCE", "0.1.0", "com.jce");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fatal_msg("SDL_Init failed: %s", SDL_GetError());
        SDL_free(e);
        return NULL;
    }

    /* Force landscape on mobile / mobile-web. */
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IOS || TARGET_OS_TV
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  #endif
#endif
#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
    EM_ASM({
        if (screen.orientation && screen.orientation.lock)
            screen.orientation.lock('landscape').catch(function(){});
    });
#endif

    /* Open embedded PAK archive. */
    e->pak = pak_open(assets_pak_data, assets_pak_data_size);
    if (!e->pak) {
        fatal_msg("Failed to open embedded PAK archive");
        SDL_free(e);
        return NULL;
    }

    /* -- Start async asset loading immediately -------------------- */

    JceAsyncTask *task_tex_demo   = jce_async_load_texture(e->pak,
        "textures/texture.jpg", JCE_TEX_CLAMP);
    JceAsyncTask *task_tex_cube   = jce_async_load_texture(e->pak,
        "textures/chalet.jpg", JCE_TEX_CLAMP);
    JceAsyncTask *task_tex_ground = jce_async_load_texture(e->pak,
        "textures/texture.jpg", JCE_TEX_WRAP);
    JceAsyncTask *task_snd_bounce = jce_async_load_audio(e->pak,
        "sounds/bounce.wav");
    JceAsyncTask *task_snd_music  = jce_async_load_audio(e->pak,
        "sounds/Aria Math - C418.ogg");

    /* -- Window + renderer (main thread, overlaps with workers) --- */

    JceWindowConfig win_cfg = {
        .title     = e->config.window_title,
        .logical_w = e->config.window_width,
        .logical_h = e->config.window_height,
        .flags     = (e->config.resizable  ? SDL_WINDOW_RESIZABLE  : 0)
                   | (e->config.fullscreen ? SDL_WINDOW_FULLSCREEN : 0)
    };
    e->window = jce_window_create(&win_cfg);
    if (!e->window) {
        fatal_msg("Window creation failed: %s", SDL_GetError());
        goto fail;
    }

    JceRendererConfig ren_cfg = {
        .backend    = (int)e->config.renderer_backend,
        .vsync      = e->config.vsync,
        .debug_text = e->config.debug_text,
        .clear_color = e->config.clear_color
    };
    e->renderer = jce_renderer_create(e->window, e->pak, &ren_cfg);
    if (!e->renderer) {
        fatal_msg("Renderer initialization failed (bgfx)");
        goto fail;
    }

    /* Show loading screen while workers finish. */
    render_loading_frame(e->renderer, e->window, "Loading...");

    jce_gpu_caps_init(&e->gpu_caps);

    e->input = jce_input_create();
    if (!e->input) {
        fatal_msg("Input system initialization failed");
        goto fail;
    }

    e->audio = jce_audio_create();
    if (!e->audio)
        LOG_WARN(LOG_TAG, "audio init failed  continuing without sound");

    if (e->audio && e->config.master_volume < 1.0f)
        jce_audio_set_master_volume(e->audio, e->config.master_volume);

    /* -- Finalize async tasks (wait + create GPU/AL resources) ---- */

    JcePreloadedAssets preloaded = {0};
    preloaded.has_preloaded = true;

    while (!(jce_async_task_done(task_tex_demo) &&
             jce_async_task_done(task_tex_cube) &&
             jce_async_task_done(task_tex_ground) &&
             jce_async_task_done(task_snd_bounce) &&
             jce_async_task_done(task_snd_music))) {
        SDL_Delay(1);
    }

    preloaded.tex_demo   = jce_async_finalize_texture(task_tex_demo);
    preloaded.tex_cube   = jce_async_finalize_texture(task_tex_cube);
    preloaded.tex_ground = jce_async_finalize_texture(task_tex_ground);

    if (e->audio) {
        preloaded.snd_bounce = jce_async_finalize_audio(task_snd_bounce,
                                                         e->audio);
        preloaded.snd_music  = jce_async_finalize_audio(task_snd_music,
                                                         e->audio);
    }

    jce_async_task_free(task_tex_demo);
    jce_async_task_free(task_tex_cube);
    jce_async_task_free(task_tex_ground);
    jce_async_task_free(task_snd_bounce);
    jce_async_task_free(task_snd_music);

    /* -- Create app with pre-loaded assets ----------------------- */

    JceAppContext ctx = {
        .window    = e->window,
        .input     = e->input,
        .audio     = e->audio,
        .renderer  = e->renderer,
        .pak       = e->pak,
        .config    = &e->config,
        .preloaded = preloaded
    };
    e->app = jce_app_create(&ctx);
    if (!e->app) goto fail;

    return e;

fail:
    jce_engine_destroy(e);
    return NULL;
}

/* -- Event routing ------------------------------------------------- */

SDL_AppResult jce_engine_event(JceEngine *e, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    jce_input_handle_event(e->input, event);

    if (event->type == SDL_EVENT_WINDOW_RESIZED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int pw, ph;
        SDL_GetWindowSizeInPixels(jce_window_sdl(e->window), &pw, &ph);
        jce_window_handle_resize(e->window, (uint32_t)pw, (uint32_t)ph);
        jce_renderer_resize(e->renderer, (uint32_t)pw, (uint32_t)ph);
    }

    /* App lifecycle: background / foreground transitions. */
    if (event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        e->backgrounded = true;
        LOG_INFO(LOG_TAG, "entered background");
    }
    if (event->type == SDL_EVENT_WILL_ENTER_FOREGROUND ||
        event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        if (e->backgrounded) {
            e->backgrounded = false;
            /* Re-bind native window handle (Android recreates ANativeWindow)
               then reset bgfx rendering context. */
            jce_renderer_rebind_platform(e->renderer, e->window);
            LOG_INFO(LOG_TAG, "returned to foreground, rebound platform data");
        }
    }

    jce_app_event(e->app, event);

    return SDL_APP_CONTINUE;
}

/* -- Per-frame ----------------------------------------------------- */

SDL_AppResult jce_engine_iterate(JceEngine *e)
{
    if (jce_app_should_quit(e->app))
        return SDL_APP_SUCCESS;

    /* Skip rendering entirely while backgrounded (no valid surface). */
    if (e->backgrounded)
        return SDL_APP_CONTINUE;

    jce_renderer_begin_frame(e->renderer, e->window);
    jce_app_update(e->app);
    jce_renderer_end_frame(e->renderer);
    jce_input_update(e->input);

    return SDL_APP_CONTINUE;
}

/* -- Shutdown ------------------------------------------------------ */

void jce_engine_destroy(JceEngine *e)
{
    if (!e) return;
    if (e->app)      jce_app_destroy(e->app);
    if (e->renderer)  jce_renderer_destroy(e->renderer);
    if (e->audio)     jce_audio_destroy(e->audio);
    if (e->pak)       pak_close(e->pak);
    if (e->input)     jce_input_destroy(e->input);
    if (e->window)    jce_window_destroy(e->window);
    SDL_free(e);
}
