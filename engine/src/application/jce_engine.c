/*
 * jce_engine.c  Engine bootstrap and lifecycle implementation.
 *
 * Centralises all subsystem creation, event routing, and
 * per-frame orchestration.  Asset loading is delegated to
 * the application via JceAppDesc callbacks.
 */

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef SDL_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <jce/application/jce_config.h>
#include <jce/application/jce_subsystem.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_crash_handler.h>
#include <jce/os/core/jce_event.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/pak_loader.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_single_instance.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_text.h>
#include <jce/resource/jce_asset.h>

#include "embedded_assets.h"
#include "os/platform/jce_window_internal.h"
#include "renderer/jce_gpu_caps.h"

#include <bgfx/c99/bgfx.h>

#define LOG_TAG "engine"
#define JCE_DEFAULT_FRAME_DT (1.0f / 60.0f)
#define JCE_MAX_FRAME_DT 0.1f

static JceAppDesc  g_app_desc;
static bool        g_app_desc_set;
static char        g_config_path_override[512];
static char        g_pak_path_override[512];
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

void jce_engine_set_pak_path(const char *path)
{
    if (!path || !path[0]) {
        g_pak_path_override[0] = '\0';
        return;
    }

    snprintf(g_pak_path_override, sizeof(g_pak_path_override), "%s", path);
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
    JcePakArchive      *pak;
    JceAssetManager *assets;
    JceServices      svc;           /* subsystem handles for IApp */

    /* L1/L2 infrastructure (Phase 0) */
    jce_event_bus_t            *event_bus;
    jce_subsystem_registry_t   *subsystems;

    /* Optional KPI logging for Phase 0 baselines. */
    SDL_IOStream                *kpi_asset_log;
    uint64_t                    kpi_asset_frame_index;
    uint32_t                    kpi_asset_frame_limit;

    /* Frame clock state for dt propagation. */
    uint64_t                    perf_freq;
    uint64_t                    frame_counter_prev;
};

static void jce_engine_reset_frame_clock(JceEngine *e)
{
    if (!e) return;
    e->perf_freq = jce_time_perf_freq();
    e->frame_counter_prev = jce_time_perf_counter();
}

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

static bool should_render_loading_frame(void)
{
    return bgfx_get_renderer_type() != BGFX_RENDERER_TYPE_OPENGL;
}

/* -- Create -------------------------------------------------------- */

static bool jce_resize_event_watch(void *userdata, SDL_Event *event);
#ifdef SDL_PLATFORM_WINDOWS
static bool SDLCALL jce_win32_msg_hook(void *userdata, MSG *msg);
#endif

/* Shared reentrancy / pause atomics for the resize watcher and the
 * Win32 modal-loop timer.  Forward-declared here because the modal
 * timer (defined in the SDL_PLATFORM_WINDOWS block below) references
 * them before the watcher's definition that owns them. */
static SDL_AtomicInt s_in_render_frame = {0};
static SDL_AtomicInt s_render_paused = {0};

/* NOTE: The previous s_in_modal_loop / s_pending_modal_resize / jce_render_
 * blocked_by_modal_loop machinery that deferred bgfx_reset and rendering for
 * the OpenGL backend during Win32 modal sizing has been removed.
 *
 * History: that code was a workaround for an old concern that wglSwapBuffers
 * called from inside the WM_ENTERSIZEMOVE modal loop would produce visible
 * black flashes because it bypassed DWM compositing.  On modern Windows 10+
 * with DWM always enabled this does NOT happen: DWM intercepts wglSwapBuffers
 * and composites correctly regardless of the modal loop state.  The deferral
 * caused a worse UX regression: content didn't follow the window size during
 * drag and visibly snapped to the new dimensions on mouse release. */

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
    e->pak = jce_pak_open_file("/game_assets.pak");
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
                        e->pak = jce_pak_open_owned(pak_buf, (size_t)pak_sz);
                    }
                    if (!e->pak) JCE_FREE(pak_buf);
                }
            }
            SDL_CloseIO(io);
        }
    }
#elif defined(JCE_BUILD_JNI)
    /* JNI desktop: PAK shipped as a separate file alongside the native lib.
     * Java side sets pak path via jce_engine_set_pak_path() before create,
     * or we fall back to searching next to the shared library / CWD. */
    {
        const char *base = SDL_GetBasePath();
        char pak_path[512];
        bool found = false;

        /* Try explicit PAK path (set by JceRuntime.java). */
        if (g_pak_path_override[0]) {
            snprintf(pak_path, sizeof(pak_path), "%s", g_pak_path_override);
            found = jce_path_exists(pak_path);
        }

        /* Try next to the shared library. */
        if (!found && base) {
            snprintf(pak_path, sizeof(pak_path), "%sgame_assets.pak", base);
            found = jce_path_exists(pak_path);
        }

        /* Try CWD. */
        if (!found) {
            snprintf(pak_path, sizeof(pak_path), "game_assets.pak");
            found = jce_path_exists(pak_path);
        }

        if (found) {
            LOG_INFO(LOG_TAG, "JNI: loading PAK from %s", pak_path);
            e->pak = jce_pak_open_file(pak_path);
        }
    }
#else
    e->pak = jce_pak_open(assets_pak_data, assets_pak_data_size);
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
        jce_engine_reset_frame_clock(e);
        return e;
    }

    /* -- Remaining subsystems ------------------------------------- */

    if (should_render_loading_frame()) {
        render_loading_frame(e->renderer, e->window, "Loading...");
    }
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
            e->kpi_asset_log = SDL_IOFromFile(asset_kpi_path, "w");
            if (e->kpi_asset_log) {
                e->kpi_asset_frame_limit = read_positive_u32_env("JCE_KPI_ASSET_FRAME_COUNT", 0u);
                static const char hdr[] = "frame_index,asset_update_ms\n";
                SDL_WriteIO(e->kpi_asset_log, hdr, sizeof(hdr) - 1);
                SDL_FlushIO(e->kpi_asset_log);
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
        if (should_render_loading_frame())
            render_loading_frame(e->renderer, e->window, "Loading assets...");
        if (!g_app_desc.init(&e->svc, g_app_desc.user_data))
            goto fail;
    } else {
        fatal_msg("No JceAppDesc registered. Call jce_engine_set_app_desc() before jce_engine_create().");
        goto fail;
    }

    /* Register live-resize watcher so resize events are handled even
       during Windows modal message loops (needed for JNI bridge). */
    SDL_AddEventWatch(jce_resize_event_watch, e);

#ifdef SDL_PLATFORM_WINDOWS
    /* Keep rendering while the user holds the title bar / window border
       even without moving the mouse (modal loop with no resize events). */
    SDL_SetWindowsMessageHook(jce_win32_msg_hook, e);
#endif

    jce_engine_reset_frame_clock(e);

    return e;

fail:
    jce_engine_destroy(e);
    return NULL;
}

/* -- Windows modal-loop rendering --------------------------------- */

/*
 * On Windows, clicking and holding the title bar or window border enters
 * a modal message loop inside DefWindowProc (WM_ENTERSIZEMOVE).  During
 * this loop, SDL_PollEvent never returns, so the JNI bridge's iterate
 * loop is completely stalled — even if the user doesn't move the mouse.
 *
 * Solution: use SDL_SetWindowsMessageHook to detect WM_ENTERSIZEMOVE /
 * WM_EXITSIZEMOVE, and pump frames via a Win32 timer (~60 fps) that
 * fires inside the modal loop.  The event watch below handles resize
 * events specifically (bgfx reset + viewport update).
 */

#ifdef SDL_PLATFORM_WINDOWS

#define JCE_MODAL_TIMER_ID   1
#define JCE_MODAL_TIMER_MS  16   /* ~60 fps */

static JceEngine *g_modal_engine;

static void CALLBACK jce_modal_timer_proc(HWND hwnd, UINT msg,
                                          UINT_PTR id, DWORD time)
{
    (void)hwnd; (void)msg; (void)id; (void)time;
    JceEngine *e = g_modal_engine;
    if (!e || !e->renderer) return;

    /* Cooperate with jce_resize_event_watch: only one of the two paths
     * may be inside begin_frame/end_frame at a time.  Skipping a timer
     * tick is safe — the next 16 ms tick will pick up. */
    if (SDL_GetAtomicInt(&s_in_render_frame) != 0)
        return;

    jce_engine_iterate(e);
}

static bool SDLCALL jce_win32_msg_hook(void *userdata, MSG *msg)
{
    JceEngine *e = (JceEngine *)userdata;

    if (msg->message == WM_ENTERSIZEMOVE) {
        g_modal_engine = e;
        SetTimer(msg->hwnd, JCE_MODAL_TIMER_ID,
                 JCE_MODAL_TIMER_MS, jce_modal_timer_proc);
    } else if (msg->message == WM_EXITSIZEMOVE) {
        KillTimer(msg->hwnd, JCE_MODAL_TIMER_ID);
        g_modal_engine = NULL;
    }

    return true;   /* let SDL process the message */
}

#endif /* SDL_PLATFORM_WINDOWS */

/* -- Live-resize event watcher ------------------------------------ */

/*
 * SDL_AddEventWatch callbacks fire from within the OS message pump,
 * including during modal operations.  This handles resize-specific
 * work: update window state, reset bgfx backbuffer, render one frame.
 */
static bool jce_resize_event_watch(void *userdata, SDL_Event *event)
{
    JceEngine *e = (JceEngine *)userdata;

    const Uint32 t = event->type;

    /* Pause/resume on minimize so we don't keep resetting to a 0-sized
     * backbuffer (which leaves bgfx in a broken state on restore). */
    if (t == SDL_EVENT_WINDOW_MINIMIZED) {
        SDL_SetAtomicInt(&s_render_paused, 1);
        return true;
    }
    if (t == SDL_EVENT_WINDOW_RESTORED ||
        t == SDL_EVENT_WINDOW_SHOWN) {
        SDL_SetAtomicInt(&s_render_paused, 0);
        /* Fall through to the size-refresh path below so we re-reset
         * bgfx with the post-restore drawable size. */
    }

    const bool is_size_event =
        (t == SDL_EVENT_WINDOW_RESIZED ||
         t == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
         t == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
         t == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN ||
         t == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
         t == SDL_EVENT_WINDOW_RESTORED ||
         t == SDL_EVENT_WINDOW_SHOWN);

    /* SDL_EVENT_WINDOW_EXPOSED needs a redraw but no bgfx_reset — handle
     * it as a "draw one frame if we can" pulse so the window doesn't
     * stay blank when uncovered during a modal drag. */
    const bool is_expose_event = (t == SDL_EVENT_WINDOW_EXPOSED);

    if (!is_size_event && !is_expose_event)
        return true;   /* pass event through, not ours */

    /* Reentrancy guard: SDL can pump events from inside our own render
     * (e.g. via SDL_SyncWindow or modal Win32 sizing).  The same guard
     * is shared with the Win32 modal timer below to keep them mutually
     * exclusive. */
    if (SDL_GetAtomicInt(&s_in_render_frame) != 0)
        return true;
    if (SDL_GetAtomicInt(&s_render_paused) != 0)
        return true;
    SDL_SetAtomicInt(&s_in_render_frame, 1);

    int pw, ph;
    SDL_GetWindowSizeInPixels(jce_window_sdl(e->window), &pw, &ph);
    if (pw <= 0 || ph <= 0) {
        SDL_SetAtomicInt(&s_in_render_frame, 0);
        return true;
    }

    if (is_size_event) {
        jce_window_handle_resize(e->window, (uint32_t)pw, (uint32_t)ph);
        jce_renderer_resize(e->renderer, (uint32_t)pw, (uint32_t)ph);

        if (g_app_desc.on_resize)
            g_app_desc.on_resize((uint32_t)pw, (uint32_t)ph,
                                 g_app_desc.user_data);
    }

    SDL_SetAtomicInt(&s_in_render_frame, 0);

    if (!jce_renderer_is_fallback(e->renderer))
        jce_engine_iterate(e);

    return true;   /* let other watchers see the event too */
}

/* -- Event routing ------------------------------------------------- */

JceAppResult jce_engine_event(JceEngine *e, const void *platform_event)
{
    const SDL_Event *event = (const SDL_Event *)platform_event;

    if (event->type == SDL_EVENT_TERMINATING)
        return JCE_APP_SUCCESS;

    if (event->type == SDL_EVENT_QUIT) {
        if (g_app_desc.on_event)
            g_app_desc.on_event(event, g_app_desc.user_data);

        /* If the application registered a should_quit callback, give it
           a chance to intercept the quit (e.g. to show an unsaved-changes
           dialog).  If the callback returns false, swallow the event. */
        if (g_app_desc.should_quit) {
            if (g_app_desc.should_quit(g_app_desc.user_data))
                return JCE_APP_SUCCESS;
            return JCE_APP_CONTINUE;
        }
        return JCE_APP_SUCCESS;
    }

    if (e->input) jce_input_handle_event(e->input, event);

    /* Resize handling is done in jce_resize_event_watch() which fires
       from both SDL_PollEvent and Windows modal message loops. */

    if (g_app_desc.on_event)
        g_app_desc.on_event(event, g_app_desc.user_data);

    return JCE_APP_CONTINUE;
}

/* -- Per-frame ----------------------------------------------------- */

JceAppResult jce_engine_iterate(JceEngine *e)
{
    JCE_PROFILE_ZONE_N("Frame");

    float dt = JCE_DEFAULT_FRAME_DT;
    const uint64_t now = jce_time_perf_counter();

    if (e->perf_freq == 0)
        e->perf_freq = jce_time_perf_freq();

    if (e->perf_freq > 0 &&
        e->frame_counter_prev > 0 &&
        now >= e->frame_counter_prev) {
        const double elapsed = (double)(now - e->frame_counter_prev);
        dt = (float)(elapsed / (double)e->perf_freq);
    }

    e->frame_counter_prev = now;

    if (dt < 0.0f) dt = 0.0f;
    if (dt > JCE_MAX_FRAME_DT) dt = JCE_MAX_FRAME_DT;

    if (g_app_desc.should_quit) {
        if (g_app_desc.should_quit(g_app_desc.user_data))
            return JCE_APP_SUCCESS;
    }

    if (jce_renderer_is_fallback(e->renderer)) {
        jce_renderer_render_fallback_frame(e->renderer);
        if (e->input) jce_input_update(e->input);
        return JCE_APP_CONTINUE;
    }

    /* Window is minimized (or its drawable size collapsed to zero).
     * Skip the entire render pipeline: bgfx_reset(0,0) would corrupt
     * the swap chain, and begin_frame on a zero-sized backbuffer is
     * undefined.  Still update input so we notice when the user
     * restores the window. */
    if (SDL_GetAtomicInt(&s_render_paused) != 0) {
        if (e->input) jce_input_update(e->input);
        /* Throttle main loop while minimized to avoid burning CPU and
         * to give worker threads (video decode, async assets) a chance
         * to drain. Without this the main loop spins at thousands of
         * fps while the renderer is paused, which keeps producing input
         * polls / log entries / event queue churn. */
        jce_thread_sleep_ms(33);
        return JCE_APP_CONTINUE;
    }

    /* Reconcile any size change the resize watcher may have missed.
     *
     * SDL_AddEventWatch callbacks fire exactly once, when the event is
     * pushed onto the queue.  If a size change is pushed while we are
     * already inside this function (s_in_render_frame == 1) — which
     * happens, for example, when the app calls SDL_SetWindowFullscreen
     * from inside its own update callback (CK's F11 handler) and SDL3
     * pumps the resulting WM_SIZE synchronously — the watcher
     * early-returns and the resize is then dropped, because by the time
     * the next iterate runs, no further watcher invocation occurs for
     * the queued event.  Result: bgfx keeps the old backbuffer size
     * while the OS-presented window is at the new size, producing the
     * "old image in a corner of an otherwise black window" symptom that
     * F11→fullscreen exhibits.
     *
     * Catch this here by comparing SDL's authoritative pixel size
     * against our tracked JceWindow size, and applying the resize once
     * before begin_frame.  This is cheap (an SDL accessor + an integer
     * compare) and a no-op in the common case. */
    if (e->window) {
        int sdl_pw = 0, sdl_ph = 0;
        SDL_GetWindowSizeInPixels(jce_window_sdl(e->window),
                                  &sdl_pw, &sdl_ph);
        if (sdl_pw > 0 && sdl_ph > 0) {
            uint32_t cur_w = 0, cur_h = 0;
            jce_window_get_size(e->window, &cur_w, &cur_h);
            if ((uint32_t)sdl_pw != cur_w || (uint32_t)sdl_ph != cur_h) {
                jce_window_handle_resize(e->window,
                                         (uint32_t)sdl_pw,
                                         (uint32_t)sdl_ph);
                if (e->renderer)
                    jce_renderer_resize(e->renderer,
                                        (uint32_t)sdl_pw,
                                        (uint32_t)sdl_ph);
                if (g_app_desc.on_resize)
                    g_app_desc.on_resize((uint32_t)sdl_pw,
                                         (uint32_t)sdl_ph,
                                         g_app_desc.user_data);
            }
        }
    }

    /* Cooperate with the resize watcher / Win32 modal timer: never enter
     * begin_frame while one of them is mid-frame, otherwise bgfx sees
     * nested frames and produces a black flash. */
    if (SDL_GetAtomicInt(&s_in_render_frame) != 0) {
        if (e->input) jce_input_update(e->input);
        return JCE_APP_CONTINUE;
    }
    SDL_SetAtomicInt(&s_in_render_frame, 1);

    jce_renderer_begin_frame(e->renderer, e->window);

    /* Finalize async asset loads (GPU resource creation). */
    if (e->assets) {
        uint64_t asset_t0 = 0;
        if (e->kpi_asset_log &&
            (e->kpi_asset_frame_limit == 0u ||
             e->kpi_asset_frame_index < e->kpi_asset_frame_limit)) {
            asset_t0 = jce_time_perf_counter();
        }

        JCE_PROFILE_ZONE_N("AssetManager::Update");
        jce_asset_manager_update(e->assets, 3.0f);
        JCE_PROFILE_ZONE_END;

        if (asset_t0 != 0) {
            const uint64_t asset_t1 = jce_time_perf_counter();
            const uint64_t freq = jce_time_perf_freq();
            if (freq > 0) {
                const double asset_ms =
                    (double)(asset_t1 - asset_t0) * 1000.0 / (double)freq;
                char kpi_line[64];
                int kpi_len = snprintf(kpi_line, sizeof(kpi_line),
                    "%llu,%.3f\n",
                    (unsigned long long)e->kpi_asset_frame_index,
                    asset_ms);
                if (kpi_len > 0)
                    SDL_WriteIO(e->kpi_asset_log, kpi_line, (size_t)kpi_len);
                e->kpi_asset_frame_index++;
                if ((e->kpi_asset_frame_index % 60u) == 0u) {
                    SDL_FlushIO(e->kpi_asset_log);
                }
            }
        }
    }

    /* Tick registered subsystems. */
    if (e->subsystems) {
        JCE_PROFILE_ZONE_N("Subsystems::Update");
        jce_subsystem_update_all(e->subsystems, dt);
        JCE_PROFILE_ZONE_END;
    }

    if (g_app_desc.update) {
        JCE_PROFILE_ZONE_N("App::UpdateAndDraw");
        g_app_desc.update(dt, g_app_desc.user_data);
        if (g_app_desc.draw)
            g_app_desc.draw(&e->svc, g_app_desc.user_data);
        JCE_PROFILE_ZONE_END;
    }

    jce_renderer_end_frame(e->renderer);

    SDL_SetAtomicInt(&s_in_render_frame, 0);

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

    SDL_RemoveEventWatch(jce_resize_event_watch, e);

#ifdef SDL_PLATFORM_WINDOWS
    SDL_SetWindowsMessageHook(NULL, NULL);
    g_modal_engine = NULL;
#endif

    if (g_app_desc.exit)
        g_app_desc.exit(g_app_desc.user_data);

    /* Shut down registered subsystems (reverse priority). */
    if (e->subsystems) {
        jce_subsystem_shutdown_all(e->subsystems);
        jce_subsystem_registry_destroy(e->subsystems);
    }

    if (e->event_bus) jce_event_bus_destroy(e->event_bus);

    if (e->kpi_asset_log) {
        SDL_FlushIO(e->kpi_asset_log);
        SDL_CloseIO(e->kpi_asset_log);
        e->kpi_asset_log = NULL;
    }

    if (e->assets)   jce_asset_manager_destroy(e->assets);
    if (e->renderer) jce_renderer_destroy(e->renderer);
    jce_text_shutdown();
    if (e->audio)    jce_audio_destroy(e->audio);
    if (e->pak)      jce_pak_close(e->pak);
    if (e->input)    jce_input_destroy(e->input);
    if (e->window)   jce_window_destroy(e->window);
    jce_single_instance_unlock();
    JCE_FREE(e);

    /* Flush and shut down the async log backend (last, so all
       teardown messages are captured). */
    jce_log_shutdown();
}
