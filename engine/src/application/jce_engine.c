/*
 * jce_engine.c  Engine bootstrap and lifecycle implementation.
 *
 * Centralises all subsystem creation, event routing, and
 * per-frame orchestration.  Asset loading is delegated to
 * the application via JceAppDesc callbacks.
 */

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_args.h>
#include <jce/application/jce_engine.h>
#include <jce/application/jce_lifecycle.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_jobs.h>
#include <jce/os/core/jce_timer.h>
#include <jce/runtime/jce_player_loop.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>   /* qsort — perf percentile (1% / 0.1% low) summary */
#include <string.h>

/* Platform branching below goes through the JCE_PLATFORM_* constants from
 * jce_defs.h (charter: raw platform macros stay inside the OS layer). The
 * emscripten header is the one exception that has no abstraction — it is
 * only pulled in when the WEB platform constant says so. */
#if JCE_PLATFORM_WEB
#include <emscripten.h>
#endif

#include <jce/os/core/jce_config.h>
#include <jce/application/jce_subsystem.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/resource/jce_streaming.h>
#include <jce/middleware/ui/jce_localization.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_crash_handler.h>
#include <jce/os/core/jce_event.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/platform/jce_host_paths.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_record.h>
#if defined(JCE_ENABLE_AI_DISPATCH) && JCE_ENABLE_AI_DISPATCH
#include <jce/middleware/ai_dispatch/jce_ai_dispatch.h>
#endif
#include <jce/os/platform/jce_single_instance.h>
#include <jce/os/platform/jce_window.h>
#include <jce/os/platform/jce_window_modal_loop.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_lowlevel.h>   /* jce_gfx_stats_capture — per-view GPU timing */
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/resource/jce_asset.h>

#include "jce_embedded_assets.h"
#include "os/platform/jce_window_internal.h"
#include "renderer/jce_gpu_caps.h"

#include <bgfx/c99/bgfx.h>

#define LOG_TAG "engine"
#define JCE_DEFAULT_FRAME_DT (1.0f / 60.0f)
#define JCE_MAX_FRAME_DT 0.1f

static void jce_engine_secure_zero(void *data, size_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (size-- > 0)
        *p++ = 0;
}

/* ── Frame-time percentiles (standard-engine benchmark metric) ───────────
 * UE (stat unit / CsvProfiler) and Unity (Performance Testing) report the
 * 1%-low and 0.1%-low frame times — the p99 / p99.9 of the frame-time
 * distribution — because the average hides the hitches players actually feel.
 * We keep a fixed ring of the last N frame times (no alloc) and, in the
 * JCE_PERF_LOG window report, sort a copy to emit those percentiles. */
#define JCE_PERF_RING 2048   /* enough samples for a meaningful 0.1%-low */

static int jce_perf_cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
/* q in [0,1]; nearest-rank on an ascending-sorted array. */
static double jce_perf_pct(const double *asc, uint32_t n, double q) {
    if (n == 0) return 0.0;
    long r = (long)(q * (double)n + 0.5) - 1;   /* nearest-rank, 0-based */
    if (r < 0) r = 0;
    if (r >= (long)n) r = (long)n - 1;
    return asc[r];
}

static JceAppDesc  g_app_desc;
static bool        g_app_desc_set;
static bool        g_quit_requested;   /* jce_engine_request_quit() latch */
static char        g_config_path_override[512];
static char        g_pak_path_override[512];
static char        g_bundle_catalog_path[512];

void JCE_CALL jce_engine_request_quit(void)  { g_quit_requested = true; }
bool JCE_CALL jce_engine_quit_requested(void) { return g_quit_requested; }
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

void jce_engine_set_bundle_catalog_path(const char *path)
{
    if (!path || !path[0]) {
        g_bundle_catalog_path[0] = '\0';
        return;
    }
    snprintf(g_bundle_catalog_path, sizeof(g_bundle_catalog_path), "%s", path);
}

/* Settings S7 follow-up: layer the jce.ini [graphics] section (written by
 * the in-game settings screen) onto the live render pipeline.  Mirrors the
 * screen's own Apply precedence: a named quality preset fills the whole
 * descriptor, then the individual toggles override on top.  "custom" keeps
 * whatever the boot resolution (.rp.json or tier preset) produced and only
 * layers the toggles.  No-op when no [graphics] section was loaded. */
void JCE_CALL jce_engine_apply_graphics_config(const struct JceConfig *cfg,
                                               struct JceRenderer *renderer)
{
    if (!cfg || !cfg->gfx_valid) return;

    JceRenderPipelineDesc desc;
    jce_render_pipeline_get(&desc);

    switch (cfg->gfx_quality) {
    case -1: jce_render_pipeline_preset_for_current_tier(&desc); break;
    case 0:  jce_render_pipeline_preset_low(&desc);              break;
    case 1:  jce_render_pipeline_preset_mid(&desc);              break;
    case 2:  jce_render_pipeline_preset_high(&desc);             break;
    case 3:  jce_render_pipeline_preset_ultra(&desc);            break;
    default: break;   /* 4 = custom: keep the boot-resolved descriptor */
    }

    desc.enable_csm            = cfg->gfx_shadows;
    desc.enable_ssao           = cfg->gfx_ssao;
    desc.enable_bloom          = cfg->gfx_bloom;
    desc.enable_volumetric_fog = cfg->gfx_fog;
    {
        int sq = cfg->gfx_shadow_quality;
        if (sq < 0) sq = 0;
        if (sq > 2) sq = 2;
        desc.shadow_filter_quality = (uint8_t)sq;
    }
    {
        int ms = cfg->gfx_msaa;
        if (ms != 1 && ms != 2 && ms != 4 && ms != 8) ms = 1;
        desc.msaa_samples = (uint8_t)ms;
    }

    jce_render_pipeline_apply(&desc);
    if (renderer)
        jce_renderer_set_msaa(renderer, (int)desc.msaa_samples);
    LOG_INFO(LOG_TAG, "applied [graphics] overrides from user config "
             "(quality=%d)", cfg->gfx_quality);
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

/* QW-input-actions — locate the action-map authored by the editor.
 * The editor's Input Manager panel writes `~/.jce/input_actions.json`
 * (jce_editor_dotjce_path).  A shipped game may instead drop a
 * project-relative `.jce/input_actions.json` next to its working dir.
 * Prefer the CWD-relative copy (game ships its own), then the per-user
 * one (editor authoring), so play-in-editor and standalone both work. */
static void jce_select_input_actions_path(char *out_path, size_t out_size)
{
    out_path[0] = '\0';

    const char *cwd_actions = ".jce/input_actions.json";
    if (jce_path_exists(cwd_actions)) {
        snprintf(out_path, out_size, "%s", cwd_actions);
        return;
    }

    char home[512];
    if (jce_host_get_user_folder(JCE_USER_FOLDER_HOME, home, sizeof(home))) {
        size_t hl = strlen(home);
        while (hl > 0 && (home[hl - 1] == '/' || home[hl - 1] == '\\'))
            home[--hl] = '\0';
        snprintf(out_path, out_size, "%s/.jce/input_actions.json", home);
    }
}

/* input_actions.json loading lives in the input-actions module itself
 * (jce_actions_load_file) so the editor's Play mode and other hosts can
 * reuse it instead of duplicating the parser. */

/* P3-B.3 — engine-internal lifecycle listener.  Bridges OS LOW_MEMORY
 * signals to the streaming pressure system so registered mip-streaming
 * / cache hooks fire even before our own budget tripped.  Other events
 * are logged at info level when JCE_DEBUG is on (lifecycle visibility
 * is cheap and very useful during platform bring-up). */
static void JCE_CALL engine_lifecycle_listener(JceLifecycleEvent event, void *user)
{
    (void)user;
    LOG_INFO(LOG_TAG, "lifecycle: %s", jce_lifecycle_event_to_string(event));
    if (event == JCE_LIFECYCLE_LOW_MEMORY)
        jce_streaming_signal_low_memory_all();
}

/* -- Engine state -------------------------------------------------- */

struct JceEngine {
    JceConfig        config;
    JceGpuCaps       gpu_caps;
    JceWindow       *window;
    JceInput        *input;
    JceInputActions *actions;       /* action-map layer (QW-input-actions) */
    JceAudio        *audio;
    JceRenderer     *renderer;
    JcePakArchive      *pak;
    JceAssetManager *assets;
    JceServices      svc;           /* subsystem handles for IApp */

    /* Optional scene-asset bundle catalog (NULL when unused). */
    void               *bundle_catalog; /* JceBundleCatalog* (opaque) */
    JceFileSystem      *bundle_fs;      /* multi-pak FS for mounted bundles */

    /* L1/L2 infrastructure (Phase 0) */
    jce_event_bus_t            *event_bus;
    jce_subsystem_registry_t   *subsystems;

    /* Optional KPI logging for Phase 0 baselines. */
    SDL_IOStream                *kpi_asset_log;
    uint64_t                    kpi_asset_frame_index;
    uint32_t                    kpi_asset_frame_limit;

    /* Headless / CI auto-quit: when JCE_MAX_FRAMES is set, the engine emits
     * WILL_QUIT and exits cleanly after that many rendered frames.  0 (the
     * default) disables it.  Used by autonomous validation harnesses to run a
     * deterministic number of frames (e.g. long enough for world streaming to
     * apply chunks) without depending on window focus or an interactive quit. */
    uint32_t                    max_frames;
    uint32_t                    frame_index;

    /* DEBUG TOGGLE: deterministic input record / replay (JIRC).
     * Opened from JCE_INPUT_RECORD / JCE_INPUT_REPLAY env vars at create;
     * ticked once per iterated frame right before the action map update;
     * closed (and thereby flushed) in jce_engine_destroy.  At most one of
     * the two is non-NULL (replay wins when both env vars are set). */
    JceInputRecorder            *input_recorder;
    JceInputRecorder            *input_replayer;

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
static void jce_modal_tick_cb(void *user);

/* Shared reentrancy / pause atomics for the resize watcher and the
 * modal-loop tick callback. */
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

/* PAK read provider: bridges os/core's VFS (and i18n) to the resource-layer
 * jce_pak_* implementation so those L2 modules never depend UP on the resource
 * tier.  Registered once at the top of jce_engine_create, before any PAK is
 * opened/mounted/read.  (See jce_fs_set_pak_provider / dependency inversion.) */
static const void *JCE_CALL eng_pak_find(const JcePakArchive *pak,
                                         const char *path)
{ return jce_pak_find(pak, path); }
static uint64_t JCE_CALL eng_pak_asset_size(const void *asset)
{ return ((const JcePakAsset *)asset)->original_size; }
static size_t JCE_CALL eng_pak_decompress(const void *asset, void *buf, size_t n)
{ return jce_pak_decompress((const JcePakAsset *)asset, buf, n); }
static const JceFsPakProvider ENG_PAK_PROVIDER = {
    eng_pak_find, eng_pak_asset_size, eng_pak_decompress
};

JceEngine *jce_engine_create(int argc, char *argv[])
{
    /* Snapshot argv first so any subsystem init below can read launch
     * flags through jce_args_* without each one re-parsing argv. */
    jce_args_stash(argc, argv);

    /* Install the PAK read provider before anything opens/mounts/reads a PAK
     * (dependency inversion: the os/core VFS + i18n call through this). */
    jce_fs_set_pak_provider(&ENG_PAK_PROVIDER);

    /* Logger + crash handler + config. */
    jce_log_init();
    jce_log_set_thread_name("MAIN");
    jce_thread_mark_main();
    jce_crash_handler_init();

    JceEngine *e = JCE_CALLOC(1, sizeof(*e));
    if (!e) return NULL;

    e->config = jce_config_defaults();
    {
        char cfg_path[512];
        jce_select_config_path(cfg_path, sizeof(cfg_path));
        jce_config_load(&e->config, cfg_path);
    }
    /* Settings S5: publish the boot-only perf knobs so the renderer (bgfx
     * pool sizing) and jobs layers — which never receive the JceConfig — can
     * read them.  Done before bgfx_init / any jce_jobs_default. */
    jce_config_publish_perf(e->config.machine_class, e->config.job_workers);

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
        /* Second instance: activate the first instance's window instead
         * of interrupting the user with a modal — restore it if
         * minimized, best-effort foreground, and flash its taskbar
         * button until it gains focus (the VS Code / Chrome behavior).
         * Then exit silently. */
        if (!jce_single_instance_activate_existing())
            LOG_WARN(LOG_TAG, "another instance of '%s' is already running "
                     "(no window published to activate) — exiting",
                     e->config.window_title[0] ? e->config.window_title : "JCE");
        SDL_Quit();
        JCE_FREE(e);
        return NULL;
    }

    /* Force landscape on mobile / mobile-web. JCE_PLATFORM_TOUCH covers
       exactly the orientation-locked targets (Android + iOS + tvOS). */
#if JCE_PLATFORM_TOUCH
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#if JCE_PLATFORM_WEB
    EM_ASM({
        if (screen.orientation && screen.orientation.lock)
            screen.orientation.lock('landscape').catch(function(){});
    });
#endif

    /* Asset encryption: reconstruct the shipped decryption key from its two
     * embedded XOR shares (key = share_a ^ share_b; see jce_embedded_assets.h)
     * and install it process-wide BEFORE the first PAK open so every archive
     * opened from here on — embedded PAK, file-loaded PAK (web/Android/JNI)
     * and later bundle mounts — decrypts transparently.  When no key was
     * embedded (present == 0) this is a no-op and plain assets work as
     * before.  The archive is authenticated, but the key necessarily ships
     * with the game, so this raises extraction cost rather than creating
     * client-side secrecy. */
    if (jce_embedded_pak_key_present) {
        uint8_t pak_key[32];
        for (int ki = 0; ki < 32; ++ki)
            pak_key[ki] = (uint8_t)(jce_embedded_pak_key_shares[ki] ^
                                    jce_embedded_pak_key_shares[32 + ki]);
        jce_archive_set_process_key(pak_key);
        jce_engine_secure_zero(pak_key, sizeof(pak_key));
    }

    /* Open PAK archive. */
#if JCE_PLATFORM_WEB
    e->pak = jce_pak_open_file("/game_assets.pak");
#elif JCE_PLATFORM_ANDROID
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
    /* Empty stub PAK (a freshly scaffolded project ships a 1-byte
     * placeholder) — skip opening so the engine boots; logical asset
     * lookups will fall back to the bundle catalog or fail gracefully. */
    if (assets_pak_data_size > 1) {
        e->pak = jce_pak_open(assets_pak_data, assets_pak_data_size);
    } else {
        LOG_WARN(LOG_TAG,
                 "no PAK embedded (assets_pak_data_size=%zu) — "
                 "running without an asset PAK.  Replace the stub by "
                 "running the asset packer for this project.",
                 (size_t)assets_pak_data_size);
        e->pak = NULL;
    }
#endif
#if !JCE_PLATFORM_WEB && !JCE_PLATFORM_ANDROID && !defined(JCE_BUILD_JNI)
    if (!e->pak && assets_pak_data_size > 1) {
        fatal_msg("Failed to open PAK archive");
        goto fail;
    }
#endif

    /* Optional bundle catalog (opt-in via jce_engine_set_bundle_catalog_path). */
    if (g_bundle_catalog_path[0]) {
        e->bundle_fs      = jce_fs_create();
        e->bundle_catalog = jce_bundle_catalog_open(e->bundle_fs,
                                                    g_bundle_catalog_path);
        if (!e->bundle_catalog) {
            LOG_WARN(LOG_TAG, "bundle catalog open failed: %s",
                     g_bundle_catalog_path);
            if (e->bundle_fs) {
                jce_fs_destroy(e->bundle_fs);
                e->bundle_fs = NULL;
            }
        }
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

    /* Publish the native handle so a later second instance can activate
     * this window (restore + foreground + taskbar flash) instead of
     * showing a modal. */
    {
        JceNativeWindow nw;
        jce_window_get_native(e->window, &nw);
        jce_single_instance_publish_window(nw.nwh);
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
           longer depends on resource/pak_loader).
           JCE_SHADER_DEV_DIR overlays freshly-compiled <dev_dir>/shaders/*.bin
           over the baked pak (per-shader fallback to pak), so a recompiled
           shader is picked up at startup without repacking — a dev/CI loop
           affordance.  Unset => byte-identical to the pak-only load. */
        const char *shader_dev_dir = getenv("JCE_SHADER_DEV_DIR");
        JceShaderSet shaders = (shader_dev_dir && shader_dev_dir[0])
            ? jce_shaders_load_all_fs(shader_dev_dir, e->pak)
            : jce_shaders_load_all(e->pak);
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

    /* Render Pipeline Asset (P3-E.4): pick `<cwd>/Settings/RenderPipeline.rp.json`
     * if present, otherwise fall back to the preset matching the GPU tier. */
    jce_render_pipeline_apply_boot("Settings/RenderPipeline.rp.json");

    /* Settings S7 follow-up: layer the player's saved in-game graphics
     * choices (jce.ini [graphics]) on top of the boot resolution above —
     * the same seam where the other jce.ini sections (window/audio) apply.
     * No-op for configs without the section. */
    jce_engine_apply_graphics_config(&e->config, e->renderer);

    e->input = jce_input_create();
    if (!e->input) {
        fatal_msg("Input system init failed");
        goto fail;
    }

    /* QW-input-actions — own the action-map layer in parallel with raw
     * input.  Load the editor-authored bindings if present; otherwise
     * seed the standard WASD/gamepad FPS defaults so games + the camera
     * controller always have a usable action set.  Updated each frame in
     * jce_engine_iterate; queried via jce_engine_get_actions / the
     * JceServices.actions handle. */
    {
        char actions_path[512];
        jce_select_input_actions_path(actions_path, sizeof(actions_path));
        e->actions = jce_actions_load_file(actions_path);
        if (e->actions) {
            LOG_INFO(LOG_TAG, "input actions loaded from %s", actions_path);
        } else {
            e->actions = jce_actions_create();
            if (e->actions) {
                jce_actions_bind_fps_defaults(e->actions);
                LOG_INFO(LOG_TAG, "input actions: using built-in FPS defaults");
            } else {
                LOG_WARN(LOG_TAG, "input actions init failed");
            }
        }
    }

    /* DEBUG TOGGLE: JCE_INPUT_RECORD / JCE_INPUT_REPLAY — deterministic
     * input record/replay to/from a .jirc file (same diagnostic env family
     * as JCE_BACKEND / JCE_CAPTURE_FRAME).  Recording captures the exact
     * JceInputFrame snapshot the game reads each frame; replay overrides
     * live input from the file until EOF, then falls back to live input.
     * Use for bug repros, regression runs, and demos.  Replay wins when
     * both are set (recording a replay would only copy the file). */
    {
        const char *rec_path = SDL_getenv("JCE_INPUT_RECORD");
        const char *rep_path = SDL_getenv("JCE_INPUT_REPLAY");
        if (rep_path && rep_path[0]) {
            e->input_replayer = jce_input_replay_open(rep_path);
            if (e->input_replayer)
                LOG_WARN(LOG_TAG,
                    "DEBUG TOGGLE: JCE_INPUT_REPLAY=%s -> input replayed from file",
                    rep_path);
            else
                LOG_WARN(LOG_TAG,
                    "JCE_INPUT_REPLAY=%s could not be opened "
                    "(missing file or header/version mismatch) — ignored",
                    rep_path);
            if (rec_path && rec_path[0])
                LOG_WARN(LOG_TAG,
                    "JCE_INPUT_RECORD ignored while JCE_INPUT_REPLAY is set");
        } else if (rec_path && rec_path[0]) {
            e->input_recorder = jce_input_record_open(rec_path);
            if (e->input_recorder)
                LOG_WARN(LOG_TAG,
                    "DEBUG TOGGLE: JCE_INPUT_RECORD=%s -> input recorded to file",
                    rec_path);
            else
                LOG_WARN(LOG_TAG,
                    "JCE_INPUT_RECORD=%s could not be opened for writing — ignored",
                    rec_path);
        }
    }

    /* JCE_AUDIO_DISABLE: skip audio entirely (diagnostic A/B — on wasm the
     * miniaudio callback runs on the MAIN thread via ScriptProcessorNode,
     * so this isolates "is the frame-time hole the audio path?").  The
     * NULL-audio path below is the same one an init failure takes, which
     * every consumer already tolerates. */
    if (getenv("JCE_AUDIO_DISABLE") != NULL) {
        e->audio = NULL;
        LOG_WARN(LOG_TAG, "JCE_AUDIO_DISABLE set — audio OFF (diagnostic)");
    } else {
        e->audio = jce_audio_create();
    }
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

    /* Headless / CI auto-quit after N frames (0 = disabled). */
    e->max_frames  = read_positive_u32_env("JCE_MAX_FRAMES", 0u);
    e->frame_index = 0u;
    if (e->max_frames)
        LOG_INFO(LOG_TAG, "JCE_MAX_FRAMES=%u: will auto-quit after %u frames",
                 e->max_frames, e->max_frames);

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
        .actions  = e->actions,
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
       during platform modal message loops (needed for JNI bridge). */
    SDL_AddEventWatch(jce_resize_event_watch, e);

    /* Keep rendering while the user holds the title bar / window border
       on platforms with a modal sizing loop (Windows). No-op elsewhere. */
    jce_window_install_modal_tick(jce_modal_tick_cb, e);

    /* P3-B.3 — wire OS LOW_MEMORY signals into the streaming pressure
     * system (pairs with the P3-A.2 mip-streaming hook).  Priority is
     * very low so engine-internal teardown runs before any consumer
     * listeners (consumers may free game-specific caches afterwards). */
    {
        JceLifecycleHandle h = jce_lifecycle_register(
            engine_lifecycle_listener, /*priority*/ -1000, NULL);
        (void)h;   /* released in bulk via jce_lifecycle_shutdown() */
    }

    jce_engine_reset_frame_clock(e);

    return e;

fail:
    jce_engine_destroy(e);
    return NULL;
}

/* -- Modal-loop tick callback ------------------------------------- */

/*
 * On Windows, clicking and holding the title bar or window border enters
 * a modal message loop inside DefWindowProc (WM_ENTERSIZEMOVE).  During
 * this loop, SDL_PollEvent never returns, so the JNI bridge's iterate
 * loop is completely stalled — even if the user doesn't move the mouse.
 *
 * The portable jce_window_install_modal_tick() hook (Windows: SetTimer
 * driven from a SDL_SetWindowsMessageHook) calls back here at ~60 fps
 * while the modal loop is active; the resize watcher below handles
 * resize events specifically (bgfx reset + viewport update).
 */
static void jce_modal_tick_cb(void *user)
{
    JceEngine *e = (JceEngine *)user;
    if (!e || !e->renderer) return;

    /* Cooperate with jce_resize_event_watch: only one of the two paths
     * may be inside begin_frame/end_frame at a time.  Skipping a tick
     * is safe — the next 16 ms tick will pick up. */
    if (SDL_GetAtomicInt(&s_in_render_frame) != 0)
        return;

    jce_engine_iterate(e);
}

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
     * backbuffer (which leaves bgfx in a broken state on restore).
     * EXCEPTION: benchmark/CI runs (JCE_MAX_FRAMES set) keep iterating to
     * their auto-quit frame even when minimized — minimizing the popup IS the
     * user's "run it in the background" gesture, and pausing silently starves
     * the JCE_PERF_LOG window forever.  The 0-size hazard stays covered: the
     * minimize event is not a size event, and the size-refresh path below
     * already rejects pw/ph <= 0, so bgfx never resets to 0x0 — rendering
     * continues at the last good backbuffer size (present goes occluded). */
    if (t == SDL_EVENT_WINDOW_MINIMIZED) {
        static int s_bench = -1;
        if (s_bench < 0) s_bench = (SDL_getenv("JCE_MAX_FRAMES") != NULL) ? 1 : 0;
        if (!s_bench)
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

/* Translate one SDL_Event into our backend-neutral JceEvent.
 * Unknown SDL event types map to JCE_EVENT_OTHER so applications can
 * still distinguish "got an event" from "no event". */
static void translate_sdl_event(const SDL_Event *src, JceEvent *dst)
{
    dst->type = JCE_EVENT_OTHER;

    switch (src->type) {
    case SDL_EVENT_QUIT:
        dst->type = JCE_EVENT_QUIT;
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        dst->type = (src->type == SDL_EVENT_KEY_DOWN)
                  ? JCE_EVENT_KEY_DOWN : JCE_EVENT_KEY_UP;
        dst->key.scancode = (JceKey)src->key.scancode;
        dst->key.mod      = (uint16_t)src->key.mod;
        dst->key.repeat   = src->key.repeat ? true : false;
        break;

    case SDL_EVENT_TEXT_INPUT:
        dst->type = JCE_EVENT_TEXT_INPUT;
        if (src->text.text) {
            size_t n = strlen(src->text.text);
            if (n >= sizeof(dst->text.text))
                n = sizeof(dst->text.text) - 1;
            memcpy(dst->text.text, src->text.text, n);
            dst->text.text[n] = '\0';
        } else {
            dst->text.text[0] = '\0';
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
        dst->type = JCE_EVENT_MOUSE_MOTION;
        dst->motion.x    = src->motion.x;
        dst->motion.y    = src->motion.y;
        dst->motion.xrel = src->motion.xrel;
        dst->motion.yrel = src->motion.yrel;
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        dst->type = (src->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                  ? JCE_EVENT_MOUSE_BUTTON_DOWN : JCE_EVENT_MOUSE_BUTTON_UP;
        dst->button.button = src->button.button;  /* SDL_BUTTON_* == JCE_MOUSE_BUTTON_* */
        dst->button.clicks = src->button.clicks;
        dst->button.x      = src->button.x;
        dst->button.y      = src->button.y;
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        dst->type = JCE_EVENT_MOUSE_WHEEL;
        dst->wheel.x = src->wheel.x;
        dst->wheel.y = src->wheel.y;
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        dst->type = JCE_EVENT_WINDOW_FOCUS_GAINED;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        dst->type = JCE_EVENT_WINDOW_FOCUS_LOST;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        dst->type = JCE_EVENT_WINDOW_RESIZED;
        dst->resize.w = (uint32_t)src->window.data1;
        dst->resize.h = (uint32_t)src->window.data2;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        dst->type = JCE_EVENT_WINDOW_CLOSE;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        dst->type = JCE_EVENT_WINDOW_EXPOSED;
        break;

    default:
        break;
    }
}

JceAppResult jce_engine_event(JceEngine *e, const void *platform_event)
{
    const SDL_Event *event = (const SDL_Event *)platform_event;

    /* P3-B.3 — translate SDL platform-lifecycle events to JCE lifecycle
     * events.  Runs BEFORE quit handling so listeners get WILL_QUIT
     * just before we propagate JCE_APP_SUCCESS.  Single switch keeps
     * the dispatch table near the SDL_Event types it consumes; unmapped
     * SDL events simply fall through.
     *
     * SDL3 event coverage (vendored):
     *   SDL_EVENT_WINDOW_FOCUS_GAINED / _LOST       (all platforms)
     *   SDL_EVENT_WINDOW_MINIMIZED / _RESTORED      (desktop pause/resume)
     *   SDL_EVENT_DID_ENTER_BACKGROUND               (mobile pause)
     *   SDL_EVENT_WILL_ENTER_FOREGROUND              (mobile resume)
     *   SDL_EVENT_LOW_MEMORY                         (mobile, sometimes desktop)
     *   SDL_EVENT_TERMINATING                        (mobile force-kill warning)
     *   SDL_EVENT_RENDER_DEVICE_RESET                (D3D/Vulkan device-lost recovery)
     *
     * DEVICE_LOST has no direct SDL3 counterpart yet; the enum value
     * is defined so the renderer layer can emit it manually when bgfx
     * surfaces a device-lost state.  Document changes in the commit
     * body if SDL exposes a dedicated event later. */
    switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
        break;
    case SDL_EVENT_LOW_MEMORY:
        jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
        jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_RESET);
        break;
    default:
        break;
    }

    if (event->type == SDL_EVENT_TERMINATING) {
        jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
        return JCE_APP_SUCCESS;
    }

    JceEvent ev = (JceEvent){0};
    translate_sdl_event(event, &ev);

    /* Treat both SDL_EVENT_QUIT and SDL_EVENT_WINDOW_CLOSE_REQUESTED as
     * application-level quit signals.  SDL3 does not automatically
     * synthesise SDL_EVENT_QUIT when a window's close button is
     * clicked, so without this we would silently drop the close
     * request — which is exactly the bug reported in SDL software
     * fallback mode (the blue/orange info-panel window had no in-app
     * quit UI, so its X button appeared dead). */
    if (event->type == SDL_EVENT_QUIT ||
        event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (g_app_desc.on_event)
            g_app_desc.on_event(&ev, g_app_desc.user_data);

        /* In SDL software fallback mode the application's init() callback
         * was skipped (we early-return after creating the fallback
         * renderer so heavy subsystems like input/audio/scripting are
         * never brought up).  That means any `should_quit` callback the
         * app registered will dereference a null app-state and return
         * false — leaving the user with an unclosable window.  Bypass
         * the callback in fallback mode and quit immediately so the
         * window's X button always works in the safe-mode UI. */
        if (e->renderer && jce_renderer_is_fallback(e->renderer)) {
            jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
            return JCE_APP_SUCCESS;
        }

        /* If the application registered a should_quit callback, give it
           a chance to intercept the quit (e.g. to show an unsaved-changes
           dialog).  If the callback returns false, swallow the event. */
        if (g_app_desc.should_quit) {
            if (g_app_desc.should_quit(g_app_desc.user_data)) {
                jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
                return JCE_APP_SUCCESS;
            }
            return JCE_APP_CONTINUE;
        }
        jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
        return JCE_APP_SUCCESS;
    }

    if (e->input) jce_input_handle_event(e->input, event);

    /* Resize handling is done in jce_resize_event_watch() which fires
       from both SDL_PollEvent and Windows modal message loops. */

    if (g_app_desc.on_event)
        g_app_desc.on_event(&ev, g_app_desc.user_data);

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

    /* Periodic allocator trim (512MB charter): mimalloc's purge is
     * OPPORTUNISTIC — it only runs during allocation activity, so an idle
     * process (steady-state rendering is 0 allocs/frame) retains its
     * startup-peak commit forever (measured: ~1.3GB of the editor's asset
     * decode/cook transient stayed committed at idle).  A slow tick returns
     * freed-but-retained segments to the OS on every machine; charter-class
     * boxes (low RAM / single core, or JCE_LOW_MEM=1) additionally switch
     * the allocator to immediate-purge and trim aggressively. */
    {
        static uint64_t s_trim_last = 0;
        static int      s_low_mem   = -1;
        if (s_low_mem < 0) {
            const int ram_mb = SDL_GetSystemRAM();
            const int cores  = SDL_GetNumLogicalCPUCores();
            bool lm = (ram_mb > 0 && ram_mb < 2048) || cores <= 1;
            /* Settings S5: jce.ini machine_class overrides the auto-detect
             * (same precedence as apply_transient_limits); env still wins. */
            switch (jce_config_machine_class()) {
            case JCE_MACHINE_CLASS_LOW:  lm = true;  break;
            case JCE_MACHINE_CLASS_FULL: lm = false; break;
            default: break;
            }
            const char *ev = getenv("JCE_LOW_MEM");
            if (ev && ev[0]) lm = (ev[0] != '0');
            s_low_mem = lm ? 1 : 0;
            if (lm) jce_alloc_low_mem_mode(true);
        }
        const uint64_t trim_every =
            e->perf_freq * (uint64_t)(s_low_mem ? 10u : 30u);
        if (e->perf_freq > 0 && now - s_trim_last >= trim_every) {
            if (s_trim_last != 0)   /* skip the boot window (startup allocs) */
                jce_alloc_trim(s_low_mem != 0);
            s_trim_last = now;
        }
    }

    if (e->perf_freq > 0 &&
        e->frame_counter_prev > 0 &&
        now >= e->frame_counter_prev) {
        const double elapsed = (double)(now - e->frame_counter_prev);
        dt = (float)(elapsed / (double)e->perf_freq);
    }

    e->frame_counter_prev = now;

    if (dt < 0.0f) dt = 0.0f;
    if (dt > JCE_MAX_FRAME_DT) dt = JCE_MAX_FRAME_DT;

    if (jce_engine_quit_requested()) {
        jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
        return JCE_APP_SUCCESS;
    }

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

    /* ── PlayerLoop: INITIALIZATION / EARLY_UPDATE ──────────────────
     * Initialization runs before any per-frame work; EarlyUpdate is
     * for input poll and event-drain style hooks that need to see a
     * fresh frame.  Engine-owned input update remains at END_OF_FRAME
     * below (it drives swap-edge / just-pressed detection). */
    jce_player_loop_run_phase(JCE_PHASE_INITIALIZATION, dt);
    jce_player_loop_run_phase(JCE_PHASE_EARLY_UPDATE, dt);

    /* DEBUG TOGGLE: input record / replay (JCE_INPUT_RECORD /
     * JCE_INPUT_REPLAY, opened in jce_engine_create).  This sits right
     * before the action-map update on purpose: replay injects the recorded
     * snapshot BEFORE anything reads input this frame, so raw-input
     * queries AND actions observe the replayed state; record captures the
     * exact snapshot the game is about to read.  The END_OF_FRAME
     * jce_input_update below rolls cur→prev as usual, so pressed/released
     * edges reconstruct identically on replay.  At replay EOF: log once,
     * close, and keep running on live input. */
    if (e->input_replayer && e->input) {
        if (!jce_input_replay_tick(e->input_replayer, e->input)) {
            LOG_WARN(LOG_TAG,
                "input replay finished after %llu frames — back to live input",
                (unsigned long long)
                    jce_input_record_frame_count(e->input_replayer));
            jce_input_record_close(e->input_replayer);
            e->input_replayer = NULL;
        }
    } else if (e->input_recorder && e->input) {
        if (!jce_input_record_tick(e->input_recorder, e->input)) {
            LOG_WARN(LOG_TAG, "input record write failed — recording stopped");
            jce_input_record_close(e->input_recorder);
            e->input_recorder = NULL;
        }
    }

#if defined(JCE_ENABLE_AI_DISPATCH) && JCE_ENABLE_AI_DISPATCH
    /* ai_dispatch: constraint records enter the frame at input parity —
     * this sits beside the input record/replay hook on purpose.  Replay
     * pumps the .jarc stream gated by the canonical tick; live results
     * pump within the frame budget (spec H/J).  No-op until the host
     * calls jce_aid_init(). */
    if (jce_aid_initialised())
        jce_aid_engine_tick(jce_fixed_clock_default()->tick_count, 0);
#endif

    /* QW-input-actions — evaluate the action map against the current
     * input snapshot so FIXED_UPDATE / UPDATE consumers (games, camera
     * controller) read fresh action values this frame.  Raw-input queries
     * remain available in parallel; this only resurrects the action layer.
     * prev_value bookkeeping (for jce_action_pressed/_released) advances
     * once per call here, matching the once-per-frame edge contract. */
    if (e->actions && e->input)
        jce_actions_update(e->actions, e->input);

    /* ── PlayerLoop: FIXED_UPDATE (P3-B.2) ──────────────────────────
     * Glenn Fiedler accumulator: drives 0..N fixed steps per frame so
     * physics + future deterministic netcode see a stable cadence
     * regardless of render rate.  Spiral-of-death clamp lives inside
     * jce_fixed_clock_advance.  This is the SAME clock the per-runtime
     * physics step adopts its cadence from (P1-fixed-clock-unify), so a
     * jce_engine_set_fixed_hz() change reaches physics too.  Configurable
     * via jce_engine_set_fixed_hz (default 60 Hz, == physics step). */
    {
        JceFixedClock *fc = jce_fixed_clock_default();
        const uint32_t steps = jce_fixed_clock_advance(fc, (double)dt);
        const float    fdt   = (float)fc->fixed_dt;
        for (uint32_t i = 0; i < steps; ++i) {
            jce_player_loop_run_phase(JCE_PHASE_FIXED_UPDATE, fdt);
            jce_fixed_clock_tick(fc);
        }
    }

    /* ── PlayerLoop: PRE_RENDER ─────────────────────────────────────
     * Fires immediately before bgfx begin_frame so hooks can prep
     * frame-local GPU state without racing the renderer. */
    jce_player_loop_run_phase(JCE_PHASE_PRE_RENDER, dt);

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

    /* ── PlayerLoop: UPDATE ─────────────────────────────────────────
     * Gameplay / ECS world tick.  Runs after legacy subsystem update
     * so phase consumers observe the same world state the app does. */
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, dt);

    if (g_app_desc.update) {
        JCE_PROFILE_ZONE_N("App::UpdateAndDraw");
        uint64_t _t0_app = jce_time_perf_counter();
        g_app_desc.update(dt, g_app_desc.user_data);
        if (g_app_desc.draw)
            g_app_desc.draw(&e->svc, g_app_desc.user_data);
        jce_perf_phase_add("app_update",
                           jce_time_perf_to_ms(_t0_app, jce_time_perf_counter()));
        JCE_PROFILE_ZONE_END;
    }

    /* ── PlayerLoop: LATE_UPDATE ────────────────────────────────────
     * Post-gameplay: cameras, IK, anim post-processing. */
    jce_player_loop_run_phase(JCE_PHASE_LATE_UPDATE, dt);

    {
        /* end_frame = bgfx_frame kick + any API-thread wait: the gap between
         * app_update (all JCE-side submit work incl. scene_render) and the
         * bgfx-reported cpu_frame_ms lives here. */
        uint64_t _t0_ef = jce_time_perf_counter();
        jce_renderer_end_frame(e->renderer);
        jce_perf_phase_add("end_frame",
                           jce_time_perf_to_ms(_t0_ef, jce_time_perf_counter()));
    }

    /* ── PlayerLoop: POST_RENDER ────────────────────────────────────
     * After the renderer submits but before we release the in-frame
     * guard, so hooks can still touch frame-local resources. */
    jce_player_loop_run_phase(JCE_PHASE_POST_RENDER, dt);

    /* Promote deferred render-pipeline toggles (settings slice S1): the
     * per-feature setters write a pending descriptor; landing it here —
     * after every submit of the frame — keeps mid-frame draws consistent.
     * No-op when nothing called a setter this frame. */
    jce_render_pipeline_end_frame();

    SDL_SetAtomicInt(&s_in_render_frame, 0);

    {
        JCE_PROFILE_ZONE_N("Input::Update");
        jce_input_update(e->input);
        JCE_PROFILE_ZONE_END;
    }

    /* ── PlayerLoop: END_OF_FRAME ───────────────────────────────────
     * Last thing before the profiler frame mark.  Use for screenshot
     * captures, async readbacks, and per-frame analytics. */
    jce_player_loop_run_phase(JCE_PHASE_END_OF_FRAME, dt);

    JCE_PROFILE_FRAME_MARK;
    JCE_PROFILE_ZONE_END;

    /* Perf telemetry (JCE_PERF_LOG): rolling frame-time / FPS over a 120-frame
     * window, logged to the engine log.  Env-gated so it is ZERO cost when
     * unset.  Engine-wide (any app/world, runtime or headless) so perf is
     * measurable without per-game instrumentation — the standalone had no
     * built-in frame telemetry, which made "is the big world fast?" unanswerable
     * without a profiler.  worst = the spike frame in the window (hitches). */
    {
        static int s_perf_on = -1;
        if (s_perf_on < 0) {
            /* libc getenv, NOT SDL_getenv: on Emscripten SDL keeps its own
             * env table and never sees Module.ENV entries, so the web
             * `?perflog` hook silently failed to arm the profiler. */
            s_perf_on = (getenv("JCE_PERF_LOG") != NULL) ? 1 : 0;
            jce_perf_phase_set_enabled(s_perf_on);
        }
        if (s_perf_on) {
            static double   s_acc_ms = 0.0;
            static double   s_worst_ms = 0.0;
            static uint32_t s_n = 0u;
            /* Ring of the last N frame times for the 1%/0.1%-low percentiles.
             * The first window is warmup (shader/PSO compile, asset upload, the
             * one-time load spike) — excluded from the ring so the percentiles
             * report STEADY-STATE hitches, not the startup frame (mirrors how UE
             * stat unit / a benchmark's warmup phase discards early frames). */
            static double   s_ring[JCE_PERF_RING];
            static uint32_t s_ring_head = 0u, s_ring_count = 0u;
            static uint32_t s_warm = 0u;
            const double    ms = (double)dt * 1000.0;
            s_acc_ms += ms;
            if (ms > s_worst_ms) s_worst_ms = ms;
            if (s_warm < 120u) {
                s_warm++;
            } else {
                s_ring[s_ring_head] = ms;
                s_ring_head = (s_ring_head + 1u) & (JCE_PERF_RING - 1u);
                if (s_ring_count < JCE_PERF_RING) s_ring_count++;
            }
            if (++s_n >= 120u) {
                const double avg = s_acc_ms / (double)s_n;
                /* Pull the last frame's CPU/GPU split + draw count so the log
                 * tells you WHICH way it is bound (GPU+high draws → LOD/overdraw;
                 * CPU high → cull/entity).  One representative sample at steady
                 * state; the avg/worst are the windowed wall-clock. */
                /* Process RSS SELF-REPORTED in the perf line: external samplers
                 * (PowerShell WorkingSet polls) race short profiling runs and
                 * can attribute another editor instance's memory to this one —
                 * the in-process readout is always attributable and catches
                 * the steady state exactly at the report window. */
                long long rss_mb = 0, commit_mb = 0;
                {
                    JceMemStats mstat;
                    if (jce_mem_stats(&mstat)) {
                        rss_mb    = (long long)(mstat.current_rss    >> 20);
                        commit_mb = (long long)(mstat.current_commit >> 20);
                    }
                }
                JceGpuStats gs;
                if (jce_renderer_get_gpu_stats(&gs) && gs.valid) {
                    /* rss = OS working set (sticky: Windows does not trim an
                     * unpressured process, so it reflects the PEAK more than
                     * the present); commit = committed private bytes — the
                     * truthful steady-state figure for the 512MB budget. */
                    LOG_INFO(LOG_TAG,
                             "perf: %.2f ms avg (%.0f FPS) | cpu %.1f / gpu %.1f ms | %u draws | gpu-mem %lld MB | rss %lld MB | commit %lld MB | worst %.2f ms / %u",
                             avg, (avg > 0.0) ? (1000.0 / avg) : 0.0,
                             gs.cpu_frame_ms, gs.gpu_ms, gs.num_draw,
                             (long long)(gs.gpu_memory_used > 0 ? gs.gpu_memory_used >> 20 : 0),
                             rss_mb, commit_mb, s_worst_ms, s_n);
                } else {
                    LOG_INFO(LOG_TAG,
                             "perf: %.2f ms/frame avg (%.0f FPS), worst %.2f ms, over %u frames",
                             avg, (avg > 0.0) ? (1000.0 / avg) : 0.0, s_worst_ms, s_n);
                }
                {
                    /* Standard-engine hitch metric: 1%-low (p99) / 0.1%-low
                     * (p99.9) frame times over the ring, plus p50/p95.  Sort a
                     * copy of the resident samples (no per-frame cost). */
                    static double srt[JCE_PERF_RING];
                    uint32_t rc = s_ring_count;
                    if (rc > 0) {
                    memcpy(srt, s_ring, (size_t)rc * sizeof(double));
                    qsort(srt, rc, sizeof(double), jce_perf_cmp_d);
                    const double p50  = jce_perf_pct(srt, rc, 0.50);
                    const double p95  = jce_perf_pct(srt, rc, 0.95);
                    const double p99  = jce_perf_pct(srt, rc, 0.99);
                    const double p999 = jce_perf_pct(srt, rc, 0.999);
                    LOG_INFO(LOG_TAG,
                        "perf-lows: p50 %.2f | p95 %.2f | 1%%low(p99) %.2f ms (%.0f FPS) | 0.1%%low(p99.9) %.2f ms (%.0f FPS) | over %u frames",
                        p50, p95, p99, (p99 > 0.0) ? 1000.0 / p99 : 0.0,
                        p999, (p999 > 0.0) ? 1000.0 / p999 : 0.0, rc);
                    }
                }
                {
                    char phase_buf[512];
                    jce_perf_phase_report(phase_buf, (int)sizeof(phase_buf));
                    if (phase_buf[0])
                        LOG_INFO(LOG_TAG, "perf-phases: %s", phase_buf);
                }
                {
                    /* Per-VIEW GPU breakdown — WHICH pass costs the GPU (shadow
                     * cascades vs gbuffer/foliage raster vs SSAO vs lighting vs
                     * postfx).  bgfx per-view GPU timers are enabled by the
                     * profiler flag we set under JCE_PERF_LOG.  One representative
                     * frame's slice; ranks the top views by GPU ms so the log
                     * answers "which pass is the GPU wall". */
                    const JceFrameStats *fs = jce_gfx_stats_capture();
                    if (fs && fs->gpu_timer_freq > 0 && fs->view_stats_count > 0) {
                        int idx[64];
                        int nv = fs->view_stats_count < 64 ? fs->view_stats_count : 64;
                        for (int i = 0; i < nv; i++) idx[i] = i;
                        /* insertion sort by GPU span, descending (nv is small) */
                        for (int i = 1; i < nv; i++) {
                            int k = idx[i];
                            int64_t kg = fs->view_stats[k].gpu_time_end - fs->view_stats[k].gpu_time_begin;
                            int j = i - 1;
                            while (j >= 0) {
                                int64_t jg = fs->view_stats[idx[j]].gpu_time_end - fs->view_stats[idx[j]].gpu_time_begin;
                                if (jg >= kg) break;
                                idx[j + 1] = idx[j]; j--;
                            }
                            idx[j + 1] = k;
                        }
                        char vbuf[512];
                        int off = 0;
                        double inv = 1000.0 / (double)fs->gpu_timer_freq;
                        for (int i = 0; i < nv && i < 8; i++) {
                            const JceViewStats *v = &fs->view_stats[idx[i]];
                            double g = (double)(v->gpu_time_end - v->gpu_time_begin) * inv;
                            if (g < 0.01) break;   /* stop at negligible views */
                            int n = snprintf(vbuf + off, sizeof(vbuf) - (size_t)off,
                                             "%s%s#%u=%.2f", (off ? " " : ""),
                                             v->name[0] ? v->name : "?",
                                             (unsigned)v->view_id, g);
                            if (n < 0 || off + n >= (int)sizeof(vbuf)) break;
                            off += n;
                        }
                        if (off > 0)
                            LOG_INFO(LOG_TAG, "perf-gpu-views: %s", vbuf);
                    }
                }
                {
                    /* rank-9: per-frame heap churn over this window (the first
                     * window includes one-time startup allocs; later windows are
                     * steady-state — watch those for per-frame churn regressions). */
                    uint64_t af = 0, ab = 0;
                    jce_alloc_frame_delta(&af, &ab);
                    LOG_INFO(LOG_TAG, "perf-alloc: %.1f allocs/frame, %.1f KB/frame",
                             (double)af / (double)s_n,
                             (double)ab / 1024.0 / (double)s_n);
                }
                s_acc_ms = 0.0;
                s_worst_ms = 0.0;
                s_n = 0u;
            }
        }
    }

    /* Headless / CI auto-quit: stop cleanly once we've rendered the requested
     * number of frames.  Counts AFTER a full frame so frame N's work (and any
     * END_OF_FRAME screenshot hook) has run.  Mirrors the SDL_EVENT_QUIT path:
     * emit WILL_QUIT, then return SUCCESS to end the loop. */
    if (e->max_frames) {
        e->frame_index++;
        if (e->frame_index >= e->max_frames) {
            LOG_INFO(LOG_TAG, "JCE_MAX_FRAMES reached (%u) — quitting",
                     e->max_frames);
            jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
            return JCE_APP_SUCCESS;
        }
    }

    return JCE_APP_CONTINUE;
}

/* -- Shutdown ------------------------------------------------------ */

void *jce_engine_get_bundle_catalog(JceEngine *e)
{
    return e ? e->bundle_catalog : NULL;
}

JceInputActions *jce_engine_get_actions(JceEngine *e)
{
    return e ? e->actions : NULL;
}

void jce_engine_destroy(JceEngine *e)
{
    if (!e) return;

    SDL_RemoveEventWatch(jce_resize_event_watch, e);

    jce_window_uninstall_modal_tick();

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
    /* jce_text_shutdown() now runs inside jce_renderer_destroy() before
     * bgfx_shutdown(), preserving the LIFO contract for any future
     * GPU-touching cleanup the text subsystem may grow. */
    if (e->renderer) jce_renderer_destroy(e->renderer);
    /* Shared data-parallel job pool (frustum cull etc.) — joined after the
     * renderer so no cull is in flight. */
    jce_jobs_shutdown_default();
    if (e->audio)    jce_audio_destroy(e->audio);
    if (e->bundle_catalog) {
        jce_bundle_catalog_close((JceBundleCatalog *)e->bundle_catalog);
        e->bundle_catalog = NULL;
    }
    if (e->bundle_fs) { jce_fs_destroy(e->bundle_fs); e->bundle_fs = NULL; }
    /* Game localization table is process-global (initialised lazily by
     * jce_runtime_create / the editor); release it before the PAK it may
     * borrow as a fallback source goes away. */
    jce_loc_shutdown();
    if (e->pak)      jce_pak_close(e->pak);

    /* Finalize input record / replay (JCE_INPUT_RECORD / JCE_INPUT_REPLAY).
     * Closing the recorder flushes the .jirc to disk; do it before the
     * input system it snapshots goes away. */
    if (e->input_recorder) {
        LOG_INFO(LOG_TAG, "input recording finalized (%llu frames)",
            (unsigned long long)
                jce_input_record_frame_count(e->input_recorder));
        jce_input_record_close(e->input_recorder);
        e->input_recorder = NULL;
    }
    if (e->input_replayer) {
        jce_input_record_close(e->input_replayer);
        e->input_replayer = NULL;
    }

    if (e->actions)  jce_actions_destroy(e->actions);
    if (e->input)    jce_input_destroy(e->input);
    if (e->window)   jce_window_destroy(e->window);
    jce_single_instance_unlock();
    JCE_FREE(e);

    /* The async log backend runs on an SDL thread and parks on SDL
       mutex/condvar primitives — it MUST be shut down while SDL is still
       alive. The previous order (SDL_Quit first, log shutdown last "so all
       teardown messages are captured") made SDL_WaitThread join through a
       dead SDL: the join silently failed, the ring was freed under the
       still-running backend thread, and when that thread woke from its
       100 ms flush timeout it dereferenced the freed ring — a 0xC0000005
       during CRT exit. The editor usually won that 100 ms race; the SDK
       smoke consumer (heavier atexit work from the whole-archive fat lib)
       lost it deterministically. Later teardown messages are still
       captured: jce_log_write falls back to synchronous stderr emission
       once the ring is gone. */
    jce_log_shutdown();

    /* SDL_Init pairs with SDL_Quit; perform it after every other
     * SDL-dependent subsystem is gone so OS resources owned by SDL are
     * released last. */
    SDL_Quit();

    /* Restore default crash handlers after all subsystems are down. */
    jce_crash_handler_shutdown();

    /* Release PlayerLoop storage after all subsystems are torn down so
     * any teardown-time callbacks have already fired. */
    jce_player_loop_shutdown();
    jce_lifecycle_shutdown();
}

/* ---- FixedUpdate cadence (P3-B.2) ------------------------------ */

void jce_engine_set_fixed_hz(double hz)
{
    JceFixedClock *fc = jce_fixed_clock_default();
    const double fixed_dt = (hz > 0.0) ? (1.0 / hz) : (1.0 / 60.0);
    /* Preserve max_frame_dt + counters; only retune cadence. */
    fc->fixed_dt = fixed_dt;
    /* Drop a stale accumulator that no longer matches the new step. */
    if (fc->accumulator > fixed_dt)
        fc->accumulator = fixed_dt;
}

double jce_engine_get_fixed_hz(void)
{
    const JceFixedClock *fc = jce_fixed_clock_default();
    return (fc->fixed_dt > 0.0) ? (1.0 / fc->fixed_dt) : 0.0;
}
