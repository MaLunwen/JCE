/*
 * jce_engine.c  Engine bootstrap and lifecycle implementation.
 *
 * Centralises all subsystem creation, event routing, and
 * per-frame orchestration.  Asset loading is delegated to
 * the application via JceAppDesc callbacks.
 */

#include <jce/app/jce_engine.h>
#include <jce/app/jce_app_interface.h>

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <jce/core/jce_log.h>
#include <jce/core/jce_crash_handler.h>
#include <jce/app/jce_config.h>
#include "core/jce_gpu_caps.h"
#include <jce/platform/jce_window.h>
#include <jce/platform/jce_input.h>
#include <jce/audio/jce_audio.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/resource/pak_loader.h>
#include "embedded_assets.h"

#define LOG_TAG "engine"

static JceAppDesc  g_app_desc;
static bool        g_app_desc_set;
static char        g_config_path_override[512];

void jce_engine_set_app_desc(const JceAppDesc *desc)
{
    if (desc) {
        g_app_desc = *desc;
        g_app_desc_set = true;
    } else {
        g_app_desc_set = false;
    }
}

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
    JceServices  svc;           /* subsystem handles for IApp */
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

static void render_loading_frame(const JceRenderer *r, JceWindow *win,
                                  const char *msg)
{
    jce_renderer_begin_frame(r, win);
    jce_renderer_dbg_text(2, 2, 0x0f, "%s", msg);
    jce_renderer_end_frame(r);
}

/* -- Create -------------------------------------------------------- */

JceEngine *jce_engine_create(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Logger + crash handler + config. */
    jce_log_init();
    jce_log_set_thread_name("MAIN");
    jce_crash_handler_init();

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

    /* Open PAK archive. */
#if defined(__EMSCRIPTEN__)
    e->pak = pak_open_file("/game_assets.pak");
#elif defined(__ANDROID__)
    /* Android: load PAK from APK assets/ dir at runtime.
     * Embedding 290 MB in .rodata causes SEGV_ACCERR on Houdini
     * ARM64-to-x86_64 translator (WSA). */
    {
        SDL_IOStream *io = SDL_IOFromFile("game_assets.pak", "rb");
        if (io) {
            Sint64 pak_sz = SDL_GetIOSize(io);
            if (pak_sz > 0) {
                void *pak_buf = SDL_malloc((size_t)pak_sz);
                if (pak_buf) {
                    if (SDL_ReadIO(io, pak_buf, (size_t)pak_sz) == (size_t)pak_sz) {
                        e->pak = pak_open_owned(pak_buf, (size_t)pak_sz);
                    }
                    if (!e->pak) SDL_free(pak_buf);
                }
            }
            SDL_CloseIO(io);
        }
    }
#else
    e->pak = pak_open(assets_pak_data, assets_pak_data_size);
#endif
    if (!e->pak) {
        fatal_msg("Failed to open PAK archive");
        SDL_free(e);
        return NULL;
    }

    /* -- Window ------------------------------------------------------- */

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

    /* -- Renderer ------------------------------------------------- */

    JceRendererConfig ren_cfg = {
        .backend     = (int)e->config.renderer_backend,
        .vsync       = e->config.vsync,
        .debug_text  = e->config.debug_text,
        .clear_color = e->config.clear_color
    };
    e->renderer = jce_renderer_create(e->window, &ren_cfg);
    if (!e->renderer) {
        LOG_WARN(LOG_TAG,
            "Renderer init failed, falling back");
        e->renderer =
            jce_renderer_create_fallback(e->window);
        if (!e->renderer) {
            fatal_msg("Fallback renderer init failed");
            goto fail;
        }
    }

    if (!jce_renderer_is_fallback(e->renderer)) {
        /* Load and attach shaders (graphics layer no
           longer depends on resource/pak_loader). */
        JceShaderSet shaders =
            jce_shaders_load_all(e->pak);
        jce_renderer_set_shaders(e->renderer, &shaders);
    }

    if (jce_renderer_is_fallback(e->renderer)) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_WARNING,
            "JCE - GPU Unsupported",
            "Hardware acceleration could not be "
            "initialized.\nThe application will now "
            "run in safe fallback mode.",
            jce_window_sdl(e->window));
        return e;
    }

    /* -- Remaining subsystems ------------------------------------- */

    render_loading_frame(e->renderer, e->window, "Loading...");
    jce_gpu_caps_init(&e->gpu_caps);

    e->input = jce_input_create();
    if (!e->input) {
        fatal_msg("Input system init failed");
        goto fail;
    }

    e->audio = jce_audio_create();
    if (!e->audio)
        LOG_WARN(LOG_TAG, "audio init failed, continuing without sound");

    if (e->audio && e->config.master_volume < 1.0f)
        jce_audio_set_master_volume(e->audio, e->config.master_volume);

    /* -- Build services struct ------------------------------------ */

    e->svc = (JceServices){
        .window   = e->window,
        .input    = e->input,
        .audio    = e->audio,
        .renderer = e->renderer,
        .pak      = e->pak,
        .config   = &e->config
    };

    /* -- Initialize application ----------------------------------- */

    if (g_app_desc_set && g_app_desc.init) {
        render_loading_frame(e->renderer, e->window, "Loading assets...");
        if (!g_app_desc.init(&e->svc, g_app_desc.user_data))
            goto fail;
    } else {
        fatal_msg("No JceAppDesc registered. Call jce_engine_set_app_desc() before jce_engine_create().");
        goto fail;
    }

    return e;

fail:
    jce_engine_destroy(e);
    return NULL;
}

/* -- Event routing ------------------------------------------------- */

SDL_AppResult jce_engine_event(JceEngine *e, const SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_TERMINATING)
        return SDL_APP_SUCCESS;

    if (e->input) jce_input_handle_event(e->input, event);

    if (event->type == SDL_EVENT_WINDOW_RESIZED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int pw, ph;
        SDL_GetWindowSizeInPixels(jce_window_sdl(e->window), &pw, &ph);
        jce_window_handle_resize(e->window, (uint32_t)pw, (uint32_t)ph);
        jce_renderer_resize(e->renderer, (uint32_t)pw, (uint32_t)ph);
    }

    if (g_app_desc.on_event)
        g_app_desc.on_event(event, g_app_desc.user_data);

    return SDL_APP_CONTINUE;
}

/* -- Per-frame ----------------------------------------------------- */

SDL_AppResult jce_engine_iterate(JceEngine *e)
{
    if (g_app_desc.should_quit) {
        if (g_app_desc.should_quit(g_app_desc.user_data))
            return SDL_APP_SUCCESS;
    }

    if (jce_renderer_is_fallback(e->renderer)) {
        jce_renderer_render_fallback_frame(e->renderer);
        if (e->input) jce_input_update(e->input);
        return SDL_APP_CONTINUE;
    }

    jce_renderer_begin_frame(e->renderer, e->window);

    if (g_app_desc.update) {
        g_app_desc.update(0.0f, g_app_desc.user_data);
        if (g_app_desc.draw)
            g_app_desc.draw(&e->svc, g_app_desc.user_data);
    }

    jce_renderer_end_frame(e->renderer);
    jce_input_update(e->input);

    return SDL_APP_CONTINUE;
}

/* -- Shutdown ------------------------------------------------------ */

void jce_engine_destroy(JceEngine *e)
{
    if (!e) return;

    if (g_app_desc.exit)
        g_app_desc.exit(g_app_desc.user_data);

    if (e->renderer) jce_renderer_destroy(e->renderer);
    if (e->audio)    jce_audio_destroy(e->audio);
    if (e->pak)      pak_close(e->pak);
    if (e->input)    jce_input_destroy(e->input);
    if (e->window)   jce_window_destroy(e->window);
    SDL_free(e);
}
