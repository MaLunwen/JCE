/*
 * jce_renderer.c  bgfx renderer implementation.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/platform/jce_library.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"
#include "os/platform/jce_window_internal.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>   /* IMG_SavePNG for backbuffer screenshots */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_renderer"

static uint32_t s_bgfx_frame_index = 0;

#if JCE_PLATFORM_ANDROID
/* ── Android bgfx-frame side thread ─────────────────────────────────────
 * WSA (Windows Subsystem for Android) uses libEGL_emulation.so, whose
 * eglSwapBuffers hangs indefinitely after ~16 frames.  bgfx's render
 * thread blocks in that call, and bgfx_frame() on the SDL game thread
 * then blocks in renderSemWait() waiting for the stuck render thread.
 * That makes the SDL thread unresponsive → ANR.
 *
 * Fix: run bgfx_frame on a dedicated side thread.  The SDL game thread
 * signals the side thread and returns immediately (fire-and-forget).
 * When eglSwapBuffers hangs, only the side thread blocks; the SDL game
 * thread stays alive and Android never fires ANR.
 *
 * Trade-off: once the EGL hang occurs, s_egl_hung=true and all further
 * bgfx draw calls are skipped. Rendering freezes but the app lives.
 *
 * Threading is done through the engine's portable jce_thread/semaphore
 * wrappers (SDL3-backed) — no raw pthread/sem_t in engine code.
 */
static JceThread    *s_frame_thread       = NULL;
static JceSemaphore *s_frame_req          = NULL; /* game→side: "call bgfx_frame" */
static SDL_AtomicInt s_frame_done         = {1}; /* 1=idle, 0=in-progress         */
static SDL_AtomicInt s_egl_hung           = {0}; /* 1=eglSwapBuffers hung          */
static SDL_AtomicInt s_frame_kick_ms      = {0}; /* SDL_GetTicks() at last kick    */
static SDL_AtomicInt s_frame_thread_live  = {0};

#define ANDROID_EGL_HANG_TIMEOUT_MS 2000    /* 2 s without done → hung        */

static void android_bgfx_frame_thread(void *arg)
{
    (void)arg;
    while (SDL_GetAtomicInt(&s_frame_thread_live)) {
        jce_semaphore_wait(s_frame_req);
        if (!SDL_GetAtomicInt(&s_frame_thread_live)) break;
        s_bgfx_frame_index = bgfx_frame(false); /* may block forever in eglSwapBuffers on WSA */
        SDL_SetAtomicInt(&s_frame_done, 1);
    }
}

static void android_frame_thread_start(void)
{
    s_frame_req = jce_semaphore_create(0);
    SDL_SetAtomicInt(&s_frame_thread_live, 1);
    SDL_SetAtomicInt(&s_frame_done, 1);
    SDL_SetAtomicInt(&s_egl_hung, 0);
    s_frame_thread = jce_thread_create(android_bgfx_frame_thread, NULL,
                                       "jce-bgfx-frame");
}

static void android_frame_thread_stop(void)
{
    SDL_SetAtomicInt(&s_frame_thread_live, 0);
    if (s_frame_req) jce_semaphore_signal(s_frame_req);  /* wake thread to exit */
    if (s_frame_thread) {
        jce_thread_join(s_frame_thread);
        s_frame_thread = NULL;
    }
    if (s_frame_req) {
        jce_semaphore_destroy(s_frame_req);
        s_frame_req = NULL;
    }
}

/* Called instead of bgfx_frame(false) from jce_renderer_end_frame.
 * Non-blocking: kicks the side thread if the previous frame is done.
 * Detects hang when the side thread hasn't returned within
 * ANDROID_EGL_HANG_TIMEOUT_MS and permanently suspends rendering. */
static void android_end_frame(void)
{
    if (SDL_GetAtomicInt(&s_egl_hung)) return;   /* EGL already hung, skip */

    if (SDL_GetAtomicInt(&s_frame_done)) {
        /* Previous frame completed — kick a new one. */
        SDL_SetAtomicInt(&s_frame_kick_ms, (int)(SDL_GetTicks() & 0x7fffffff));
        SDL_SetAtomicInt(&s_frame_done, 0);
        jce_semaphore_signal(s_frame_req);
    } else {
        /* Still in-progress: check for hang. */
        uint32_t now_ms  = (uint32_t)SDL_GetTicks();
        uint32_t kick_ms = (uint32_t)SDL_GetAtomicInt(&s_frame_kick_ms);
        if (now_ms - kick_ms > ANDROID_EGL_HANG_TIMEOUT_MS) {
            SDL_SetAtomicInt(&s_egl_hung, 1);
            LOG_WARN(LOG_TAG,
                "eglSwapBuffers hung (WSA/libEGL_emulation bug) — "
                "rendering suspended, app stays alive");
        }
    }
    /* Do not wait — SDL game thread must stay responsive. */
}

bool jce_renderer_is_egl_hung(void)
{
    return SDL_GetAtomicInt(&s_egl_hung) != 0;
}
#else  /* !JCE_PLATFORM_ANDROID — stub: EGL hang detection is Android-only */
bool jce_renderer_is_egl_hung(void) { return false; }
#endif /* JCE_PLATFORM_ANDROID */
#if JCE_PLATFORM_WINDOWS
/* Backend probe uses jce_library_exists (SDL_LoadObject) for vulkan/d3d
 * library presence — keeps <windows.h> and <dlfcn.h> out of engine sources. */
#else
#include <setjmp.h>
#include <signal.h>
#if JCE_PLATFORM_ANDROID
#include <sys/system_properties.h>
#endif
#endif

struct JceRenderer {
    bool is_fallback;
    SDL_Renderer *sdl_renderer;

    bgfx_program_handle_t program;          /* color (pos+color) */
    bgfx_vertex_layout_t layout;            /* color vertex layout */
    bgfx_program_handle_t program_textured; /* textured (pos+color+uv) */
    bgfx_vertex_layout_t layout_textured;   /* textured vertex layout */
    bgfx_uniform_handle_t u_tex_color;      /* sampler uniform for textures */
    bgfx_program_handle_t program_mesh;     /* mesh (pos+normal+uv) */
    /* PBR programs */
    bgfx_program_handle_t program_pbr;
    bgfx_program_handle_t program_pbr_inst;     /* GPU-instanced PBR */
    bgfx_program_handle_t program_pbr_skinned;
    /* Forward+ clustered fragment variants (fs_pbr_fwdplus). */
    bgfx_program_handle_t program_pbr_fwdplus;
    bgfx_program_handle_t program_pbr_inst_fwdplus;
    bgfx_program_handle_t program_pbr_skinned_fwdplus;
    /* When true, jce_renderer_get_program_pbr* return the fwdplus variant
     * (if it loaded).  Set per-frame by the scene renderer from the
     * r.forwardplus cvar; default false => unchanged non-variant programs. */
    bool                  forwardplus_program_active;
    bgfx_program_handle_t program_shadow;
    bgfx_program_handle_t program_shadow_inst;     /* GPU-instanced shadow */
    bgfx_program_handle_t program_shadow_skinned;
    bgfx_program_handle_t program_terrain;
    bgfx_uniform_handle_t u_light_dir;   /* vec4: xyz = light direction */
    bgfx_uniform_handle_t u_light_color; /* vec4: xyz = color, w = ambient */
    uint32_t reset_flags;
    uint32_t debug_flags;
    char gpu_name[128];
};

static bool s_dbg_text_enabled = false;

/* ── Per-backend availability probe ──────────────────────────────── *
 *                                                                    *
 * Called before bgfx_init() to verify a backend is actually usable. *
 * Runs entirely before bgfx allocates any state, so a crash in the  *
 * probe is caught here (main thread, clean stack) and the fallback  *
 * loop can safely continue to the next backend.                     *
 * ─────────────────────────────────────────────────────────────────*/
#if JCE_PLATFORM_WINDOWS

static bool backend_probe(bgfx_renderer_type_t type)
{
    switch (type) {
    case BGFX_RENDERER_TYPE_VULKAN:
        /* Loader must export its entry point, not merely map — a stub
         * vulkan-1.dll without a real ICD behind it is reported unusable. */
        return jce_library_has_symbol("vulkan-1.dll", "vkGetInstanceProcAddr");
    case BGFX_RENDERER_TYPE_DIRECT3D12:
        return jce_library_exists("d3d12.dll");
    case BGFX_RENDERER_TYPE_DIRECT3D11:
        return jce_library_exists("d3d11.dll");
    default:
        return true;
    }
}

#else /* POSIX: Linux, macOS, Android, … */

static sigjmp_buf s_probe_jmp;

static void s_probe_sigsegv(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)info; (void)ctx;
    siglongjmp(s_probe_jmp, 1);
}

static bool s_probe_vulkan(void)
{
#if JCE_PLATFORM_ANDROID
    /* Houdini ARM64→x86_64 translation layer (WSA and some Intel Android
     * devices) initialises libvulkan.so successfully but crashes inside
     * bgfx's render thread at RendererContextVK::init with SEGV_MAPERR
     * because certain function pointers returned by vkGetDeviceProcAddr
     * are NULL.  That crash is in a thread we cannot intercept with
     * sigsetjmp, so we must detect and skip Vulkan before bgfx creates
     * any threads.  The reliable indicator is ro.dalvik.vm.isa.arm64 = "x86_64". */
    {
        char isa[PROP_VALUE_MAX];
        if (__system_property_get("ro.dalvik.vm.isa.arm64", isa) > 0
                && isa[0] == 'x' /* "x86_64" */) {
            LOG_INFO(LOG_TAG,
                "Vulkan probe: Houdini ARM64->x86_64 detected"
                " -- skipping Vulkan (WSA)");
            return false;
        }
    }
#endif

    /* Probe the Vulkan loader via the os/platform library wrapper
     * (SDL_LoadObject/SDL_LoadFunction under the hood — no raw dlopen/dlsym).
     * We require the loader to export vkGetInstanceProcAddr, not merely map,
     * so a stub/forwarder library is skipped to the next backend.  We keep
     * the SIGSEGV-trap scaffold around the load so a crash inside a broken
     * loader (e.g. WSA/Houdini on Android) is caught here on the clean
     * main-thread stack rather than later in bgfx's render thread. */
#if JCE_PLATFORM_APPLE
    static const char *const kVkLibs[] = {
        "libMoltenVK.dylib", "libvulkan.1.dylib", "@rpath/libvulkan.1.dylib", NULL
    };
#else
    static const char *const kVkLibs[] = { "libvulkan.so.1", "libvulkan.so", NULL };
#endif

    bool ok = false;
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = s_probe_sigsegv;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_sa);

    if (sigsetjmp(s_probe_jmp, 1) == 0) {
        for (size_t i = 0; kVkLibs[i]; ++i) {
            if (jce_library_has_symbol(kVkLibs[i], "vkGetInstanceProcAddr")) { ok = true; break; }
        }
    } else {
        LOG_WARN(LOG_TAG,
            "Vulkan probe: Vulkan loader crashed on load "
            "— driver not usable on this device");
        ok = false;
    }

    sigaction(SIGSEGV, &old_sa, NULL);

    if (ok) LOG_INFO(LOG_TAG, "Vulkan probe: OK");
    else    LOG_INFO(LOG_TAG, "Vulkan probe: library not found, skipping");
    return ok;
}

static bool backend_probe(bgfx_renderer_type_t type)
{
    if (type == BGFX_RENDERER_TYPE_VULKAN)
        return s_probe_vulkan();
    return true;
}

#endif /* JCE_PLATFORM_WINDOWS / POSIX */

static void jce_bgfx_fatal(bgfx_callback_interface_t *_this, const char *_filePath, uint16_t _line,
                           bgfx_fatal_t _code, const char *_str)
{
    (void)_this;
    LOG_ERROR(LOG_TAG, "bgfx fatal: code=%d file=%s line=%u msg=%s", (int)_code,
              _filePath ? _filePath : "<null>", (unsigned)_line, _str ? _str : "<null>");
}

/* When JCE_GFX_DEBUG=1, mirror bgfx internal traces (including the
 * D3D12 HRESULT printed right before "Failed to create PSO!") to our
 * log.  Otherwise stays silent to avoid spamming. */
static int s_bgfx_trace_enabled = -1;

static void jce_bgfx_trace_vargs(bgfx_callback_interface_t *_this, const char *_filePath,
                                 uint16_t _line, const char *_format, va_list _argList)
{
    (void)_this;
    if (s_bgfx_trace_enabled < 0) {
        const char *v = getenv("JCE_GFX_DEBUG");
        s_bgfx_trace_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (!s_bgfx_trace_enabled || !_format) return;

    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), _format, _argList);
    if (n < 0) return;
    /* Strip trailing newline that bgfx tends to append. */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    if (n == 0) return;

    const char *file = _filePath ? _filePath : "<bgfx>";
    /* Keep only the basename for compactness. */
    const char *slash = strrchr(file, '/');
    const char *back  = strrchr(file, '\\');
    if (back && back > slash) slash = back;
    if (slash) file = slash + 1;

    LOG_INFO("bgfx", "%s:%u %s", file, (unsigned)_line, buf);
}

static void jce_bgfx_profiler_begin(bgfx_callback_interface_t *_this, const char *_name,
                                    uint32_t _abgr, const char *_filePath, uint16_t _line)
{
    (void)_this;
    (void)_name;
    (void)_abgr;
    (void)_filePath;
    (void)_line;
}

static void jce_bgfx_profiler_begin_literal(bgfx_callback_interface_t *_this, const char *_name,
                                            uint32_t _abgr, const char *_filePath, uint16_t _line)
{
    (void)_this;
    (void)_name;
    (void)_abgr;
    (void)_filePath;
    (void)_line;
}

static void jce_bgfx_profiler_end(bgfx_callback_interface_t *_this)
{
    (void)_this;
}

static uint32_t jce_bgfx_cache_read_size(bgfx_callback_interface_t *_this, uint64_t _id)
{
    (void)_this;
    (void)_id;
    return 0;
}

static bool jce_bgfx_cache_read(bgfx_callback_interface_t *_this, uint64_t _id, void *_data,
                                uint32_t _size)
{
    (void)_this;
    (void)_id;
    (void)_data;
    (void)_size;
    return false;
}

static void jce_bgfx_cache_write(bgfx_callback_interface_t *_this, uint64_t _id, const void *_data,
                                 uint32_t _size)
{
    (void)_this;
    (void)_id;
    (void)_data;
    (void)_size;
}

/* Set while a bgfx_request_screen_shot() is in flight; cleared by the
   screen_shot callback once the file is written (or fails). */
static bool s_screenshot_pending = false;

/* Continuous capture (video recording). BGFX_RESET_CAPTURE does not deliver
   capture callbacks in this bgfx configuration, so recording instead drives
   the proven bgfx_request_screen_shot path: while active, end_frame requests a
   backbuffer shot each frame using the sentinel path below, and the screen_shot
   callback routes those pixels to the capture sink instead of writing a file. */
#define JCE_CAPTURE_SENTINEL "\x01__jce_capture__"
static struct {
    JceCaptureBeginFn begin;
    JceCaptureFrameFn frame;
    JceCaptureEndFn   end;
    void             *ud;
} s_capture_sink;
static bool s_capture_active       = false;
static bool s_capture_shot_pending = false;

static void jce_bgfx_screen_shot(bgfx_callback_interface_t *_this, const char *_filePath,
                                 uint32_t _width, uint32_t _height, uint32_t _pitch,
                                 const void *_data, uint32_t _size, bool _yflip)
{
    (void)_this;

    /* Recording frame: route pixels to the capture sink, write no file. */
    if (_filePath && strcmp(_filePath, JCE_CAPTURE_SENTINEL) == 0) {
        if (s_capture_active && _data && _width && _height) {
            if (s_capture_sink.begin)
                s_capture_sink.begin(s_capture_sink.ud, _width, _height, _pitch,
                                     _yflip ? 1 : 0);
            if (s_capture_sink.frame)
                s_capture_sink.frame(s_capture_sink.ud, _data, _size);
        }
        s_capture_shot_pending = false;
        return;
    }

    (void)_size;

    bool ok = false;
    if (_data && _filePath && _width && _height) {
        /* bgfx delivers the backbuffer as BGRA8.  Wrap it (respecting the
           row pitch), drop the undefined backbuffer alpha by converting to
           RGB24, flip when the backend reports bottom-up data, then encode by
           file extension (.png default, .bmp optional). */
        SDL_Surface *src = SDL_CreateSurfaceFrom((int)_width, (int)_height,
            SDL_PIXELFORMAT_BGRA32, (void *)(uintptr_t)_data, (int)_pitch);
        if (src) {
            SDL_Surface *rgb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGB24);
            SDL_DestroySurface(src);
            if (rgb) {
                if (_yflip)
                    SDL_FlipSurface(rgb, SDL_FLIP_VERTICAL);
                const char *ext = strrchr(_filePath, '.');
                if (ext && SDL_strcasecmp(ext, ".bmp") == 0)
                    ok = SDL_SaveBMP(rgb, _filePath);
                else
                    ok = IMG_SavePNG(rgb, _filePath);
                SDL_DestroySurface(rgb);
            }
        }
    }

    if (ok)
        LOG_SUCCESS(LOG_TAG, "screenshot saved: %s (%ux%u)", _filePath, _width, _height);
    else
        LOG_ERROR(LOG_TAG, "screenshot failed: %s (%s)",
                  _filePath ? _filePath : "(null)", SDL_GetError());

    s_screenshot_pending = false;
}

/* Request an async capture of the current frame's backbuffer to `path`.
   The shot is taken at the next bgfx_frame() and written from the screen_shot
   callback above.  Output format is chosen by `path`'s extension (.png by
   default).  Returns false if a capture is already pending or `path` is bad. */
bool jce_renderer_request_screenshot(const char *path)
{
    if (!path || !path[0])
        return false;
    if (s_screenshot_pending)
        return false;
    bgfx_frame_buffer_handle_t backbuffer = { UINT16_MAX }; /* invalid == backbuffer */
    s_screenshot_pending = true;
    bgfx_request_screen_shot(backbuffer, path);
    return true;
}

bool jce_renderer_screenshot_pending(void)
{
    return s_screenshot_pending;
}

/* Register the capture sink (s_capture_sink is defined near the screenshot
   callback, which feeds it). bgfx's BGFX_RESET_CAPTURE hooks below also forward
   to it, but are inert in this config — the screenshot path drives recording. */
void jce_renderer_set_capture_sink(JceCaptureBeginFn begin, JceCaptureFrameFn frame,
                                   JceCaptureEndFn end, void *ud)
{
    s_capture_sink.begin = begin;
    s_capture_sink.frame = frame;
    s_capture_sink.end   = end;
    s_capture_sink.ud    = ud;
}

static void jce_bgfx_capture_begin(bgfx_callback_interface_t *_this, uint32_t _width,
                                   uint32_t _height, uint32_t _pitch, bgfx_texture_format_t _format,
                                   bool _yflip)
{
    (void)_this;
    (void)_format;
    if (s_capture_sink.begin)
        s_capture_sink.begin(s_capture_sink.ud, _width, _height, _pitch, _yflip ? 1 : 0);
}

static void jce_bgfx_capture_end(bgfx_callback_interface_t *_this)
{
    (void)_this;
    if (s_capture_sink.end)
        s_capture_sink.end(s_capture_sink.ud);
}

static void jce_bgfx_capture_frame(bgfx_callback_interface_t *_this, const void *_data,
                                   uint32_t _size)
{
    (void)_this;
    if (s_capture_sink.frame)
        s_capture_sink.frame(s_capture_sink.ud, _data, _size);
}

static const bgfx_callback_vtbl_t s_bgfx_callback_vtbl = {
    jce_bgfx_fatal,          jce_bgfx_trace_vargs,
    jce_bgfx_profiler_begin, jce_bgfx_profiler_begin_literal,
    jce_bgfx_profiler_end,   jce_bgfx_cache_read_size,
    jce_bgfx_cache_read,     jce_bgfx_cache_write,
    jce_bgfx_screen_shot,    jce_bgfx_capture_begin,
    jce_bgfx_capture_end,    jce_bgfx_capture_frame,
};

static bgfx_callback_interface_t s_bgfx_callback = {
    &s_bgfx_callback_vtbl,
};

/* Map JceRendererBackend enum value to bgfx renderer type. */
static bgfx_renderer_type_t to_bgfx_type(enum JceRendererBackend b)
{
    switch (b) {
    case JCE_BACKEND_D3D11:    return BGFX_RENDERER_TYPE_DIRECT3D11;
    case JCE_BACKEND_D3D12:    return BGFX_RENDERER_TYPE_DIRECT3D12;
    case JCE_BACKEND_VULKAN:   return BGFX_RENDERER_TYPE_VULKAN;
    case JCE_BACKEND_OPENGL:   return BGFX_RENDERER_TYPE_OPENGL;
    case JCE_BACKEND_OPENGLES: return BGFX_RENDERER_TYPE_OPENGLES;
    case JCE_BACKEND_METAL:    return BGFX_RENDERER_TYPE_METAL;
    case JCE_BACKEND_NOOP:     return BGFX_RENDERER_TYPE_NOOP;
    default:                   return BGFX_RENDERER_TYPE_COUNT; /* auto */
    }
}

/* Map legacy integer backend (config field) to bgfx renderer type.
   Kept compatible with persisted JceConfig.backend values 0..6. */
static bgfx_renderer_type_t map_backend(int backend)
{
    return to_bgfx_type((enum JceRendererBackend)backend);
}

/* Platform-preferred bgfx fallback chain.
   Single source of truth lives in jce_renderer_caps_preferred_chain();
   we just translate JceRendererBackend → bgfx_renderer_type_t and append
   the BGFX_RENDERER_TYPE_COUNT sentinel.
   Stays automatically aligned with the preferences UI dropdown and with
   JCE_SHADER_PROFILES (we never list a backend whose .bin shaders weren't
   built — see CMakeLists.txt). */
static const bgfx_renderer_type_t *get_platform_fallback_chain(void)
{
    enum  { CAP = 8 };
    static bgfx_renderer_type_t chain[CAP + 1];
    static bool                 initialised = false;
    if (!initialised) {
        enum JceRendererBackend pref[CAP];
        int n = jce_renderer_caps_preferred_chain(pref, CAP);
        if (n > CAP) n = CAP;
        int o = 0;
        for (int i = 0; i < n; ++i) {
            bgfx_renderer_type_t t = to_bgfx_type(pref[i]);
            if (t != BGFX_RENDERER_TYPE_COUNT) chain[o++] = t;
        }
        chain[o] = BGFX_RENDERER_TYPE_COUNT;  /* sentinel */
        initialised = true;
    }
    return chain;
}

/* -- Lifecycle ------------------------------------------------------ */

JceRenderer *jce_renderer_create(JceWindow *win,
                                  const JceRendererConfig *cfg)
{
    if (!win || !cfg) return NULL;

    /* Test/diagnostic hook: JCE_FORCE_FALLBACK=1 short-circuits the
     * entire bgfx init path so the engine drops straight into the
     * SDL software fallback (the blue/orange info-panel UI in
     * jce_renderer_render_fallback_frame()).  Use this in caged_kingdom
     * to exercise the fallback live without needing a broken GPU:
     *     PowerShell:  $env:JCE_FORCE_FALLBACK=1; .\caged_kingdom.exe
     *     bash:        JCE_FORCE_FALLBACK=1 ./caged_kingdom
     * Any non-empty value other than "0" enables it. */
    {
        const char *force = getenv("JCE_FORCE_FALLBACK");
        if (force && force[0] && force[0] != '0') {
            LOG_WARN(LOG_TAG,
                "JCE_FORCE_FALLBACK=%s set — skipping bgfx init, "
                "engine will use SDL software renderer", force);
            return NULL;
        }
    }

    /* Retrieve native window handle.
     * On iOS the native handle may become available slightly after window
     * creation, so retry briefly before giving up. */
    JceNativeWindow nw;
    memset(&nw, 0, sizeof(nw));
    for (int i = 0; i < 120; i++) {
        jce_window_get_native(win, &nw);
        if (nw.nwh) break;
        SDL_PumpEvents();
        jce_thread_sleep_ms(16);
    }

    if (!nw.nwh) {
        LOG_ERROR(LOG_TAG, "native window handle is NULL");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "JCE",
            "Native window not ready", NULL);
        return NULL;
    }

    bgfx_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.nwh = nw.nwh;
    pd.ndt = nw.ndt;
    bgfx_set_platform_data(&pd);

    /* Initialise bgfx with graceful fallback. */
    uint32_t w, h;
    jce_window_get_size(win, &w, &h);

    uint32_t reset_flags = cfg->vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;

    /* JCE_GFX_DEBUG=1 → enable bgfx debug device (forwards to the
       D3D12 debug layer / Vulkan validation) so PSO compile failures
       print the underlying HRESULT / validation message. */
    bool gfx_debug = false;
    {
        const char *v = getenv("JCE_GFX_DEBUG");
        gfx_debug = (v && v[0] && v[0] != '0');
    }
    if (gfx_debug)
        LOG_INFO(LOG_TAG, "JCE_GFX_DEBUG enabled (D3D12/Vulkan validation on)");

    /* JCE_BACKEND=auto|d3d11|d3d12|vulkan|opengl|gles|metal|noop — overrides
       the caller-selected backend (same diagnostic env family as
       JCE_FORCE_FALLBACK above). Primary consumer: tools/render_parity.py,
       which boots the SAME binary on several backends and pixel-compares
       the output to catch backend-divergent shader/render behavior (the
       class of bug where a raw mat3 ctor flipped TBN on GLSL only) without
       touching any per-user config. */
    int backend_choice = cfg->backend;
    {
        const char *bv = getenv("JCE_BACKEND");
        if (bv && bv[0]) {
            char   low[16];
            size_t bi;
            for (bi = 0; bi + 1 < sizeof(low) && bv[bi]; bi++)
                low[bi] = (char)((bv[bi] >= 'A' && bv[bi] <= 'Z')
                                 ? bv[bi] + ('a' - 'A') : bv[bi]);
            low[bi] = '\0';
            if      (strcmp(low, "auto")   == 0) backend_choice = JCE_BACKEND_AUTO;
            else if (strcmp(low, "d3d11")  == 0) backend_choice = JCE_BACKEND_D3D11;
            else if (strcmp(low, "d3d12")  == 0) backend_choice = JCE_BACKEND_D3D12;
            else if (strcmp(low, "vulkan") == 0) backend_choice = JCE_BACKEND_VULKAN;
            else if (strcmp(low, "opengl") == 0 || strcmp(low, "gl") == 0)
                backend_choice = JCE_BACKEND_OPENGL;
            else if (strcmp(low, "gles") == 0 || strcmp(low, "opengles") == 0)
                backend_choice = JCE_BACKEND_OPENGLES;
            else if (strcmp(low, "metal")  == 0) backend_choice = JCE_BACKEND_METAL;
            else if (strcmp(low, "noop")   == 0) backend_choice = JCE_BACKEND_NOOP;
            else
                LOG_WARN(LOG_TAG, "JCE_BACKEND=%s not recognized — ignored", bv);
            if (backend_choice != cfg->backend)
                LOG_WARN(LOG_TAG,
                    "DEBUG TOGGLE: JCE_BACKEND=%s -> backend override", bv);
        }
    }

    bgfx_renderer_type_t requested_type = map_backend(backend_choice);
    const char *backend_name =
        requested_type == BGFX_RENDERER_TYPE_COUNT
            ? "auto"
            : bgfx_get_renderer_name(requested_type);
    LOG_INFO(LOG_TAG,
        "init request: backend=%s nwh=%p ndt=%p size=%ux%u",
        backend_name, pd.nwh, pd.ndt, w, h);

    /* On macOS (not iOS), bgfx's default multi-threaded mode causes a deadlock:
     * the render thread needs to call back to the main thread (via GCD)
     * to set up CAMetalLayer, but the main thread is blocked in bgfx_init()
     * waiting for the render thread.  Calling bgfx_render_frame(-1) before
     * bgfx_init() switches bgfx to single-threaded mode, avoiding the deadlock.
     *
     * iOS does NOT have this problem — UIKit's run-loop allows bgfx's render
     * thread to initialise Metal without deadlocking, so we leave bgfx in its
     * default multi-threaded mode on iOS. */
#if JCE_PLATFORM_MACOS
    bgfx_render_frame(-1);
#endif

    /* Try to initialise bgfx.  When the user picked a specific backend we
     * attempt that first; on failure (or AUTO) we walk the platform-specific
     * preferred list until one succeeds. */
    bgfx_init_t init;
    bool ok = false;

    if (requested_type != BGFX_RENDERER_TYPE_COUNT) {
        if (!backend_probe(requested_type)) {
            LOG_WARN(LOG_TAG, "requested backend %s probe failed — trying fallback chain",
                     bgfx_get_renderer_name(requested_type));
        } else {
            bgfx_init_ctor(&init);
            init.type              = requested_type;
            init.resolution.width  = w;
            init.resolution.height = h;
            init.resolution.reset  = reset_flags;
            init.platformData      = pd;
            init.callback          = &s_bgfx_callback;
            init.debug             = gfx_debug;
            ok = bgfx_init(&init);
            if (!ok)
                LOG_WARN(LOG_TAG, "requested backend %s failed",
                         bgfx_get_renderer_name(requested_type));
        }
    }

    if (!ok) {
        const bgfx_renderer_type_t *chain = get_platform_fallback_chain();
        for (int i = 0; chain[i] != BGFX_RENDERER_TYPE_COUNT; i++) {
            if (chain[i] == requested_type) continue; /* already tried */
            if (!backend_probe(chain[i])) {
                LOG_INFO(LOG_TAG, "skipping backend %s (probe failed)",
                         bgfx_get_renderer_name(chain[i]));
                continue;
            }
            LOG_INFO(LOG_TAG, "trying backend: %s",
                     bgfx_get_renderer_name(chain[i]));
            bgfx_init_ctor(&init);
            init.type              = chain[i];
            init.resolution.width  = w;
            init.resolution.height = h;
            init.resolution.reset  = reset_flags;
            init.platformData      = pd;
            init.callback          = &s_bgfx_callback;
            init.debug             = gfx_debug;
            if (bgfx_init(&init)) { ok = true; break; }
        }
    }

    if (!ok) {
        LOG_ERROR(LOG_TAG, "bgfx_init failed - all backends exhausted "
                  "(nwh=%p, w=%u, h=%u)", pd.nwh, w, h);
        /* 
         * We do NOT show a messagebox here, as the engine will handle
         * falling back to the SDL renderer or show an error screen later.
         */
        return NULL;
    }

    LOG_INFO(LOG_TAG, "renderer: %s",
             bgfx_get_renderer_name(bgfx_get_renderer_type()));

    bool enable_debug_text = cfg->debug_text;
    if (bgfx_get_renderer_type() == BGFX_RENDERER_TYPE_OPENGL) {
        if (enable_debug_text) {
            LOG_INFO(LOG_TAG, "disabling bgfx debug text on OpenGL backend");
        }
        enable_debug_text = false;
    }

    uint32_t debug_flags = enable_debug_text ? BGFX_DEBUG_TEXT : 0;
#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)
    debug_flags |= BGFX_DEBUG_PROFILER;
    LOG_INFO(LOG_TAG, "bgfx GPU profiler enabled (Tracy mode)");
#endif
    if (debug_flags)
        bgfx_set_debug(debug_flags);

    /* View 0 (3D): clear color + depth. */
    bgfx_set_view_clear(JCE_VIEW_MAIN_3D,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
        cfg->clear_color, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_MAIN_3D, 0, 0, (uint16_t)w, (uint16_t)h);

    /* View 1 (UI): no clear  draws on top of 3D.
       Sequential mode = painter's algorithm (submission order).
       RmlUi already submits back-to-front; post-render primitives
       (polyline graph, etc.) appear on top of the UI panels. */
    bgfx_set_view_clear(JCE_VIEW_UI, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_UI, 0, 0, (uint16_t)w, (uint16_t)h);
    bgfx_set_view_mode(JCE_VIEW_UI, BGFX_VIEW_MODE_SEQUENTIAL);

    /* View 2 (debug): no clear  debug text overlay. */
    bgfx_set_view_clear(JCE_VIEW_DEBUG, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_DEBUG, 0, 0, (uint16_t)w, (uint16_t)h);

    /* Vertex layout: Position (float3) + Color0 (UINT8x4, normalized). */
    JceRenderer *r = (JceRenderer *)JCE_CALLOC(1, sizeof(*r));
    if (!r) {
        bgfx_shutdown();
        return NULL;
    }
    r->reset_flags = reset_flags;
    r->debug_flags = debug_flags;
    s_dbg_text_enabled = enable_debug_text;

    /* Color vertex layout: pos(float3) + color(uint8x4). */
    bgfx_vertex_layout_begin(&r->layout,
        bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&r->layout,
        BGFX_ATTRIB_POSITION, 3,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout,
        BGFX_ATTRIB_COLOR0, 4,
        BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&r->layout);

    /* Textured vertex layout: pos + color + uv. */
    bgfx_vertex_layout_begin(&r->layout_textured,
        bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&r->layout_textured,
        BGFX_ATTRIB_POSITION, 3,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout_textured,
        BGFX_ATTRIB_COLOR0, 4,
        BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_add(&r->layout_textured,
        BGFX_ATTRIB_TEXCOORD0, 2,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&r->layout_textured);

    /* Uniforms (created here; shaders attached later). */
    r->u_tex_color = bgfx_create_uniform(
        "s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    r->u_light_dir = bgfx_create_uniform(
        "u_lightDir", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_color = bgfx_create_uniform(
        "u_lightColor", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Shader programs default to invalid; call
       jce_renderer_set_shaders() after creation. */
    r->program.idx              = UINT16_MAX;
    r->program_textured.idx     = UINT16_MAX;
    r->program_mesh.idx         = UINT16_MAX;
    r->program_pbr.idx          = UINT16_MAX;
    r->program_pbr_inst.idx     = UINT16_MAX;
    r->program_pbr_skinned.idx  = UINT16_MAX;
    r->program_pbr_fwdplus.idx         = UINT16_MAX;
    r->program_pbr_inst_fwdplus.idx    = UINT16_MAX;
    r->program_pbr_skinned_fwdplus.idx = UINT16_MAX;
    r->program_shadow.idx       = UINT16_MAX;
    r->program_shadow_inst.idx  = UINT16_MAX;
    r->program_shadow_skinned.idx = UINT16_MAX;
    r->program_terrain.idx        = UINT16_MAX;

    /* Build GPU name from vendor ID + renderer name. */
    {
        const bgfx_caps_t *caps = bgfx_get_caps();
        const char *vendor;
        switch (caps->vendorId) {
        case 0x1002: vendor = "AMD";     break;
        case 0x10DE: vendor = "NVIDIA";  break;
        case 0x8086: vendor = "Intel";   break;
        case 0x13B5: vendor = "ARM";     break;
        case 0x106B: vendor = "Apple";   break;
        default:     vendor = "Unknown"; break;
        }
        snprintf(r->gpu_name, sizeof(r->gpu_name), "%s / %s",
                 vendor, bgfx_get_renderer_name(bgfx_get_renderer_type()));
    }

    LOG_SUCCESS(LOG_TAG, "initialized (%s)", r->gpu_name);

    return r;
}

void jce_renderer_set_shaders(JceRenderer *r,
                              const JceShaderSet *shaders)
{
    if (!r || !shaders || r->is_fallback) return;

    r->program = (bgfx_program_handle_t){
        shaders->color.idx };
    r->program_textured = (bgfx_program_handle_t){
        shaders->textured.idx };
    r->program_mesh = (bgfx_program_handle_t){
        shaders->mesh.idx };
    r->program_pbr = (bgfx_program_handle_t){ shaders->pbr.idx };
    r->program_pbr_inst = (bgfx_program_handle_t){ shaders->pbr_inst.idx };
    r->program_pbr_skinned = (bgfx_program_handle_t){ shaders->pbr_skinned.idx };
    r->program_pbr_fwdplus = (bgfx_program_handle_t){ shaders->pbr_fwdplus.idx };
    r->program_pbr_inst_fwdplus = (bgfx_program_handle_t){ shaders->pbr_inst_fwdplus.idx };
    r->program_pbr_skinned_fwdplus = (bgfx_program_handle_t){ shaders->pbr_skinned_fwdplus.idx };
    r->program_shadow = (bgfx_program_handle_t){ shaders->shadow.idx };
    r->program_shadow_inst = (bgfx_program_handle_t){ shaders->shadow_inst.idx };
    r->program_shadow_skinned = (bgfx_program_handle_t){ shaders->shadow_skinned.idx };
    r->program_terrain = (bgfx_program_handle_t){ shaders->terrain.idx };

    if (r->program.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "color shader not provided");
}

bool jce_renderer_reload_shaders_fs(JceRenderer        *r,
                                    const char         *dev_dir,
                                    const JcePakArchive *pak)
{
    if (!r || r->is_fallback || !pak) return false;

    /* Snapshot old program handles so we can destroy them after the
       new set is installed.  bgfx defers destruction to end-of-frame
       which keeps any in-flight draws safe. */
    bgfx_program_handle_t old[] = {
        r->program, r->program_textured, r->program_mesh,
        r->program_pbr, r->program_pbr_inst, r->program_pbr_skinned,
        r->program_pbr_fwdplus, r->program_pbr_inst_fwdplus,
        r->program_pbr_skinned_fwdplus,
        r->program_shadow, r->program_shadow_inst, r->program_shadow_skinned,
        r->program_terrain,
    };

    JceShaderSet ns = jce_shaders_load_all_fs(dev_dir, pak);
    if (!jce_shader_valid(ns.color)) {
        LOG_ERROR(LOG_TAG, "reload: color shader load failed, aborting swap");
        /* Destroy any partially-loaded handles to avoid leaking. */
        bgfx_program_handle_t parts[] = {
            { ns.color.idx }, { ns.textured.idx }, { ns.mesh.idx },
            { ns.pbr.idx }, { ns.pbr_inst.idx }, { ns.pbr_skinned.idx },
            { ns.pbr_fwdplus.idx }, { ns.pbr_inst_fwdplus.idx },
            { ns.pbr_skinned_fwdplus.idx },
            { ns.shadow.idx }, { ns.shadow_inst.idx }, { ns.shadow_skinned.idx },
            { ns.terrain.idx },
        };
        for (size_t i = 0; i < sizeof(parts)/sizeof(parts[0]); i++) {
            if (parts[i].idx != UINT16_MAX)
                bgfx_destroy_program(parts[i]);
        }
        return false;
    }

    jce_renderer_set_shaders(r, &ns);

    for (size_t i = 0; i < sizeof(old)/sizeof(old[0]); i++) {
        if (old[i].idx != UINT16_MAX)
            bgfx_destroy_program(old[i]);
    }
    LOG_INFO(LOG_TAG, "shaders reloaded (dev_dir=%s)", dev_dir ? dev_dir : "(none)");
    return true;
}

JceRenderer *jce_renderer_create_fallback(JceWindow *win)
{
    if (!win) return NULL;
    JceRenderer *r = (JceRenderer *)JCE_CALLOC(1, sizeof(*r));
    if (!r) return NULL;

    r->is_fallback = true;

    SDL_Window *sdl_win = jce_window_sdl(win);

    /* This path is reached after the entire bgfx fallback chain
     * (D3D12 → Vulkan → D3D11 → OpenGL on Windows; Vulkan → GL on
     * Linux; Metal → Vulkan on macOS; Vulkan → GLES on Android) has
     * been exhausted, which means every GPU driver path on this
     * machine refused to initialise.  Per the engine policy
     * (performance → compatibility → safe software), we go straight
     * to SDL's pure-CPU software renderer here so the user always
     * sees the diagnostic UI (the blue/orange info panel painted by
     * jce_renderer_render_fallback_frame()) instead of risking yet
     * another hardware-path crash via SDL's HW-accelerated 2D
     * backends. */

    /* 1) Force the pure CPU software renderer — no GPU touched. */
    r->sdl_renderer = SDL_CreateRenderer(sdl_win, SDL_SOFTWARE_RENDERER);
    if (r->sdl_renderer) {
        snprintf(r->gpu_name, sizeof(r->gpu_name), "Fallback: software (CPU)");
        LOG_SUCCESS(LOG_TAG, "fallback initialized (software CPU renderer)");
        return r;
    }
    LOG_WARN(LOG_TAG,
        "SDL software renderer failed: %s — trying SDL auto as last resort",
        SDL_GetError());

    /* 2) Last-resort: let SDL pick anything it can (HW or SW).  Only
     * runs if the software renderer itself failed to create, which
     * normally indicates a deeper SDL/window issue. */
    r->sdl_renderer = SDL_CreateRenderer(sdl_win, NULL);
    if (r->sdl_renderer) {
        snprintf(r->gpu_name, sizeof(r->gpu_name), "Fallback: %s",
                 SDL_GetRendererName(r->sdl_renderer));
        LOG_SUCCESS(LOG_TAG, "fallback initialized (%s)", r->gpu_name);
        return r;
    }
    LOG_ERROR(LOG_TAG, "SDL auto renderer also failed: %s", SDL_GetError());

    JCE_FREE(r);
    return NULL;
}

bool jce_renderer_is_fallback(const JceRenderer *r)
{
    return r ? r->is_fallback : false;
}

/* -- Fallback frame: real 2D rendering via SDL_Renderer ------------- */

/* Draw a filled rounded-corner rectangle (approximated with rects). */
static void fb_draw_panel(SDL_Renderer *rd, float x, float y, float w, float h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(rd, r, g, b, a);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRect(rd, &rect);
}

/* Draw a 1px border rectangle. */
static void fb_draw_border(SDL_Renderer *rd, float x, float y, float w, float h,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(rd, r, g, b, a);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderRect(rd, &rect);
}

void jce_renderer_render_fallback_frame(const JceRenderer *r)
{
    if (!r || !r->is_fallback || !r->sdl_renderer) return;

    SDL_Renderer *rd = r->sdl_renderer;
    int ww = 0, wh = 0;
    SDL_GetRenderOutputSize(rd, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    float fw = (float)ww, fh = (float)wh;
    float scale = fw / 800.0f; /* base design at 800px wide */
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.5f) scale = 2.5f;

    /* -- Background gradient (approximated with horizontal bands) -- */
    for (int i = 0; i < wh; i++) {
        float t = (float)i / fh;
        uint8_t cr = (uint8_t)(20  + t * 15);
        uint8_t cg = (uint8_t)(22  + t * 18);
        uint8_t cb = (uint8_t)(35  + t * 25);
        SDL_SetRenderDrawColor(rd, cr, cg, cb, 255);
        SDL_FRect line = { 0, (float)i, fw, 1.0f };
        SDL_RenderFillRect(rd, &line);
    }

    /* -- Center panel ------------------------------------------------ */
    float panel_w = 460 * scale;
    float panel_h = 280 * scale;
    float px = (fw - panel_w) / 2.0f;
    float py = (fh - panel_h) / 2.0f;

    fb_draw_panel(rd, px, py, panel_w, panel_h, 30, 32, 45, 230);
    fb_draw_border(rd, px, py, panel_w, panel_h, 80, 180, 255, 200);

    /* -- Title bar --------------------------------------------------- */
    float bar_h = 36 * scale;
    fb_draw_panel(rd, px, py, panel_w, bar_h, 50, 130, 220, 255);

    /* -- Text via SDL_RenderDebugText (8x8 monospace, built-in) ------ */
    float text_scale = scale * 1.5f;
    SDL_SetRenderScale(rd, text_scale, text_scale);

    float tx = (px + 12 * scale) / text_scale;
    float ty = (py + 10 * scale) / text_scale;

    /* Title. */
    SDL_SetRenderDrawColor(rd, 255, 255, 255, 255);
    SDL_RenderDebugText(rd, tx, ty, "JCE - Software Renderer");

    /* Info lines below title bar. */
    float line_y = (py + bar_h + 16 * scale) / text_scale;
    float line_x = (px + 20 * scale) / text_scale;
    float line_h = 14.0f;

    SDL_SetRenderDrawColor(rd, 200, 200, 210, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "GPU acceleration unavailable.");
    line_y += line_h;
    SDL_RenderDebugText(rd, line_x, line_y, "Running in CPU software mode.");
    line_y += line_h * 1.8f;

    SDL_SetRenderDrawColor(rd, 140, 180, 220, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "Renderer:");
    SDL_SetRenderDrawColor(rd, 255, 220, 100, 255);
    SDL_RenderDebugText(rd, line_x + 80, line_y, r->gpu_name);
    line_y += line_h;

    SDL_SetRenderDrawColor(rd, 140, 180, 220, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "Platform:");
    SDL_SetRenderDrawColor(rd, 255, 220, 100, 255);
    SDL_RenderDebugText(rd, line_x + 80, line_y, SDL_GetPlatform());
    line_y += line_h * 1.8f;

    /* Uptime. */
    uint64_t ticks = jce_time_ticks_ms();
    unsigned secs = (unsigned)(ticks / 1000);
    unsigned mins = secs / 60;
    secs %= 60;
    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%u:%02u", mins, secs);

    SDL_SetRenderDrawColor(rd, 140, 180, 220, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "Uptime:");
    SDL_SetRenderDrawColor(rd, 180, 255, 180, 255);
    SDL_RenderDebugText(rd, line_x + 80, line_y, time_buf);
    line_y += line_h * 1.8f;

    /* Hint message. */
    SDL_SetRenderDrawColor(rd, 120, 120, 140, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "For full rendering, use a system");
    line_y += line_h;
    SDL_RenderDebugText(rd, line_x, line_y, "with GPU hardware acceleration.");

    /* Restore scale. */
    SDL_SetRenderScale(rd, 1.0f, 1.0f);

    /* -- Animated activity indicator (bottom of panel) --------------- */
    {
        float bar_x = px + 20 * scale;
        float bar_y = py + panel_h - 28 * scale;
        float bar_w = panel_w - 40 * scale;
        float bar_ht = 6 * scale;
        /* Ping-pong animation. */
        float t = (float)(ticks % 3000) / 3000.0f;
        float pos = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
        fb_draw_panel(rd, bar_x, bar_y, bar_w, bar_ht, 40, 40, 50, 255);
        float dot_w = bar_w * 0.25f;
        fb_draw_panel(rd, bar_x + pos * (bar_w - dot_w), bar_y,
                      dot_w, bar_ht, 80, 180, 255, 255);
    }

    SDL_RenderPresent(rd);
}

void jce_renderer_destroy(JceRenderer *r)
{
    if (!r) return;
    if (r->is_fallback) {
        if (r->sdl_renderer) {
            SDL_DestroyRenderer(r->sdl_renderer);
        }
        JCE_FREE(r);
        LOG_INFO(LOG_TAG, "renderer destroyed (fallback)");
        return;
    }
    s_dbg_text_enabled = false;
    if (r->program.idx != UINT16_MAX)
        bgfx_destroy_program(r->program);
    if (r->program_textured.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_textured);
    if (r->u_tex_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_tex_color);
    if (r->program_mesh.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_mesh);
    if (r->program_pbr.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr);
    if (r->program_pbr_inst.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr_inst);
    if (r->program_pbr_skinned.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr_skinned);
    if (r->program_pbr_fwdplus.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr_fwdplus);
    if (r->program_pbr_inst_fwdplus.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr_inst_fwdplus);
    if (r->program_pbr_skinned_fwdplus.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr_skinned_fwdplus);
    if (r->program_shadow.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_shadow);
    if (r->program_shadow_inst.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_shadow_inst);
    if (r->program_shadow_skinned.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_shadow_skinned);
    if (r->program_terrain.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_terrain);
    if (r->u_light_dir.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_dir);
    if (r->u_light_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_color);

    /* Tear down the text/FreeType subsystem here so any future GPU-touching
     * cleanup it grows runs while bgfx is still alive. Today FT_Done_FreeType
     * is bgfx-agnostic, but routing the call through renderer destroy
     * preserves the LIFO contract documented in jce_engine.c. */
    jce_text_shutdown();

    /* Free graph-generated custom programs cached by jce_pbr_material_load_json
     * while bgfx is still alive. */
    jce_pbr_material_shutdown();

    bgfx_shutdown();
    JCE_FREE(r);
    LOG_INFO(LOG_TAG, "renderer destroyed");
}

/* -- Per-frame ------------------------------------------------------ */

void jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win)
{
    JCE_PROFILE_ZONE_N("Renderer::BeginFrame");
    if (!r || !win) { JCE_PROFILE_ZONE_END; return; }
    if (r->is_fallback) { JCE_PROFILE_ZONE_END; return; }

    /* Full-backbuffer viewport for all views. */
    uint16_t vp_x, vp_y, vp_w, vp_h;
    jce_window_calc_viewport(win, &vp_x, &vp_y, &vp_w, &vp_h);

    const bgfx_caps_t *caps = bgfx_get_caps();

    /* View 0 (3D): identity view, ortho proj  overridden by begin_frame_3d. */
    {
        jce_mat4 view = jce_m4_identity();
        int lw, lh;
        jce_window_get_logical(win, &lw, &lh);
        jce_mat4 proj = jce_m4_ortho(0, (float)lw, (float)lh, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(JCE_VIEW_MAIN_3D, view.raw[0], proj.raw[0]);
        bgfx_set_view_rect(JCE_VIEW_MAIN_3D, vp_x, vp_y, vp_w, vp_h);
    }

    /* View 1 (UI): 2D orthographic in logical coordinates. */
    {
        jce_mat4 view = jce_m4_identity();
        uint32_t pw, ph;
        jce_window_get_size(win, &pw, &ph);
        jce_mat4 proj = jce_m4_ortho(0, (float)pw, (float)ph, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(JCE_VIEW_UI, view.raw[0], proj.raw[0]);
        bgfx_set_view_rect(JCE_VIEW_UI, vp_x, vp_y, vp_w, vp_h);
    }

    /* View 2 (debug): same as UI for debug text. */
    bgfx_set_view_rect(JCE_VIEW_DEBUG, vp_x, vp_y, vp_w, vp_h);

    if (s_dbg_text_enabled)
        bgfx_dbg_text_clear(0, false);
    bgfx_touch(JCE_VIEW_MAIN_3D);
    bgfx_touch(JCE_VIEW_UI);
    bgfx_touch(JCE_VIEW_DEBUG);
    bgfx_touch(JCE_VIEW_IMGUI);
    JCE_PROFILE_ZONE_END;
}

void jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
                                  const JceCamera *cam, uint16_t view_id)
{
    JCE_PROFILE_ZONE_N("Renderer::BeginFrame3D");
    if (!r || !win) { JCE_PROFILE_ZONE_END; return; }
    if (r->is_fallback) { JCE_PROFILE_ZONE_END; return; }

    uint16_t vp_x, vp_y, vp_w, vp_h;
    jce_window_calc_viewport(win, &vp_x, &vp_y, &vp_w, &vp_h);

    if (cam) {
        const bgfx_caps_t *caps = bgfx_get_caps();
        float aspect = (vp_h > 0) ? (float)vp_w / (float)vp_h : 1.0f;

        jce_mat4 view = jce_camera_view(cam);
        jce_mat4 proj = jce_camera_proj(cam, aspect, caps->homogeneousDepth);

        bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
    } else {
        /* Fallback: 2D ortho. */
        jce_mat4 view = jce_m4_identity();
        int lw, lh;
        jce_window_get_logical(win, &lw, &lh);
        const bgfx_caps_t *caps = bgfx_get_caps();
        jce_mat4 proj = jce_m4_ortho(0, (float)lw, (float)lh, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
    }

    bgfx_set_view_rect(view_id, vp_x, vp_y, vp_w, vp_h);
    /* Force sequential submission order for the 3D view so the scene
     * renderer's draw order (sky → shadows → opaques → transparents) is
     * preserved when rendering directly to the backbuffer.  The editor's
     * offscreen bridge already enables sequential mode, so this matches
     * that behaviour for runtime games that bypass the bridge. */
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);
    JCE_PROFILE_ZONE_END;
}

void jce_renderer_present_splash(const JceRenderer *r,
                                 JceWindow *win,
                                 uint32_t rgba_color)
{
    if (!r || r->is_fallback || !win) return;

    uint32_t w = 0, h = 0;
    jce_window_get_size(win, &w, &h);
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    bgfx_set_view_clear(0,
                        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        rgba_color, 1.0f, 0);
    bgfx_set_view_rect(0, 0, 0, (uint16_t)w, (uint16_t)h);
    bgfx_touch(0);
    s_bgfx_frame_index = bgfx_frame(false);
}

void jce_renderer_end_frame(const JceRenderer *r)
{
    JCE_PROFILE_ZONE_N("Renderer::EndFrame");
    if (!r || r->is_fallback) { JCE_PROFILE_ZONE_END; return; }

    /* ── GPU memory diagnostic (every 15 seconds) ─────────────────────
     * Logs bgfx GPU resource counts and memory usage. Use this to
     * confirm whether the editor's working-set growth lives on the
     * GPU side (textures / framebuffers leaking) or the CPU side
     * (CRT heap, mapped files). Disable by undef'ing the macro.
     *
     * Note: bgfx returns INT64_MAX (~9.2e18, displayed as ~-8.8e12 MB
     * after the >>20 shift on signed types) for memory counters when
     * the backend doesn't expose them (D3D11 typically doesn't). We
     * detect the sentinel and print "n/a" instead. */
#ifndef JCE_DISABLE_BGFX_STATS_LOG
    {
        static double s_last_log_s = 0.0;
        static uint16_t s_max_textures = 0;
        static uint16_t s_max_framebuffers = 0;
        const double now_s = (double)jce_time_ticks_ms() / 1000.0;
        if (now_s - s_last_log_s >= 15.0) {
            s_last_log_s = now_s;
            const bgfx_stats_t *st = bgfx_get_stats();
            if (st) {
                if (st->numTextures     > s_max_textures)     s_max_textures     = st->numTextures;
                if (st->numFrameBuffers > s_max_framebuffers) s_max_framebuffers = st->numFrameBuffers;
                char gpu_buf[64];
                if (st->gpuMemoryUsed < 0 || st->gpuMemoryMax < 0) {
                    snprintf(gpu_buf, sizeof(gpu_buf), "gpu=n/a (backend not reporting)");
                } else {
                    snprintf(gpu_buf, sizeof(gpu_buf), "gpu=%lld/%lld MB",
                             (long long)(st->gpuMemoryUsed >> 20),
                             (long long)(st->gpuMemoryMax  >> 20));
                }
                LOG_INFO(LOG_TAG,
                    "bgfx stats: %s tex=%u(peak %u) fb=%u(peak %u) "
                    "vb=%u ib=%u prog=%u shader=%u uniform=%u",
                    gpu_buf,
                    (unsigned)st->numTextures,     (unsigned)s_max_textures,
                    (unsigned)st->numFrameBuffers, (unsigned)s_max_framebuffers,
                    (unsigned)st->numVertexBuffers, (unsigned)st->numIndexBuffers,
                    (unsigned)st->numPrograms,      (unsigned)st->numShaders,
                    (unsigned)st->numUniforms);
            }
        }
    }
#endif

    /* JCE_CAPTURE_FRAME=N + JCE_CAPTURE_PATH=file.png — one-shot automated
       backbuffer capture once the bgfx frame index reaches N (env family of
       JCE_NO_PICK / JCE_BACKEND). Lets tools/render_parity.py and headless
       verification grab a deterministic frame without window focus, hotkeys
       or per-user config. Parsed once; fires exactly once per process. */
    {
        static int  s_cap_frame = -2;        /* -2 = unparsed, -1 = disabled */
        static char s_cap_path[512];
        static bool s_cap_done = false;
        if (s_cap_frame == -2) {
            const char *fv = getenv("JCE_CAPTURE_FRAME");
            const char *pv = getenv("JCE_CAPTURE_PATH");
            if (fv && fv[0] && pv && pv[0]) {
                s_cap_frame = atoi(fv);
                if (s_cap_frame < 0) s_cap_frame = 0;
                snprintf(s_cap_path, sizeof(s_cap_path), "%s", pv);
                LOG_WARN(LOG_TAG,
                    "DEBUG TOGGLE: JCE_CAPTURE_FRAME=%d -> %s",
                    s_cap_frame, s_cap_path);
            } else {
                s_cap_frame = -1;
            }
        }
        if (s_cap_frame >= 0 && !s_cap_done &&
            s_bgfx_frame_index >= (uint32_t)s_cap_frame) {
            if (jce_renderer_request_screenshot(s_cap_path))
                s_cap_done = true;
        }
    }

    /* Recording: request a backbuffer capture for this frame (one in flight;
       the screen_shot callback routes it to the capture sink). Reuses the
       proven screenshot path since BGFX_RESET_CAPTURE is inert here. */
    if (s_capture_active && !s_capture_shot_pending && !s_screenshot_pending) {
        bgfx_frame_buffer_handle_t bb = { UINT16_MAX };  /* backbuffer */
        s_capture_shot_pending = true;
        bgfx_request_screen_shot(bb, JCE_CAPTURE_SENTINEL);
    }

    s_bgfx_frame_index = bgfx_frame(false);

    /* Surface allocator + renderer stats to Tracy each frame. */
#if defined(JCE_PROFILER_ENABLED)
    {
        JceMemStats ms;
        if (jce_mem_stats(&ms)) {
            JCE_PROFILE_PLOT_I("mem.rss_mb",    (int64_t)(ms.current_rss / (1024 * 1024)));
            JCE_PROFILE_PLOT_I("mem.commit_mb", (int64_t)(ms.current_commit / (1024 * 1024)));
        }

        const bgfx_stats_t *st = bgfx_get_stats();
        if (st) {
            JCE_PROFILE_PLOT_I("render.draw_calls",  (int64_t)st->numDraw);
            JCE_PROFILE_PLOT_I("render.num_prims",   (int64_t)st->numPrims);
            JCE_PROFILE_PLOT_I("render.textures",    (int64_t)st->numTextures);
            JCE_PROFILE_PLOT_I("render.gpu_mem_mb",  (int64_t)(st->gpuMemoryUsed >> 20));
        }
    }
#endif

    JCE_PROFILE_ZONE_END;
}

/* -- Events --------------------------------------------------------- */

void jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h)
{
    if (!r) return;
    if (r->is_fallback) return;

    /* Sanitize dimensions before handing them to bgfx:
     *  - bgfx_reset(0, 0) leaves the backbuffer in an invalid state which
     *    typically manifests as a permanent black screen after restoring
     *    from a minimized window.
     *  - Some D3D11/D3D12 drivers reject backbuffer widths that are not a
     *    multiple of 4, falling back silently and leaving the previous
     *    swap chain — the user sees a frozen / flickering image while the
     *    window is dragged across DPI boundaries that produce odd pixel
     *    widths. Round the width up so reset always succeeds. */
    if (w < 1u) w = 1u;
    if (h < 1u) h = 1u;
    w = (w + 3u) & ~3u;

    bgfx_reset(w, h, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

void jce_renderer_rebind_platform(JceRenderer *r, JceWindow *win)
{
    if (!r || !win) return;
    if (r->is_fallback) return;

    JceNativeWindow nw;
    jce_window_get_native(win, &nw);
    if (!nw.nwh) {
        LOG_WARN(LOG_TAG, "rebind_platform: nwh is NULL, skipping");
        return;
    }

    bgfx_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.nwh = nw.nwh;
    pd.ndt = nw.ndt;
    bgfx_set_platform_data(&pd);

    uint32_t w, h;
    jce_window_get_size(win, &w, &h);
    bgfx_reset(w, h, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

/* -- Debug text ----------------------------------------------------- */

void jce_renderer_dbg_text(uint16_t x, uint16_t y,
                           uint8_t attr, const char *fmt, ...)
{
    /* bgfx handles global state so we don't need 'r' here.
     * Our callers in jce_engine already skip this when in fallback mode. */
    if (!s_dbg_text_enabled)
        return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bgfx_dbg_text_printf(x, y, attr, "%s", buf);
}

void jce_renderer_dbg_text_v(uint16_t x, uint16_t y,
                             uint8_t attr, const char *fmt, va_list ap)
{
    if (!s_dbg_text_enabled)
        return;

    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    bgfx_dbg_text_printf(x, y, attr, "%s", buf);
}

/* -- Accessors for primitives module -------------------------------- */

const bgfx_vertex_layout_t *jce_renderer_get_layout(const JceRenderer *r)
{
    return r ? &r->layout : NULL;
}

bgfx_program_handle_t jce_renderer_get_program(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program : invalid;
}

const bgfx_vertex_layout_t *jce_renderer_get_layout_textured(const JceRenderer *r)
{
    return r ? &r->layout_textured : NULL;
}

bgfx_program_handle_t jce_renderer_get_program_textured(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_textured : invalid;
}

JceUniformHandle jce_renderer_get_tex_uniform(const JceRenderer *r)
{
    JceUniformHandle invalid = JCE_INVALID_UNIFORM;
    if (!r) return invalid;
    return (JceUniformHandle){ r->u_tex_color.idx };
}

JceShaderHandle jce_renderer_get_program_color(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program.idx };
}

JceShaderHandle jce_renderer_get_program_mesh(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_mesh.idx };
}

JceShaderHandle jce_renderer_get_program_pbr(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    if (r->forwardplus_program_active &&
        r->program_pbr_fwdplus.idx != UINT16_MAX)
        return (JceShaderHandle){ r->program_pbr_fwdplus.idx };
    return (JceShaderHandle){ r->program_pbr.idx };
}

JceShaderHandle jce_renderer_create_program_from_blobs(
    const void *vs_blob, size_t vs_size,
    const void *fs_blob, size_t fs_size)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!vs_blob || vs_size == 0 || !fs_blob || fs_size == 0)
        return invalid;

    /* bgfx_copy: bgfx allocates internal memory and copies the bytes,
       so the caller's buffers may be freed immediately after this. */
    const bgfx_memory_t *vs_mem = bgfx_copy(vs_blob, (uint32_t)vs_size);
    const bgfx_memory_t *fs_mem = bgfx_copy(fs_blob, (uint32_t)fs_size);
    if (!vs_mem || !fs_mem) return invalid;

    bgfx_shader_handle_t vs = bgfx_create_shader(vs_mem);
    bgfx_shader_handle_t fs = bgfx_create_shader(fs_mem);
    if (vs.idx == UINT16_MAX || fs.idx == UINT16_MAX) {
        if (vs.idx != UINT16_MAX) bgfx_destroy_shader(vs);
        if (fs.idx != UINT16_MAX) bgfx_destroy_shader(fs);
        return invalid;
    }

    /* destroy_shaders=true: bgfx ref-counts the shaders to the program,
       and destroys them when the program is destroyed.  We never need
       to touch the vs/fs handles after this point. */
    bgfx_program_handle_t prog = bgfx_create_program(vs, fs, true);
    if (prog.idx == UINT16_MAX) {
        bgfx_destroy_shader(vs);
        bgfx_destroy_shader(fs);
        return invalid;
    }
    return (JceShaderHandle){ prog.idx };
}

void jce_renderer_destroy_program(JceShaderHandle prog)
{
    if (prog.idx == UINT16_MAX) return;
    bgfx_destroy_program((bgfx_program_handle_t){ prog.idx });
}

JceShaderHandle jce_renderer_get_program_pbr_inst(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    if (r->forwardplus_program_active &&
        r->program_pbr_inst_fwdplus.idx != UINT16_MAX)
        return (JceShaderHandle){ r->program_pbr_inst_fwdplus.idx };
    return (JceShaderHandle){ r->program_pbr_inst.idx };
}

JceShaderHandle jce_renderer_get_program_pbr_skinned(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    if (r->forwardplus_program_active &&
        r->program_pbr_skinned_fwdplus.idx != UINT16_MAX)
        return (JceShaderHandle){ r->program_pbr_skinned_fwdplus.idx };
    return (JceShaderHandle){ r->program_pbr_skinned.idx };
}

void jce_renderer_set_forwardplus_program_active(JceRenderer *r, bool active)
{
    if (r) r->forwardplus_program_active = active;
}

bool jce_renderer_get_forwardplus_program_active(const JceRenderer *r)
{
    return r ? r->forwardplus_program_active : false;
}

JceShaderHandle jce_renderer_get_program_pbr_fwdplus(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_pbr_fwdplus.idx };
}

JceShaderHandle jce_renderer_get_program_pbr_inst_fwdplus(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_pbr_inst_fwdplus.idx };
}

JceShaderHandle jce_renderer_get_program_pbr_skinned_fwdplus(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_pbr_skinned_fwdplus.idx };
}

JceShaderHandle jce_renderer_get_program_shadow(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_shadow.idx };
}

JceShaderHandle jce_renderer_get_program_shadow_inst(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_shadow_inst.idx };
}

JceShaderHandle jce_renderer_get_program_shadow_skinned(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_shadow_skinned.idx };
}

JceShaderHandle jce_renderer_get_program_terrain(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_terrain.idx };
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_terrain(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_terrain : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_pbr : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr_skinned(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_pbr_skinned : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_shadow : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow_skinned(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_shadow_skinned : invalid;
}

bgfx_uniform_handle_t jce_renderer_get_light_dir_uniform(const JceRenderer *r)
{
    bgfx_uniform_handle_t invalid = { UINT16_MAX };
    return r ? r->u_light_dir : invalid;
}

bgfx_uniform_handle_t jce_renderer_get_light_color_uniform(const JceRenderer *r)
{
    bgfx_uniform_handle_t invalid = { UINT16_MAX };
    return r ? r->u_light_color : invalid;
}

/* -- Queries (for debug HUD) --------------------------------------- */

const char *jce_renderer_get_backend_name(const JceRenderer *r)
{
    (void)r;
    return bgfx_get_renderer_name(bgfx_get_renderer_type());
}

JceRendererBackend jce_renderer_get_backend(const JceRenderer *r)
{
    (void)r;
    switch (bgfx_get_renderer_type()) {
    case BGFX_RENDERER_TYPE_DIRECT3D11: return JCE_BACKEND_D3D11;
    case BGFX_RENDERER_TYPE_DIRECT3D12: return JCE_BACKEND_D3D12;
    case BGFX_RENDERER_TYPE_VULKAN:     return JCE_BACKEND_VULKAN;
    case BGFX_RENDERER_TYPE_METAL:      return JCE_BACKEND_METAL;
    case BGFX_RENDERER_TYPE_OPENGL:     return JCE_BACKEND_OPENGL;
    case BGFX_RENDERER_TYPE_OPENGLES:   return JCE_BACKEND_OPENGLES;
    case BGFX_RENDERER_TYPE_NOOP:       return JCE_BACKEND_NOOP;
    default:                            return JCE_BACKEND_AUTO;
    }
}

const char *jce_renderer_get_gpu_name(const JceRenderer *r)
{
    return r ? r->gpu_name : "N/A";
}

bool jce_renderer_get_vsync(const JceRenderer *r)
{
    return r ? (r->reset_flags & BGFX_RESET_VSYNC) != 0 : false;
}

void jce_renderer_set_vsync_for_size(JceRenderer *r, bool enabled,
                                     uint32_t width, uint32_t height)
{
    if (!r || r->is_fallback) return;
    bool current = (r->reset_flags & BGFX_RESET_VSYNC) != 0;
    if (current == enabled) return;

    if (enabled)
        r->reset_flags |= BGFX_RESET_VSYNC;
    else
        r->reset_flags &= ~BGFX_RESET_VSYNC;

    if (width == 0 || height == 0) {
        const bgfx_stats_t *stats = bgfx_get_stats();
        width = stats->width;
        height = stats->height;
    }

    bgfx_reset(width, height, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

void jce_renderer_set_vsync(JceRenderer *r, bool enabled)
{
    const bgfx_stats_t *stats = bgfx_get_stats();
    jce_renderer_set_vsync_for_size(r, enabled, stats->width, stats->height);
}

/* Multisample anti-aliasing.  `samples` 0/1 = off, else snapped to 2/4/8/16.
 * Toggles the MSAA field of the swapchain reset flags + triggers a GPU reset —
 * the same mechanism as vsync, so a shipped game can honor the authored
 * Project Settings > Graphics MSAA level (applied from render_settings.json). */
void jce_renderer_set_msaa(JceRenderer *r, int samples)
{
    if (!r || r->is_fallback) return;
    uint32_t msaa = BGFX_RESET_NONE;
    if      (samples >= 16) msaa = BGFX_RESET_MSAA_X16;
    else if (samples >= 8)  msaa = BGFX_RESET_MSAA_X8;
    else if (samples >= 4)  msaa = BGFX_RESET_MSAA_X4;
    else if (samples >= 2)  msaa = BGFX_RESET_MSAA_X2;
    /* The MSAA level is a multi-bit field; clear it before OR-ing the new one. */
    const uint32_t msaa_mask = BGFX_RESET_MSAA_X2 | BGFX_RESET_MSAA_X4 |
                               BGFX_RESET_MSAA_X8 | BGFX_RESET_MSAA_X16;
    uint32_t want = (r->reset_flags & ~msaa_mask) | msaa;
    if (want == r->reset_flags) return;
    r->reset_flags = want;
    const bgfx_stats_t *stats = bgfx_get_stats();
    bgfx_reset(stats->width, stats->height, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

void jce_renderer_set_backbuffer_capture(JceRenderer *r, bool enable)
{
    (void)r;   /* the screenshot-based path needs no device reset */
    if (s_capture_active == enable) return;
    s_capture_active       = enable;
    s_capture_shot_pending = false;
}

/* -- Transform / texture binding (game-layer wrappers) ------------- */

void jce_renderer_set_transform(const float *mtx)
{
    bgfx_set_transform(mtx, 1);
}

void jce_renderer_bind_texture(const JceRenderer *r,
                               uint8_t stage,
                               JceTexture tex)
{
    if (!r || tex.idx == UINT16_MAX) return;
    bgfx_texture_handle_t th = { tex.idx };
    bgfx_set_texture(stage,
        (bgfx_uniform_handle_t){ r->u_tex_color.idx },
        th, UINT32_MAX);
}

void jce_renderer_dbg_text_attr(uint16_t x, uint16_t y,
                                uint8_t attr,
                                const char *str)
{
    if (!s_dbg_text_enabled)
        return;
    bgfx_dbg_text_printf(x, y, attr, "%s", str);
}

/* -- Wireframe debug mode ------------------------------------------ */

void jce_renderer_set_wireframe(JceRenderer *r, bool enabled)
{
    if (!r || r->is_fallback) return;
    if (enabled)
        r->debug_flags |= BGFX_DEBUG_WIREFRAME;
    else
        r->debug_flags &= ~(uint32_t)BGFX_DEBUG_WIREFRAME;
    bgfx_set_debug(r->debug_flags);
}

bool jce_renderer_get_wireframe(const JceRenderer *r)
{
    return r ? (r->debug_flags & BGFX_DEBUG_WIREFRAME) != 0 : false;
}

bool jce_renderer_origin_bottom_left(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    return caps ? caps->originBottomLeft : false;
}

uint32_t jce_renderer_get_frame_index(const JceRenderer *r)
{
    (void)r;
    return s_bgfx_frame_index;
}
