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
#include "core/jce_memory.h"
#include <jce/core/jce_profiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <jce/core/jce_log.h>
#include <jce/core/jce_crash_handler.h>
#include <jce/app/jce_config.h>
#include "graphics/jce_gpu_caps.h"
#include <jce/platform/jce_window.h>
#include "platform/jce_window_internal.h"
#include <jce/platform/jce_single_instance.h>
#include <jce/platform/jce_input.h>
#include <jce/audio/jce_audio.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/core/pak_loader.h>
#include <jce/resource/jce_asset.h>
#include <jce/app/jce_subsystem.h>
#include <jce/core/jce_allocator.h>
#include <jce/core/jce_event.h>
#include "embedded_assets.h"

#define LOG_TAG "engine"

static JceAppDesc  g_app_desc;
static bool        g_app_desc_set;
static char        g_config_path_override[512];
static int         g_renderer_backend_override = -1;  /* -1 = no override */

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

void jce_engine_set_renderer_override(int backend)
{
    g_renderer_backend_override = backend;
}

static bool jce_path_exists(const char *path)
{
    if (!path || !path[0]) return false;
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
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
    JceConfig        config;
    JceGpuCaps       gpu_caps;
    JceWindow       *window;
    JceInput        *input;
    JceAudio        *audio;
    JceRenderer     *renderer;
    PakArchive      *pak;
    JceAssetManager *assets;
    JceServices      svc;           /* subsystem handles for IApp */

    /* L1/L2 infrastructure (Phase 0) */
    jce_event_bus_t            *event_bus;
    jce_subsystem_registry_t   *subsystems;

    /* Optional KPI logging for Phase 0 baselines. */
    FILE                       *kpi_asset_log;
    uint64_t                    kpi_asset_frame_index;
    uint32_t                    kpi_asset_frame_limit;
};

static uint32_t read_positive_u32_env(const char *env_name, uint32_t fallback)
{
    const char *value = SDL_getenv(env_name);
    if (!value || !value[0]) {
        return fallback;
    }

    const int parsed = SDL_atoi(value);
    if (parsed <= 0) {
        return fallback;
    }

    return (uint32_t)parsed;
}

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

    JceEngine *e = JCE_CALLOC(1, sizeof(*e));
    if (!e) return NULL;

    e->config = jce_config_defaults();
    {
        char cfg_path[512];
        jce_select_config_path(cfg_path, sizeof(cfg_path));
        jce_config_load(&e->config, cfg_path);
    }

    /* Apply renderer backend override from editor (or other host). */
    if (g_renderer_backend_override >= 0)
        e->config.renderer_backend = (JceRendererBackend)g_renderer_backend_override;

    jce_log_set_level((JceLogLevel)e->config.log_level);
    jce_log_set_colors(e->config.log_colors);

    if (g_app_desc_set && g_app_desc.name && g_app_desc.name[0])
        SDL_strlcpy(e->config.window_title, g_app_desc.name,
                     sizeof(e->config.window_title));

    SDL_SetAppMetadata("JCE", "0.1.0", "com.jce");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fatal_msg("SDL_Init failed: %s", SDL_GetError());
        JCE_FREE(e);
        return NULL;
    }

    if (!jce_single_instance_lock(e->config.window_title)) {
        char msg[256];
        const char *title = (e->config.window_title[0] != '\0')
            ? e->config.window_title
            : "JCE";
        snprintf(msg, sizeof(msg), "%s is already running.", title);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, msg, NULL);
        SDL_Quit();
        JCE_FREE(e);
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
                void *pak_buf = JCE_MALLOC((size_t)pak_sz);
                if (pak_buf) {
                    if (SDL_ReadIO(io, pak_buf, (size_t)pak_sz) == (size_t)pak_sz) {
                        e->pak = pak_open_owned(pak_buf, (size_t)pak_sz);
                    }
                    if (!e->pak) JCE_FREE(pak_buf);
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
        JCE_FREE(e);
        return NULL;
    }

    /* Override window dimensions from app descriptor if set. */
    if (g_app_desc_set && g_app_desc.window_width)
        e->config.window_width  = (int)g_app_desc.window_width;
    if (g_app_desc_set && g_app_desc.window_height)
        e->config.window_height = (int)g_app_desc.window_height;

    /* -- Window ------------------------------------------------------- */

    JceWindowConfig win_cfg = {
        .title     = e->config.window_title,
        .logical_w = e->config.window_width,
        .logical_h = e->config.window_height,
        .flags     = (e->config.resizable  ? JCE_WINDOW_RESIZABLE  : 0)
                   | (e->config.fullscreen ? JCE_WINDOW_FULLSCREEN : 0)
                   | (e->config.maximized  ? JCE_WINDOW_MAXIMIZED  : 0)
                   | (g_app_desc_set && g_app_desc.maximized ? JCE_WINDOW_MAXIMIZED : 0)
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

    /* -- Asset manager -------------------------------------------- */

    {
        JceAssetManagerConfig acfg = {0};
        acfg.pak   = e->pak;
        acfg.audio = e->audio;
        e->assets = jce_asset_manager_create(&acfg);
        if (!e->assets)
            LOG_WARN(LOG_TAG, "asset manager init failed — direct loading only");
    }

    {
        const char *asset_kpi_path = SDL_getenv("JCE_KPI_ASSET_LOG");
        if (asset_kpi_path && asset_kpi_path[0]) {
            e->kpi_asset_log = fopen(asset_kpi_path, "w");
            if (e->kpi_asset_log) {
                e->kpi_asset_frame_limit = read_positive_u32_env("JCE_KPI_ASSET_FRAME_COUNT", 0u);
                fprintf(e->kpi_asset_log, "frame_index,asset_update_ms\n");
                fflush(e->kpi_asset_log);
                LOG_INFO(LOG_TAG, "asset KPI capture enabled -> %s", asset_kpi_path);
            } else {
                LOG_WARN(LOG_TAG, "failed to open asset KPI log: %s", asset_kpi_path);
            }
        }
    }

    /* -- Build services struct ------------------------------------ */

    e->svc = (JceServices){
        .window   = e->window,
        .input    = e->input,
        .audio    = e->audio,
        .renderer = e->renderer,
        .pak      = e->pak,
        .config   = &e->config,
        .assets   = e->assets
    };

    /* -- L1/L2 infrastructure ------------------------------------- */

    {
        jce_allocator_t def_alloc = jce_allocator_default();
        e->event_bus = jce_event_bus_create(def_alloc);
        if (!e->event_bus)
            LOG_WARN(LOG_TAG, "event bus creation failed");

        e->subsystems = jce_subsystem_registry_create(def_alloc);
        if (!e->subsystems)
            LOG_WARN(LOG_TAG, "subsystem registry creation failed");
    }

    /* Initialize registered subsystems before app init. */
    if (e->subsystems)
        jce_subsystem_init_all(e->subsystems, &e->svc);

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

JceAppResult jce_engine_event(JceEngine *e, const void *platform_event)
{
    const SDL_Event *event = (const SDL_Event *)platform_event;
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_TERMINATING)
        return JCE_APP_SUCCESS;

    if (e->input) jce_input_handle_event(e->input, event);

    if (event->type == SDL_EVENT_WINDOW_RESIZED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int pw, ph;
        SDL_GetWindowSizeInPixels(jce_window_sdl(e->window), &pw, &ph);
        jce_window_handle_resize(e->window, (uint32_t)pw, (uint32_t)ph);
        jce_renderer_resize(e->renderer, (uint32_t)pw, (uint32_t)ph);
        if (g_app_desc.on_resize)
            g_app_desc.on_resize((uint32_t)pw, (uint32_t)ph,
                                 g_app_desc.user_data);

        /* On Windows, SDL3 runs a modal loop during window resize (WM_SIZING)
         * so SDL_AppIterate is never called.  Emit a minimal render frame here
         * so bgfx processes the reset and the backbuffer stays in sync. */
        if (!jce_renderer_is_fallback(e->renderer)) {
            jce_renderer_begin_frame(e->renderer, e->window);

            if (g_app_desc.update)
                g_app_desc.update(0.0f, g_app_desc.user_data);
            if (g_app_desc.draw)
                g_app_desc.draw(&e->svc, g_app_desc.user_data);

            jce_renderer_end_frame(e->renderer);
        }
    }

    if (g_app_desc.on_event)
        g_app_desc.on_event(event, g_app_desc.user_data);

    return JCE_APP_CONTINUE;
}

/* -- Per-frame ----------------------------------------------------- */

JceAppResult jce_engine_iterate(JceEngine *e)
{
    JCE_PROFILE_ZONE_N("Frame");

    if (g_app_desc.should_quit) {
        if (g_app_desc.should_quit(g_app_desc.user_data))
            return JCE_APP_SUCCESS;
    }

    if (jce_renderer_is_fallback(e->renderer)) {
        jce_renderer_render_fallback_frame(e->renderer);
        if (e->input) jce_input_update(e->input);
        return JCE_APP_CONTINUE;
    }

    jce_renderer_begin_frame(e->renderer, e->window);

    /* Finalize async asset loads (GPU resource creation). */
    if (e->assets) {
        uint64_t asset_t0 = 0;
        if (e->kpi_asset_log &&
            (e->kpi_asset_frame_limit == 0u ||
             e->kpi_asset_frame_index < e->kpi_asset_frame_limit)) {
            asset_t0 = SDL_GetPerformanceCounter();
        }

        JCE_PROFILE_ZONE_N("AssetManager::Update");
        jce_asset_manager_update(e->assets, 3.0f);
        JCE_PROFILE_ZONE_END;

        if (asset_t0 != 0) {
            const uint64_t asset_t1 = SDL_GetPerformanceCounter();
            const uint64_t freq = SDL_GetPerformanceFrequency();
            if (freq > 0) {
                const double asset_ms =
                    (double)(asset_t1 - asset_t0) * 1000.0 / (double)freq;
                fprintf(e->kpi_asset_log, "%llu,%.3f\n",
                        (unsigned long long)e->kpi_asset_frame_index,
                        asset_ms);
                e->kpi_asset_frame_index++;
                if ((e->kpi_asset_frame_index % 60u) == 0u) {
                    fflush(e->kpi_asset_log);
                }
            }
        }
    }

    /* Tick registered subsystems. */
    if (e->subsystems) {
        JCE_PROFILE_ZONE_N("Subsystems::Update");
        jce_subsystem_update_all(e->subsystems, 0.0f);
        JCE_PROFILE_ZONE_END;
    }

    if (g_app_desc.update) {
        JCE_PROFILE_ZONE_N("App::UpdateAndDraw");
        g_app_desc.update(0.0f, g_app_desc.user_data);
        if (g_app_desc.draw)
            g_app_desc.draw(&e->svc, g_app_desc.user_data);
        JCE_PROFILE_ZONE_END;
    }

    jce_renderer_end_frame(e->renderer);

    {
        JCE_PROFILE_ZONE_N("Input::Update");
        jce_input_update(e->input);
        JCE_PROFILE_ZONE_END;
    }

    JCE_PROFILE_FRAME_MARK;
    JCE_PROFILE_ZONE_END;
    return JCE_APP_CONTINUE;
}

/* -- Shutdown ------------------------------------------------------ */

void jce_engine_destroy(JceEngine *e)
{
    if (!e) return;

    if (g_app_desc.exit)
        g_app_desc.exit(g_app_desc.user_data);

    /* Shut down registered subsystems (reverse priority). */
    if (e->subsystems) {
        jce_subsystem_shutdown_all(e->subsystems);
        jce_subsystem_registry_destroy(e->subsystems);
    }

    if (e->event_bus) jce_event_bus_destroy(e->event_bus);

    if (e->kpi_asset_log) {
        fflush(e->kpi_asset_log);
        fclose(e->kpi_asset_log);
        e->kpi_asset_log = NULL;
    }

    if (e->assets)   jce_asset_manager_destroy(e->assets);
    if (e->renderer) jce_renderer_destroy(e->renderer);
    if (e->audio)    jce_audio_destroy(e->audio);
    if (e->pak)      pak_close(e->pak);
    if (e->input)    jce_input_destroy(e->input);
    if (e->window)   jce_window_destroy(e->window);
    jce_single_instance_unlock();
    JCE_FREE(e);

    /* Flush and shut down the async log backend (last, so all
       teardown messages are captured). */
    jce_log_shutdown();
}
