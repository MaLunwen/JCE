/*
 * jce_renderer_bgfx_callback.c -- bgfx's callback interface: diagnostics, the
 * shader cache, screenshots and backbuffer capture.
 *
 * Split out of jce_renderer.c, which was 3,247 lines -- past the 3,000-line cap
 * and frozen at the size gate's baseline.  See jce_renderer_bgfx_callback.h for
 * why this block was the one that moved.
 */

#include <jce/os/core/jce_config.h>   /* settings S5: machine_class */
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_str.h>   /* jce_strlcpy for the PNG writer job path */
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/platform/jce_library.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_gpu_capture.h>
#include <jce/renderer/jce_impostor.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_render_readback.h>
#include <jce/renderer/jce_shaders.h>
#include "renderer/jce_render_encoder.h"   /* jce_dbg_xform_matrices (frame reset) */
#include <jce/renderer/jce_ies_profile.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_views.h>
#include "os/core/jce_memory.h"
#include "os/core/jce_atomic_storage.h"
#include "os/platform/jce_window_internal.h"
#include "jce_renderer_caps_internal.h"
#include "jce_renderer_frame_serial_internal.h"
#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>   /* IMG_SavePNG for backbuffer screenshots */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jce_renderer_bgfx_callback.h"
#include "jce_renderer_bgfx_profiler_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include <limits.h>

#define LOG_TAG "jce_renderer"

#if defined(JCE_BGFX_DEBUG_ANNOTATION_COMPILED) && (JCE_BGFX_DEBUG_ANNOTATION_COMPILED == 1)
#define JCE_RCB_ANNOTATION_COMPILED 1
#elif defined(JCE_BGFX_DEBUG_ANNOTATION_COMPILED) && (JCE_BGFX_DEBUG_ANNOTATION_COMPILED == 0)
#define JCE_RCB_ANNOTATION_COMPILED 0
#else
#define JCE_RCB_ANNOTATION_COMPILED (-1)
#endif
#if defined(JCE_BGFX_DEBUG_ANNOTATION_REQUEST) && (JCE_BGFX_DEBUG_ANNOTATION_REQUEST == 1)
#define JCE_RCB_ANNOTATION_REQUEST 1
#elif defined(JCE_BGFX_DEBUG_ANNOTATION_REQUEST) && (JCE_BGFX_DEBUG_ANNOTATION_REQUEST == 0)
#define JCE_RCB_ANNOTATION_REQUEST 0
#else
#define JCE_RCB_ANNOTATION_REQUEST (-1)
#endif
#if defined(JCE_BGFX_DEBUG_ANNOTATION_OPTION) && (JCE_BGFX_DEBUG_ANNOTATION_OPTION == 1)
#define JCE_RCB_ANNOTATION_OPTION 1
#elif defined(JCE_BGFX_DEBUG_ANNOTATION_OPTION) && (JCE_BGFX_DEBUG_ANNOTATION_OPTION == 0)
#define JCE_RCB_ANNOTATION_OPTION 0
#else
#define JCE_RCB_ANNOTATION_OPTION (-1)
#endif

/* What this module holds ON BEHALF OF A HOST, in one struct rather than
 * loose names -- the shape the global-state detector has been teaching since
 * the text shape cache.  Folding `device_lost` in here as the capture hook
 * arrived keeps this module at ONE name, so the baseline ratcheted to 854
 * stays where it is instead of going to 855 for a second static.
 *
 * device_lost is set on bgfx's RENDER thread by the fatal handler and
 * read-and-cleared on the main thread by jce_renderer_take_device_lost();
 * see the handler for why it is a polled flag rather than a lifecycle emit.
 * capture_fn is installed and called on the main thread only. */
static struct {
    JceAtomicI32          device_lost;
    JceAtomicI32          profiler_begins;
    bool                  profiler_attempt_open; /* owner-thread boundary only */
    JceAutoCaptureFn       capture_fn;
    void                  *capture_ud;
} g_rcb_host = { .device_lost = { { 0 } }, .profiler_begins = { { 0 } } };

JceBgfxCpuProfilerWitness jce_rcb_cpu_profiler_witness(void)
{
    JceBgfxCpuProfilerWitness witness;
#if defined(JCE_BGFX_CPU_PROFILER_COMPILED) && (JCE_BGFX_CPU_PROFILER_COMPILED == 1)
    witness.compiled = 1;
#elif defined(JCE_BGFX_CPU_PROFILER_COMPILED) && (JCE_BGFX_CPU_PROFILER_COMPILED == 0)
    witness.compiled = 0;
#else
    witness.compiled = -1;
#endif
    witness.bridge_supported = JCE_BGFX_CPU_TLS_AVAILABLE != 0;
    witness.begin_observed = (uint32_t)jce_atomic_i32_load(&g_rcb_host.profiler_begins);
    witness.begin_count_saturated = witness.begin_observed == INT32_MAX;
    return witness;
}

static void jce_bgfx_cpu_profiler_observe_begin(void)
{
    int32_t before = jce_atomic_i32_load(&g_rcb_host.profiler_begins);
    while (before < INT32_MAX) {
        if (jce_atomic_i32_compare_exchange_internal(&g_rcb_host.profiler_begins,
                                                      before, before + 1))
            return;
        before = jce_atomic_i32_load(&g_rcb_host.profiler_begins);
    }
}

static void jce_bgfx_cpu_profiler_publish(const char *phase, const char *scope_name,
                                          JceTraceTaskState outcome)
{
    const JceBgfxCpuProfilerWitness witness = jce_rcb_cpu_profiler_witness();
    LOG_INFO(LOG_TAG, "bgfx CPU profiler: phase=%s compiled=%d bridge_supported=%u "
             "begin_observed=%u begin_count_saturated=%u annotation_request=%d "
             "annotation_option=%d annotation_compiled=%d", phase, (int)witness.compiled,
             (unsigned)witness.bridge_supported, (unsigned)witness.begin_observed,
             (unsigned)witness.begin_count_saturated, JCE_RCB_ANNOTATION_REQUEST,
             JCE_RCB_ANNOTATION_OPTION, JCE_RCB_ANNOTATION_COMPILED);
    if (jce_trace_enabled()) {
        JceTraceScope scope = jce_trace_scope_begin(scope_name);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.cpu.compiled", witness.compiled);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.gpu.annotation.request", JCE_RCB_ANNOTATION_REQUEST);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.gpu.annotation.option", JCE_RCB_ANNOTATION_OPTION);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.gpu.annotation.compiled", JCE_RCB_ANNOTATION_COMPILED);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.cpu.bridge_supported", witness.bridge_supported);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.cpu.begin_observed", witness.begin_observed);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.cpu.begin_saturated", witness.begin_count_saturated);
        jce_trace_scope_end(scope, outcome);
    }
}

void jce_rcb_cpu_profiler_attempt_end(JceBgfxCpuProfilerAttemptResult result)
{
    const char *phase, *scope_name;
    if (!g_rcb_host.profiler_attempt_open) return;
    switch (result) {
    case JCE_BGFX_CPU_ATTEMPT_INIT_FAILED:
        phase = "init_failed"; scope_name = "bgfx.cpu.capability.init_failed"; break;
    case JCE_BGFX_CPU_ATTEMPT_API_REFUSED:
        phase = "api_refused"; scope_name = "bgfx.cpu.capability.api_refused"; break;
    case JCE_BGFX_CPU_ATTEMPT_ALLOCATION_FAILED:
        phase = "allocation_failed"; scope_name = "bgfx.cpu.capability.allocation_failed"; break;
    default:
        /* Preserve an omitted terminal before reset without claiming a join. */
        phase = "superseded_unfinalized"; scope_name = "bgfx.cpu.capability.superseded_unfinalized"; break;
    }
    jce_bgfx_cpu_profiler_publish(phase, scope_name, JCE_TRACE_TASK_FAILED);
    g_rcb_host.profiler_attempt_open = false;
}

static void jce_bgfx_fatal(bgfx_callback_interface_t *_this, const char *_filePath, uint16_t _line,
                           bgfx_fatal_t _code, const char *_str)
{
    (void)_this;
    LOG_ERROR(LOG_TAG, "bgfx fatal: code=%d file=%s line=%u msg=%s", (int)_code,
              _filePath ? _filePath : "<null>", (unsigned)_line, _str ? _str : "<null>");

    /* THE ONLY REAL DEVICE-LOST SIGNAL ON THE REAL PATH.
     *
     * JCE_LIFECYCLE_DEVICE_LOST had zero emitters: a consumer's
     * jce_lifecycle_register(DEVICE_LOST, ...) succeeded and its callback was
     * dead code.  The obvious fix -- translating SDL_EVENT_RENDER_DEVICE_LOST
     * beside the existing DEVICE_RESET case in jce_engine.c -- is wrong here:
     * those are SDL_Render events, raised for an SDL_Renderer, and this engine
     * creates one ONLY in the safe-mode software fallback.  It would have
     * fired on the diagnostic path and never on a real backend.
     *
     * A FLAG, NOT AN EMIT, FOR TWO SEPARATE REASONS.
     *   1. LAYERS.  The renderer may not include <jce/application/...>;
     *      check_layer_dependencies.py refuses it, and it was right to --
     *      this is the same polled-flag shape as jce_rcb_capture_wants_shot
     *      below, which exists for the same reason.
     *   2. THREADS.  bgfx runs this on its render thread everywhere except
     *      macOS (which forces single-threaded mode to dodge a CAMetalLayer
     *      deadlock), and every lifecycle listener is written against the
     *      main-thread contract.  The engine polls this on the main thread.
     *
     * AND THE PROCESS DOES SURVIVE TO BE POLLED.  bgfx's fatal() aborts only
     * when NO callback is installed -- with one it delegates entirely and
     * returns -- so this handler returning means execution continues.  That
     * is a pre-existing decision of this engine, and it is exactly what makes
     * a deferred delivery reach anybody.
     *
     * Atomic read-and-clear preserves a notification arriving after the
     * consumer exchange. A volatile load followed by a store can lose that
     * notification and is a data race in C; no repeat from bgfx is assumed. */
    if (_code == BGFX_FATAL_DEVICE_LOST)
        jce_atomic_i32_store(&g_rcb_host.device_lost, 1);
    /* JCE_BGFX_TRAP=1: break on the FIRST fatal so the crash handler prints
     * the fully symbolized stack of the offender (debug-bgfx bug hunts). */
    {
        static int s_trap = -1;
        if (s_trap < 0) { const char *v = getenv("JCE_BGFX_TRAP");
                          s_trap = (v && v[0] == '1') ? 1 : 0; }
        if (s_trap) {
#if defined(_MSC_VER)
            __debugbreak();
#endif
        }
    }
}

/* When JCE_GFX_DEBUG=1, mirror bgfx internal traces (including the
 * D3D12 HRESULT printed right before "Failed to create PSO!") to our
 * log.  Otherwise stays silent to avoid spamming. */
static int s_bgfx_trace_enabled = -1;

static void jce_bgfx_trace_vargs(bgfx_callback_interface_t *_this, const char *_filePath,
                                 uint16_t _line, const char *_format, va_list _argList)
{
    bool api_version_line;

    (void)_this;
    if (s_bgfx_trace_enabled < 0) {
        const char *v = getenv("JCE_GFX_DEBUG");
        s_bgfx_trace_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (!_format)
        return;
    api_version_line = strstr(_format, "JCE runtime API version:") != NULL ||
                       strstr(_format, "JCE runtime shader version:") != NULL;
    if (!s_bgfx_trace_enabled && !api_version_line)
        return;

    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), _format, _argList);
    if (n < 0)
        return;
    const bool truncated = (size_t)n >= sizeof(buf);
    const int required = n;
    if (truncated)
        n = (int)sizeof(buf) - 1;
    /* Strip trailing newline that bgfx tends to append. */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    if (n == 0 && !truncated)
        return;

    if (api_version_line)
        jce_renderer_caps_api_capture_trace(buf);
    if (!s_bgfx_trace_enabled)
        return;

    const char *file = _filePath ? _filePath : "<bgfx>";
    /* Keep only the basename for compactness. */
    const char *slash = strrchr(file, '/');
    const char *back  = strrchr(file, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) file = slash + 1;

    /* Explicit debug mirroring survives DIST's ordinary INFO erasure. The
     * prefix also survives any subsequent canonical logger truncation. */
    if (truncated)
        jce_log_write(JCE_LOG_LEVEL_INFO, "bgfx", __FILE__, __LINE__,
            "[bgfx_trace_truncated required_bytes=%d] %s:%u %s",
            required, file, (unsigned)_line, buf);
    else
        jce_log_write(JCE_LOG_LEVEL_INFO, "bgfx", __FILE__, __LINE__,
            "%s:%u %s", file, (unsigned)_line, buf);
}

static void jce_bgfx_profiler_begin(bgfx_callback_interface_t *_this, const char *_name,
                                    uint32_t _abgr, const char *_filePath, uint16_t _line)
{
    (void)_this;
    (void)_abgr;
    (void)_filePath;
    (void)_line;
    jce_bgfx_cpu_profiler_observe_begin();
    jce_bgfx_cpu_trace_begin(_name);
}

static void jce_bgfx_profiler_begin_literal(bgfx_callback_interface_t *_this, const char *_name,
                                            uint32_t _abgr, const char *_filePath, uint16_t _line)
{
    (void)_this;
    (void)_abgr;
    (void)_filePath;
    (void)_line;
    jce_bgfx_cpu_profiler_observe_begin();
    jce_bgfx_cpu_trace_begin(_name);
}

static void jce_bgfx_profiler_end(bgfx_callback_interface_t *_this)
{
    (void)_this;
    jce_bgfx_cpu_trace_end();
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

/* One bounded file request, shared by screenshot and texture readback.
 * API admission/poll/shutdown are main-thread operations; callback and writer
 * completion cross threads. The short lock protects only these scalar/path
 * copies, never graphics, allocation, encoding, IO or worker waits. */
#define SCREENSHOT_PATH_MAX 512
#define SCREENSHOT_REQUEST_PREFIX "\x03__jce_file__"
static struct {
    SDL_SpinLock lock;
    uint64_t serial, identity, readback_identity;
    uint16_t screenshot_framebuffer;
    int result;                  /* -1 idle, 0 pending, 1 wrote, 2 failed */
    unsigned callbacks;         /* callbacks already owning pixels */
    bool closing, readback, writing, backend_active, trace_admitted;
    char path[SCREENSHOT_PATH_MAX];
    char request[64];            /* identity-bearing bgfx pseudo-path */
} s_file = { .result = -1 };

/* Private ring-counter ledger, not task lifetimes: both names use the file
 * identity as event.id, parent zero. Admit value is backbuffer=0/readback=1;
 * terminal value is JCE_TRACE_TASK_SUCCEEDED/FAILED/CANCELLED. Only accepted
 * requests traced at admission emit a terminal. A complete pair requires one
 * continuously enabled capture, with the core quiescence/loss rules. */
static bool rb_png_prepare(void);
static bool rb_png_primitives_ready(void);

static uint64_t screenshot_admit(const char *path, bool readback,
                                  uint16_t framebuffer)
{
    uint64_t id = 0;

    if (!path || !path[0] || strlen(path) >= SCREENSHOT_PATH_MAX ||
        (unsigned char)path[0] < 32)
        return 0;
    /* Prepare on the requesting main thread, before a callback can arrive.
     * Failed primitives reject before GPU work; a host without a writer
     * thread retains the existing inline backbuffer fallback. */
    SDL_LockSpinlock(&s_file.lock);
    bool available = s_file.backend_active && s_file.result != 0;
    SDL_UnlockSpinlock(&s_file.lock);
    if (!available)
        return 0;
    bool writer_ready = rb_png_prepare();
    if (!writer_ready && (readback || !rb_png_primitives_ready()))
        return 0;
    bool trace_admitted = jce_trace_enabled();
    SDL_LockSpinlock(&s_file.lock);
    if (s_file.backend_active && !s_file.closing && s_file.result != 0 &&
        s_file.serial != UINT64_MAX) {
        id = ++s_file.serial;
        s_file.identity = id;
        s_file.result = 0;
        s_file.readback = readback;
        s_file.screenshot_framebuffer = framebuffer;
        s_file.writing = false;
        s_file.trace_admitted = trace_admitted;
        jce_strlcpy(s_file.path, path, sizeof(s_file.path));
        snprintf(s_file.request, sizeof(s_file.request),
                 SCREENSHOT_REQUEST_PREFIX "%llu", (unsigned long long)id);
    }
    SDL_UnlockSpinlock(&s_file.lock);
    /* This returns before the caller can submit GPU work or complete an
     * early readback failure, so the admit precedes its terminal. */
    if (id && trace_admitted)
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
            "renderer.screenshot.admit", readback ? 1 : 0, id, 0);
    return id;
}

static void screenshot_complete(uint64_t id, bool success)
{
    bool trace_admitted = false;
    SDL_LockSpinlock(&s_file.lock);
    if (id && s_file.identity == id && s_file.result == 0) {
        s_file.result = success ? 1 : 2;
        trace_admitted = s_file.trace_admitted;
        s_file.trace_admitted = false;
    }
    SDL_UnlockSpinlock(&s_file.lock);
    if (trace_admitted)
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
            "renderer.screenshot.terminal",
            success ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED, id, 0);
}

static uint64_t screenshot_callback_begin(const char *request, char *path,
                                           uint16_t *framebuffer, bool *trace_pixels)
{
    uint64_t id = 0;

    SDL_LockSpinlock(&s_file.lock);
    if (request && !s_file.closing && s_file.result == 0 &&
        !s_file.readback && !s_file.writing &&
        strcmp(request, s_file.request) == 0) {
        id = s_file.identity;
        s_file.writing = true;
        ++s_file.callbacks;
        *framebuffer = s_file.screenshot_framebuffer;
        *trace_pixels = s_file.trace_admitted;
        memcpy(path, s_file.path, sizeof(s_file.path));
    }
    SDL_UnlockSpinlock(&s_file.lock);
    return id;
}

static void screenshot_callback_end(void);

/* Continuous capture (video recording). BGFX_RESET_CAPTURE does not deliver
   capture callbacks in this bgfx configuration, so recording instead drives
   the proven bgfx_request_screen_shot path: while active, end_frame requests a
   backbuffer shot each frame using the sentinel path below, and the screen_shot
   callback routes those pixels to the capture sink instead of writing a file. */
static struct {
    JceCaptureBeginFn begin;
    JceCaptureFrameFn frame;
    JceCaptureEndFn   end;
    void             *ud;
} s_capture_sink;
static bool s_capture_active       = false;
static bool s_capture_shot_pending = false;
/* Scratch for the GL RGBA->BGRA swizzle on the recording path; see the note at
   its use. Sized on demand, released when capture stops. */
static uint8_t *s_swz_buf;
static size_t   s_swz_cap;
/* When set, recording is driven by the ImGui renderer reading its offscreen FBO
   back into the sink (whole-window video) instead of the backbuffer screen_shot
   path below (which is black on D3D flip-model swap chains). */
static bool s_capture_imgui_mode   = false;

void jce_renderer_set_capture_imgui_mode(bool on) { s_capture_imgui_mode = on; }

/* One-shot offscreen-FBO RGBA readback (impostor bake).  The sentinel path
   below routes a requested screenshot's raw pixels (RGBA8, alpha preserved) to
   this sink instead of writing a file.  One in flight at a time. */
#define JCE_FBO_CAPTURE_SENTINEL "\x02__jce_fbo_capture__"
static struct {
    JceFboCaptureFn fn;
    void           *ud;
} s_fbo_capture_sink;
static bool s_fbo_capture_pending = false;

/* Defined with the PNG writer service further down. Copies `data` and hands
   the encode + file write to the writer thread; false means "not queued, do it
   yourself". */
static bool rb_png_submit_raw(const void *data, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t size, int format,
                              int yflip, int drop_alpha, const char *what,
                              const char *path, uint64_t file_identity);

static void jce_bgfx_screen_shot(bgfx_callback_interface_t *_this, const char *_filePath,
                                 uint32_t _width, uint32_t _height, uint32_t _pitch,
                                 bgfx_texture_format_t _format,
                                 const void *_data, uint32_t _size, bool _yflip)
{
    (void)_this;

    /* bgfx delivers the captured pixels in the source surface's NATIVE channel
       order: BGRA8 on D3D11/D3D12/Vulkan/Metal, but RGBA8 on OpenGL / OpenGL ES
       (glReadPixels). Unconditionally treating the data as BGRA8 swapped the
       red and blue channels in every screenshot taken on the GL backend.
       Honour the source format bgfx reports (>= 1.146); if it is neither known
       8-bit form, fall back to the active renderer type. */
    bool src_is_rgba;
    switch (_format) {
    case BGFX_TEXTURE_FORMAT_RGBA8: src_is_rgba = true;  break;
    case BGFX_TEXTURE_FORMAT_BGRA8: src_is_rgba = false; break;
    default: {
        bgfx_renderer_type_t rt = bgfx_get_renderer_type();
        src_is_rgba = (rt == BGFX_RENDERER_TYPE_OPENGL ||
                       rt == BGFX_RENDERER_TYPE_OPENGLES);
        break;
    }
    }
    {
        static bool s_ss_fmt_logged = false;
        if (!s_ss_fmt_logged) {
            s_ss_fmt_logged = true;
            LOG_INFO(LOG_TAG, "screenshot channel order: bgfx fmt=%d -> %s",
                     (int)_format, src_is_rgba ? "RGBA8 (no R/B swap)"
                                               : "BGRA8 (R/B swap)");
        }
    }

    /* Impostor-bake FBO readback: deliver raw RGBA8 (alpha preserved) to the
       one-shot sink instead of writing a file.  bgfx delivers BGRA8 with a row
       pitch; convert to tightly-packed RGBA8 for the consumer. */
    if (_filePath && strcmp(_filePath, JCE_FBO_CAPTURE_SENTINEL) == 0) {
        if (s_fbo_capture_sink.fn && _data && _width && _height) {
            uint8_t *rgba = (uint8_t *)JCE_MALLOC((size_t)_width * _height * 4u);
            if (rgba) {
                const uint8_t *src = (const uint8_t *)_data;
                for (uint32_t y = 0; y < _height; ++y) {
                    const uint8_t *srow = src + (size_t)y * _pitch;
                    uint8_t       *drow = rgba + (size_t)y * _width * 4u;
                    for (uint32_t x = 0; x < _width; ++x) {
                        /* Emit tightly-packed RGBA8 for the consumer, swapping
                           R/B only when the source is BGRA (D3D/VK); a GL source
                           is already RGBA. */
                        if (src_is_rgba) {
                            drow[x * 4 + 0] = srow[x * 4 + 0];
                            drow[x * 4 + 1] = srow[x * 4 + 1];
                            drow[x * 4 + 2] = srow[x * 4 + 2];
                            drow[x * 4 + 3] = srow[x * 4 + 3];
                        } else {
                            drow[x * 4 + 0] = srow[x * 4 + 2];
                            drow[x * 4 + 1] = srow[x * 4 + 1];
                            drow[x * 4 + 2] = srow[x * 4 + 0];
                            drow[x * 4 + 3] = srow[x * 4 + 3];
                        }
                    }
                }
                s_fbo_capture_sink.fn(s_fbo_capture_sink.ud, rgba, _width,
                                      _height, _yflip ? 1 : 0);
                JCE_FREE(rgba);
            }
        }
        s_fbo_capture_pending = false;
        s_fbo_capture_sink.fn = NULL;
        s_fbo_capture_sink.ud = NULL;
        return;
    }

    /* Recording frame: route pixels to the capture sink, write no file. The
       WebM sink consumes BGRA8; a GL source hands us RGBA8, so swap R/B into a
       scratch buffer first (D3D/VK are already BGRA and pass through). This is
       the backbuffer recording path (JCE_CAPTURE_SENTINEL); the editor's normal
       recording reads an RGBA16F FBO and converts explicitly elsewhere. */
    if (_filePath && strcmp(_filePath, JCE_CAPTURE_SENTINEL) == 0) {
        if (s_capture_active && _data && _width && _height) {
            const void *frame_data = _data;
            /* Persistent scratch, not a per-frame allocation.
             *
             * This is the recording path: it runs on EVERY captured frame, and
             * at 2560x1600 the buffer is 16 MB. Allocating and freeing 16 MB
             * per frame means faulting in ~4000 fresh pages per frame and
             * handing them straight back -- a per-frame cost that grows with
             * window size and does no work. The staging texture and pixel
             * buffer on the readback side were made resident for exactly this
             * reason in the earlier recording pass; this allocation was missed
             * because it only exists on GL (D3D and VK deliver BGRA and pass
             * through untouched).
             *
             * Freed when capture stops, so an idle editor holds nothing. */
            uint8_t    *swz = NULL;
            if (src_is_rgba) {
                if (s_swz_cap < (size_t)_size) {
                    JCE_FREE(s_swz_buf);
                    s_swz_buf = (uint8_t *)JCE_MALLOC((size_t)_size);
                    s_swz_cap = s_swz_buf ? (size_t)_size : 0;
                }
                swz = s_swz_buf;
                if (swz) {
                    const uint8_t *s = (const uint8_t *)_data;
                    for (uint32_t y = 0; y < _height; ++y) {
                        const uint8_t *srow = s   + (size_t)y * _pitch;
                        uint8_t       *drow = swz + (size_t)y * _pitch;
                        for (uint32_t x = 0; x < _width; ++x) {
                            drow[x * 4 + 0] = srow[x * 4 + 2];
                            drow[x * 4 + 1] = srow[x * 4 + 1];
                            drow[x * 4 + 2] = srow[x * 4 + 0];
                            drow[x * 4 + 3] = srow[x * 4 + 3];
                        }
                    }
                    frame_data = swz;
                }
            }
            if (s_capture_sink.begin)
                s_capture_sink.begin(s_capture_sink.ud, _width, _height, _pitch,
                                     _yflip ? 1 : 0);
            if (s_capture_sink.frame)
                s_capture_sink.frame(s_capture_sink.ud, frame_data, _size);
        }
        s_capture_shot_pending = false;
        return;
    }

    char output_path[SCREENSHOT_PATH_MAX];
    uint16_t framebuffer = UINT16_MAX;
    bool trace_pixels = false;
    uint64_t file_identity = screenshot_callback_begin(_filePath, output_path,
                                                        &framebuffer, &trace_pixels);
    if (!file_identity)
        return; /* late/duplicate callbacks cannot finish another request */
    _filePath = output_path;
    if (!_data || !_width || !_height || _width > UINT16_MAX ||
        _height > UINT16_MAX || _pitch > INT_MAX ||
        (uint64_t)_pitch < (uint64_t)_width * 4u ||
        (uint64_t)(_height - 1u) * _pitch + (uint64_t)_width * 4u > _size) {
        LOG_ERROR(LOG_TAG, "screenshot readback invalid: %s", _filePath);
        screenshot_complete(file_identity, false);
        screenshot_callback_end();
        return;
    }

    /* The provider delivered a validated CPU pixel layout. Encoding and
     * file outcome are separate, as are queue fences and presentation. */
    if (trace_pixels)
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
            "renderer.screenshot.cpu_pixels", framebuffer, file_identity, 0);

    /* DIAG (root-cause hunt for "F12/F9 black"): sample a 32x32 grid of the raw
       capture and log its average luminance.  avg_lum ~0 => the backbuffer itself
       was black at capture time (not rendered / occluded / wrong buffer), i.e. the
       problem is the DATA, not the write path.  Pitch-correct sampling. */
    if (_data && _width && _height) {
        const uint8_t *base = (const uint8_t *)_data;
        double sum = 0.0; uint32_t n = 0;
        uint32_t sy = _height / 32u ? _height / 32u : 1u;
        uint32_t sx = _width  / 32u ? _width  / 32u : 1u;
        for (uint32_t y = 0; y < _height; y += sy) {
            const uint8_t *row = base + (size_t)y * _pitch;
            for (uint32_t x = 0; x < _width; x += sx) {
                const uint8_t *px = row + (size_t)x * 4u;
                sum += px[0] + px[1] + px[2]; ++n;
            }
        }
        /* pitch and size are here because without them "black" has two very
         * different causes that look identical: a backbuffer that really was
         * not drawn, and a correctly drawn one being read with the wrong row
         * stride.  A capture whose only lit pixel is (0,0) is the second. */
        LOG_INFO(LOG_TAG,
                 "screenshot DIAG: %ux%u pitch=%u size=%u (w*4=%u, w*h*4=%u) "
                 "avg_lum=%.1f (n=%u) %s",
                 _width, _height, _pitch, _size, _width * 4u,
                 _width * _height * 4u, n ? sum / (n * 3.0) : 0.0, n,
                 (n && sum / (n * 3.0) < 1.0) ? "<-- BLACK backbuffer (data, not write)" : "");
    }

    /* This callback runs on the thread bgfx calls back on while the main thread
       sits inside bgfx_frame() waiting for it, so everything done here is
       frame stall, measured at 2560x1600:

           convert (SDL RGB24 + flip)        5-8 ms
           IMG_SavePNG encode + write      348-369 ms
           -------------------------------------------
           total                           354-375 ms per screenshot

       At a 13.8 ms frame that is a 26-frame freeze on every F12, and the same
       stall on every frame of a recording that writes stills. The readback
       capture path already solved this exact problem with a PNG writer thread
       -- it just was not wired to the backbuffer path, which is the one F12
       uses. So hand the raw pixels over and let the writer do the conversion
       too; the frame keeps only a memcpy of the staging buffer.

       Anything that cannot be handed over (queue full, allocation failed, or a
       .bmp, which the writer does not encode) still runs inline. A screenshot
       is never dropped to save a frame. */
    const uint64_t t_enter = jce_time_perf_counter();
    uint64_t t_conv = t_enter;
    bool async = false;

    if (_data && _filePath && _width && _height) {
        const char *aext = strrchr(_filePath, '.');
        const bool  is_bmp = aext && SDL_strcasecmp(aext, ".bmp") == 0;
        if (!is_bmp)
            async = rb_png_submit_raw(_data, _width, _height, _pitch, _size,
                                      (int)(src_is_rgba ? SDL_PIXELFORMAT_RGBA32
                                                        : SDL_PIXELFORMAT_BGRA32),
                                      _yflip ? 1 : 0, 1, "screenshot",
                                      _filePath, file_identity);
    }
    /* JCE_SHOT_DUAL=1 also writes the inline version to "<path>.inline.png".
     *
     * The two files then come from ONE callback invocation and one pixel
     * buffer, so a diff between them measures the code and nothing else.
     * Comparing an async PNG against an inline PNG from a SEPARATE run does
     * not: this scene's own run-to-run floor reaches 4.10%, which is larger
     * than any difference the two write paths could plausibly have, and a
     * cross-run comparison here reported 1.05-2.57%. Reading that as a defect
     * in the async path would have repeated an attribution error this campaign
     * has already made twice. */
    if (async) {
        static int s_dual = -1;
        if (s_dual < 0) {
            const char *dv = getenv("JCE_SHOT_DUAL");
            s_dual = (dv && dv[0] && dv[0] != '0') ? 1 : 0;
        }
        if (s_dual && _data && _filePath) {
            char ip[600];
            snprintf(ip, sizeof ip, "%s.inline.png", _filePath);
            SDL_Surface *ds = SDL_CreateSurfaceFrom((int)_width, (int)_height,
                src_is_rgba ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_BGRA32,
                (void *)(uintptr_t)_data, (int)_pitch);
            if (ds) {
                SDL_Surface *drgb = SDL_ConvertSurface(ds, SDL_PIXELFORMAT_RGB24);
                SDL_DestroySurface(ds);
                if (drgb) {
                    if (_yflip) SDL_FlipSurface(drgb, SDL_FLIP_VERTICAL);
                    IMG_SavePNG(drgb, ip);
                    SDL_DestroySurface(drgb);
                }
            }
        }
        const uint64_t t_end = jce_time_perf_counter();
        LOG_INFO(LOG_TAG, "screenshot queued in %.2f ms (%ux%u) — encode and "
                 "write run on the PNG writer thread",
                 jce_time_perf_to_ms(t_enter, t_end), _width, _height);
        screenshot_callback_end();
        return;
    }

    JceTraceScope write_scope = jce_trace_scope_begin("renderer.screenshot_write");
    bool ok = false;
    if (_data && _filePath && _width && _height) {
        /* Wrap the raw pixels in their native channel order (BGRA8 on D3D/VK,
           RGBA8 on GL — see src_is_rgba above), respecting the row pitch; drop
           the undefined backbuffer alpha by converting to RGB24, flip when the
           backend reports bottom-up data, then encode by file extension
           (.png default, .bmp optional). */
        SDL_Surface *src = SDL_CreateSurfaceFrom((int)_width, (int)_height,
            src_is_rgba ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_BGRA32,
            (void *)(uintptr_t)_data, (int)_pitch);
        if (src) {
            SDL_Surface *rgb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGB24);
            SDL_DestroySurface(src);
            if (rgb) {
                bool flipped = !_yflip || SDL_FlipSurface(rgb, SDL_FLIP_VERTICAL);
                t_conv = jce_time_perf_counter();
                const char *ext = strrchr(_filePath, '.');
                if (flipped) {
                    if (ext && SDL_strcasecmp(ext, ".bmp") == 0)
                        ok = SDL_SaveBMP(rgb, _filePath);
                    else
                        ok = IMG_SavePNG(rgb, _filePath);
                }
                SDL_DestroySurface(rgb);
            }
        }
    }

    {   /* Inline fallback took the frame with it — say so, with the split, so
           a regression back onto this path is visible rather than merely slow. */
        const uint64_t t_end = jce_time_perf_counter();
        LOG_WARN(LOG_TAG, "screenshot written INLINE: convert %.2f ms + "
                 "encode/write %.2f ms = %.2f ms blocking the frame (%ux%u)",
                 jce_time_perf_to_ms(t_enter, t_conv),
                 jce_time_perf_to_ms(t_conv, t_end),
                 jce_time_perf_to_ms(t_enter, t_end), _width, _height);
    }

    if (ok)
        LOG_SUCCESS(LOG_TAG, "screenshot saved: %s (%ux%u)", _filePath, _width, _height);
    else
        LOG_ERROR(LOG_TAG, "screenshot failed: %s (%s)",
                  _filePath ? _filePath : "(null)", SDL_GetError());

    screenshot_complete(file_identity, ok);
    jce_trace_scope_end(write_scope, ok ? JCE_TRACE_TASK_SUCCEEDED
                                       : JCE_TRACE_TASK_FAILED);
    screenshot_callback_end();
}

/* Request an async capture of the current frame's backbuffer to `path`.
   The shot is taken at the next bgfx_frame() and written from the screen_shot
   callback above.  Output format is chosen by `path`'s extension (.png by
   default).  Returns false if a capture is already pending or `path` is bad. */
bool jce_renderer_request_screenshot(const char *path)
{
    return jce_renderer_request_screenshot_fbo(UINT16_MAX, path);
}

bool jce_renderer_request_screenshot_fbo(uint16_t fbo_idx, const char *path)
{
    char request[64];
    bgfx_frame_buffer_handle_t fbh = { fbo_idx };

    if (!screenshot_admit(path, false, fbo_idx))
        return false;
    SDL_LockSpinlock(&s_file.lock);
    memcpy(request, s_file.request, sizeof(request));
    SDL_UnlockSpinlock(&s_file.lock);
    /* The callback resolves the identity to the bounded real output path.
     * This also distinguishes late callbacks for repeated identical paths. */
    bgfx_request_screen_shot(fbh, request);
    return true;
}

void JCE_CALL jce_renderer_set_auto_capture_hook(JceAutoCaptureFn fn, void *user)
{
    g_rcb_host.capture_fn = fn;
    g_rcb_host.capture_ud = user;
}

/* Let the host take the automated shot.  False means "not taken" -- either
 * nobody installed a hook, or the hook declined because it had nothing to
 * photograph this frame (the editor's Game View panel can be closed, in which
 * case its offscreen target does not exist).  The caller falls back to the
 * backbuffer, which is the behaviour every host had before this existed. */
bool jce_rcb_host_took_capture(const char *path)
{
    if (!g_rcb_host.capture_fn)
        return false;
    return g_rcb_host.capture_fn(g_rcb_host.capture_ud, path);
}

bool jce_renderer_take_device_lost(void)
{
    return jce_atomic_i32_exchange(&g_rcb_host.device_lost, 0) != 0;
}

int jce_renderer_screenshot_poll(void)
{
    int result;

    SDL_LockSpinlock(&s_file.lock);
    result = s_file.result;
    SDL_UnlockSpinlock(&s_file.lock);
    return result;
}

bool jce_renderer_screenshot_pending(void)
{
    return jce_renderer_screenshot_poll() == 0;
}

bool jce_renderer_request_fbo_capture(uint16_t fbo_idx,
                                      JceFboCaptureFn sink, void *ud)
{
    if (!sink || fbo_idx == UINT16_MAX)
        return false;
    if (s_fbo_capture_pending)
        return false;
    s_fbo_capture_sink.fn = sink;
    s_fbo_capture_sink.ud = ud;
    s_fbo_capture_pending = true;
    bgfx_frame_buffer_handle_t fbh = { fbo_idx };
    /* The sentinel path in jce_bgfx_screen_shot routes the pixels to the sink. */
    bgfx_request_screen_shot(fbh, JCE_FBO_CAPTURE_SENTINEL);
    return true;
}

/* ── Headless offscreen capture (blit + bgfx_read_texture) ───────────────────
 * bgfx_request_screen_shot only completes on a foreground PRESENT, so it never
 * fires for a background/headless window.  This path instead blits an LDR source
 * texture into a READ_BACK staging texture and reads it back to CPU — a pure
 * GPU->CPU copy that completes during normal frame processing, with no window
 * focus required.  Same proven pattern as the impostor bake / pick pass.  The
 * editor postfx output is RGBA16F (HDR-format, holding tonemapped 0..1), so we
 * stage RGBA16F and convert the half-floats to RGBA8 on read.  One in flight. */
/* A small FIFO ring of readback slots.  With ONE slot in flight the capture
   rate was framerate / (readback latency + 1) — bgfx completes a read
   ~2 frames after submit, so a 66 fps editor recorded at ~22 fps.  Three
   slots keep a readback in flight every frame; delivery stays strictly
   FIFO (the recorder sink timestamps at delivery, so out-of-order delivery
   would scramble frame times).  Staging textures and CPU buffers persist
   across frames while recording (they were created + destroyed per frame:
   30 MB of texture churn and 30 MB of malloc per capture at 2560x1494)
   and are released once everything is idle again. */
#define RB_SLOTS 3
typedef struct {
    int                    state;       /* 0 idle, 1 GPU pending, 2 retiring */
    int                    mode;        /* 0 = write PNG (path), 1 = feed capture sink */
    int                    yflip;       /* rows bottom-up? SOURCE-specific: the
                                         * postfx RT reads back bottom-up on
                                         * D3D, the ImGui recording FBO reads
                                         * back per texture origin — callers
                                         * pass what their source needs. */
    bgfx_texture_handle_t  staging;
    uint16_t               staging_w, staging_h;  /* size staging+pixels hold */
    uint8_t               *pixels;
    uint32_t               ready_frame;
    uint16_t               w, h;
    uint64_t               seq;         /* FIFO delivery order */
    uint64_t               file_identity;
    char                   path[512];
} RbSlot;

/* One legacy capture queue for the active renderer backend.  The API-thread
 * submit/poll owner and the after-backend-stop reset own the same state. */
typedef struct {
    RbSlot slots[RB_SLOTS];
    uint64_t seq_submit;   /* next sequence to hand out */
    uint64_t seq_deliver;  /* next sequence poll delivers */
    uint8_t *half_lut;
    bool format_unavailable_warned;
} RbQueue;

static RbQueue s_rb = { .seq_submit = 1, .seq_deliver = 1 };

/* Half-float -> byte LUT for the tonemapped 0..1 capture sources: one table
   lookup per channel instead of bit-twiddling + float math + clamp per
   pixel.  The per-pixel conversion ran on the MAIN thread (poll) and cost
   tens of ms per 2560x1494 frame — the single biggest "recording slows the
   editor" contributor.  64 KB, built on first use. */
static const uint8_t *rb_lut(void)
{
    if (!s_rb.half_lut) {
        uint8_t *lut = (uint8_t *)JCE_MALLOC(65536);
        if (!lut) return NULL;
        for (uint32_t hbits = 0; hbits < 65536u; ++hbits) {
            float f = jce_half_to_float((uint16_t)hbits);
            f = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
            lut[hbits] = (uint8_t)(f * 255.0f + 0.5f);
        }
        s_rb.half_lut = lut;
    }
    return s_rb.half_lut;
}

static void rb_slot_release(RbSlot *s)
{
    if (s->pixels) { JCE_FREE(s->pixels); s->pixels = NULL; }
    /* staging_w gates validity: the slots are static-zero-initialized, and a
       zeroed bgfx handle (idx 0) would otherwise LOOK valid and destroy a
       live texture. */
    if (s->staging_w && BGFX_HANDLE_IS_VALID(s->staging))
        bgfx_destroy_texture(s->staging);
    s->staging.idx = UINT16_MAX;
    s->staging_w = s->staging_h = 0;
    s->state = 0;
}

/* Shared submit: blit src (RGBA16F) -> READ_BACK staging and kick the read.
   mode 0 -> the poll writes `path` as a PNG; mode 1 -> the poll converts to BGRA8
   and feeds the capture sink (video recording).  Returns false when every
   slot is busy — the caller skips this frame and submits again next frame. */
static bool rb_submit(uint16_t src_tex_idx, uint16_t blit_view,
                      uint16_t w, uint16_t h, int mode, int yflip,
                      const char *path)
{
    if (src_tex_idx == UINT16_MAX || w == 0 || h == 0)
        return false;
    if (mode == 0 && (!path || !path[0]))
        return false;
    /* A promised frame is not proof a backend performed an unsupported blit.
     * Reuse the canonical same-format admission before allocating or admitting
     * a file request. In this provider every uint32 ready serial can be valid. */
    if (!jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA16F)) {
        if (!s_rb.format_unavailable_warned) {
            s_rb.format_unavailable_warned=true;
            LOG_WARN(LOG_TAG,"readback capture unavailable: RGBA16F blit/read-back capability missing");
        }
        return false;
    }

    RbSlot *slot = NULL;
    for (int i = 0; i < RB_SLOTS; ++i)
        if (s_rb.slots[i].state == 0) { slot = &s_rb.slots[i]; break; }
    if (!slot)
        return false;
    uint64_t file_identity = mode == 0 ? screenshot_admit(path, true, UINT16_MAX) : 0;
    if (mode == 0 && !file_identity)
        return false;

    /* (Re)create the staging texture + CPU buffer only when the size changed;
       both persist across captures (released when everything is idle).
       staging_w==0 covers the static-zero-init state (handle idx 0 would
       otherwise look valid). */
    if (slot->staging_w != w || slot->staging_h != h || !slot->pixels) {
        rb_slot_release(slot);
        /* Match the editor postfx/UI output's RGBA16F format (blit requires
           equal formats); convert half-floats -> 8-bit in the poll. */
        slot->staging = bgfx_create_texture_2d(w, h, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA16F,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
        if (!BGFX_HANDLE_IS_VALID(slot->staging)) {
            screenshot_complete(file_identity, false);
            return false;
        }
        slot->pixels = (uint8_t *)JCE_MALLOC((size_t)w * h * 8u); /* RGBA16F */
        if (!slot->pixels) {
            bgfx_destroy_texture(slot->staging);
            slot->staging.idx = UINT16_MAX;
            screenshot_complete(file_identity, false);
            return false;
        }
        slot->staging_w = w;
        slot->staging_h = h;
    }


    bgfx_texture_handle_t src = { src_tex_idx };
    /* blit_view must sort AFTER the source's render pass so the blit reads this
       frame's fully-composited pixels. */
    bgfx_blit(blit_view, slot->staging, 0, 0, 0, 0, src, 0, 0, 0, 0, w, h, 1);
    slot->mode  = mode;
    slot->yflip = yflip;
    slot->path[0] = '\0';
    if (mode == 0) snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->ready_frame = bgfx_read_texture(slot->staging, slot->pixels, 0, 0);
    slot->w = w; slot->h = h;
    slot->seq = s_rb.seq_submit++;
    slot->file_identity = file_identity;
    if (mode == 0) {
        SDL_LockSpinlock(&s_file.lock);
        s_file.readback_identity = file_identity;
        SDL_UnlockSpinlock(&s_file.lock);
    }
    slot->state = 1;
    return true;
}

bool jce_renderer_readback_capture_submit(uint16_t src_tex_idx, uint16_t blit_view,
                                          uint16_t w, uint16_t h, const char *path,
                                          int yflip)
{
    /* `yflip` is SOURCE-specific, same contract as _submit_sink below: 1 for
     * the engine postfx RT (bottom-up rows, empirically verified on D3D11
     * headless captures), bgfx caps originBottomLeft for plain FBO passes
     * (the ImGui whole-window FBO reads back TOP-down on D3D — hardcoding 1
     * here is what inverted D3D11/D3D12/VK F12+WINCAP PNGs). */
    return rb_submit(src_tex_idx, blit_view, w, h, 0, yflip, path);
}

/* Recording variant: read the source back and feed it to the capture sink as
   BGRA8 (the WebM encoder's input format). One in flight; returns false if busy
   so the caller simply skips this frame (the next frame submits again).
   `yflip`: are the SOURCE texture's readback rows bottom-up? This is a
   property of how the source was rendered, not just of the backend — pass
   1 for the postfx RT, bgfx caps originBottomLeft for plain FBO passes
   (hardcoding 1 here is what inverted D3D12 recordings). */
bool jce_renderer_readback_capture_submit_sink(uint16_t src_tex_idx, uint16_t blit_view,
                                               uint16_t w, uint16_t h, int yflip)
{
    return rb_submit(src_tex_idx, blit_view, w, h, 1, yflip, NULL);
}

/* ── Off-thread PNG writer ──────────────────────────────────────────────
 *
 * The readback itself is already asynchronous (staging + N slots), but the
 * DELIVERY encoded the PNG on the main thread.  A 1280x720 zlib encode plus
 * the file write costs on the order of 100 ms, which showed up as a
 * "frame_dt exceeded max_frame_dt; clamping to avoid spiral of death" warning
 * timestamped against every single capture: one screenshot stalled the frame
 * hard enough for the fixed clock to drop simulated time.
 *
 * Encoding is pure CPU work on a private pixel buffer with a private output
 * path, so it moves to a writer thread. The main thread keeps only the LUT
 * conversion (a few ms) and hands the buffer over. The queue is bounded; when
 * it is full the ready readback slot stays pending and retries next frame.
 * This preserves every capture without ever encoding inline on a frame. */
#define RB_PNG_QUEUE 4

typedef struct {
    uint8_t *pixels;     /* owned; freed by whoever encodes it */
    uint16_t w, h;
    uint32_t pitch;      /* bytes per row in `pixels` */
    int      format;     /* SDL_PixelFormat of `pixels` */
    int      yflip;
    int      drop_alpha; /* convert to RGB24 first (backbuffer alpha is junk) */
    const char *what;    /* log label; a literal, never freed */
    char     path[512];
    uint64_t file_identity;
} RbPngJob;

static RbPngJob   s_png_queue[RB_PNG_QUEUE];
static int        s_png_head, s_png_tail, s_png_count;
static JceMutex  *s_png_mu;
static JceCondVar *s_png_cv;       /* signalled on enqueue and on shutdown */
static JceCondVar *s_png_drained;  /* signalled when the queue empties     */
static JceThread *s_png_thread;
static bool       s_png_quit;
static int        s_png_busy;      /* jobs handed out but not yet finished */

/* Encode + write on the serial writer service. Takes ownership of rgba8. */
static void rb_png_write(RbPngJob *job)
{
    bool ok = false;
    JceTraceScope scope = jce_trace_scope_begin("renderer.screenshot_write");
    SDL_Surface *surface = SDL_CreateSurfaceFrom((int)job->w, (int)job->h,
        (SDL_PixelFormat)job->format, job->pixels, (int)job->pitch);

    if (surface && job->drop_alpha) {
        SDL_Surface *rgb = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
        SDL_DestroySurface(surface);
        surface = rgb;
    }
    if (surface) {
        if (!job->yflip || SDL_FlipSurface(surface, SDL_FLIP_VERTICAL))
            ok = IMG_SavePNG(surface, job->path);
        SDL_DestroySurface(surface); /* exactly one owner, including RGBA */
    }
    if (ok)
        LOG_SUCCESS(LOG_TAG, "%s saved: %s (%ux%u)",
                    job->what, job->path, job->w, job->h);
    else
        LOG_ERROR(LOG_TAG, "%s PNG write failed: %s (%s)",
                  job->what, job->path, SDL_GetError());
    JCE_FREE(job->pixels);
    job->pixels = NULL;
    screenshot_complete(job->file_identity, ok);
    jce_trace_scope_end(scope, ok ? JCE_TRACE_TASK_SUCCEEDED
                                : JCE_TRACE_TASK_FAILED);
}

static void screenshot_callback_end(void)
{
    /* The writer primitives exist before the GPU request is accepted. Keep
     * the same lock order as shutdown: writer mutex, then tiny state lock. */
    jce_mutex_lock(s_png_mu);
    SDL_LockSpinlock(&s_file.lock);
    --s_file.callbacks;
    SDL_UnlockSpinlock(&s_file.lock);
    jce_cond_broadcast(s_png_drained);
    jce_mutex_unlock(s_png_mu);
}

static void rb_png_worker(void *unused)
{
    (void)unused;
    for (;;) {
        RbPngJob job;
        jce_mutex_lock(s_png_mu);
        while (s_png_count == 0 && !s_png_quit)
            jce_cond_wait(s_png_cv, s_png_mu);
        if (s_png_count == 0 && s_png_quit) {
            jce_mutex_unlock(s_png_mu);
            return;
        }
        job = s_png_queue[s_png_head];
        s_png_head = (s_png_head + 1) % RB_PNG_QUEUE;
        s_png_count--;
        s_png_busy++;
        jce_mutex_unlock(s_png_mu);

        rb_png_write(&job);

        jce_mutex_lock(s_png_mu);
        s_png_busy--;
        if (s_png_count == 0 && s_png_busy == 0)
            jce_cond_broadcast(s_png_drained);
        jce_mutex_unlock(s_png_mu);
    }
}

static bool rb_png_prepare(void)
{
    if (!s_png_mu) {
        s_png_mu      = jce_mutex_create();
        s_png_cv      = jce_cond_create();
        s_png_drained = jce_cond_create();
        if (!s_png_mu || !s_png_cv || !s_png_drained) {
            if (s_png_drained) jce_cond_destroy(s_png_drained);
            if (s_png_cv) jce_cond_destroy(s_png_cv);
            if (s_png_mu) jce_mutex_destroy(s_png_mu);
            s_png_drained = NULL;
            s_png_cv = NULL;
            s_png_mu = NULL;
            return false;
        }
    }
    SDL_LockSpinlock(&s_file.lock);
    s_file.closing = false; /* reopen only from main-thread file admission */
    SDL_UnlockSpinlock(&s_file.lock);
    if (!s_png_thread) {
        s_png_thread = jce_thread_create(rb_png_worker, NULL, "jce-png-write");
        if (!s_png_thread)
            return false;
    }
    return true;
}

static bool rb_png_primitives_ready(void)
{
    return s_png_mu != NULL;
}

static bool rb_png_has_capacity(void)
{
    bool has_capacity;

    jce_mutex_lock(s_png_mu);
    has_capacity = s_png_count < RB_PNG_QUEUE;
    jce_mutex_unlock(s_png_mu);
    return has_capacity;
}

/* Hand a converted frame to the writer service without blocking. */
static bool rb_png_enqueue(RbPngJob *job)
{
    bool queued = false;

    jce_mutex_lock(s_png_mu);
    if (s_png_count < RB_PNG_QUEUE) {
        s_png_queue[s_png_tail] = *job;
        s_png_tail = (s_png_tail + 1) % RB_PNG_QUEUE;
        s_png_count++;
        queued = true;
        jce_cond_signal(s_png_cv);
    }
    jce_mutex_unlock(s_png_mu);
    return queued;
}

static bool rb_png_submit_raw(const void *data, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t size, int format,
                              int yflip, int drop_alpha, const char *what,
                              const char *path, uint64_t file_identity)
{
    if (!data || !path || !w || !h || !size) return false;
    if (w > 0xFFFFu || h > 0xFFFFu)          return false;
    if (!s_png_thread)                      return false;

    uint8_t *copy = (uint8_t *)JCE_MALLOC((size_t)size);
    if (!copy) return false;
    memcpy(copy, data, (size_t)size);

    RbPngJob job;
    job.pixels     = copy;
    job.w          = (uint16_t)w;
    job.h          = (uint16_t)h;
    job.pitch      = pitch;
    job.format     = format;
    job.yflip      = yflip;
    job.drop_alpha = drop_alpha;
    job.what       = what;
    job.file_identity = file_identity;
    jce_strlcpy(job.path, path, sizeof job.path);
    if (!rb_png_enqueue(&job)) {
        JCE_FREE(copy);
        return false;
    }
    return true;
}

void jce_renderer_readback_capture_flush(void)
{
    if (!s_png_mu) return;
    jce_mutex_lock(s_png_mu);
    while (s_png_count > 0 || s_png_busy > 0)
        jce_cond_wait(s_png_drained, s_png_mu);
    jce_mutex_unlock(s_png_mu);
}

void jce_renderer_readback_capture_shutdown(void)
{
    unsigned callbacks;
    uint64_t cancelled_trace_identity = 0;

    SDL_LockSpinlock(&s_file.lock);
    s_file.closing = true;
    s_file.backend_active = false;
    SDL_UnlockSpinlock(&s_file.lock);
    if (s_png_mu) {
        jce_mutex_lock(s_png_mu);
        for (;;) {
            SDL_LockSpinlock(&s_file.lock);
            callbacks = s_file.callbacks;
            SDL_UnlockSpinlock(&s_file.lock);
            if (!callbacks)
                break;
            jce_cond_wait(s_png_drained, s_png_mu);
        }
        jce_mutex_unlock(s_png_mu);
        /* Wait only for callbacks already owning pixels and queued CPU work.
         * Never wait for a bgfx callback not yet delivered: bgfx_shutdown
         * runs later, so waiting for that callback here would deadlock. */
        jce_renderer_readback_capture_flush();
        jce_mutex_lock(s_png_mu);
        s_png_quit = true;
        jce_cond_broadcast(s_png_cv);
        jce_mutex_unlock(s_png_mu);
        if (s_png_thread) {
            jce_thread_join(s_png_thread);
            s_png_thread = NULL;
        }
        jce_cond_destroy(s_png_drained);
        jce_cond_destroy(s_png_cv);
        jce_mutex_destroy(s_png_mu);
        s_png_drained = NULL;
        s_png_cv = NULL;
        s_png_mu = NULL;
        s_png_quit = false;
        s_png_head = s_png_tail = s_png_count = s_png_busy = 0;
    }
    SDL_LockSpinlock(&s_file.lock);
    if (s_file.result == 0) {
        s_file.result = 2;
        if (s_file.trace_admitted)
            cancelled_trace_identity = s_file.identity;
        s_file.trace_admitted = false;
    }
    s_file.readback_identity = 0;
    SDL_UnlockSpinlock(&s_file.lock);
    if (cancelled_trace_identity)
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
            "renderer.screenshot.terminal", JCE_TRACE_TASK_CANCELLED,
            cancelled_trace_identity, 0);
    for (int i = 0; i < RB_SLOTS; ++i) {
        RbSlot *slot = &s_rb.slots[i];
        if (slot->state == 1) {
            /* bgfx still borrows pixels. Retire the GPU handle in command
             * order, but retain CPU storage until the backend has joined. */
            if (slot->staging_w && BGFX_HANDLE_IS_VALID(slot->staging))
                bgfx_destroy_texture(slot->staging);
            slot->staging.idx = UINT16_MAX;
            slot->staging_w = slot->staging_h = 0;
            slot->state = 2;
        } else if (slot->state == 0) {
            rb_slot_release(slot);
        }
    }
    s_rb.seq_deliver = s_rb.seq_submit;
}

/* Called only after bgfx_shutdown has joined the backend. No GPU API calls:
 * all pending readbacks have now finished or been cancelled by their owner. */
void jce_rcb_after_backend_shutdown(void)
{
    if (g_rcb_host.profiler_attempt_open) {
        jce_bgfx_cpu_profiler_publish("after_backend_shutdown",
            "bgfx.cpu.capability.after_backend_shutdown", JCE_TRACE_TASK_SUCCEEDED);
        g_rcb_host.profiler_attempt_open = false;
    }
    jce_atomic_i32_store(&g_rcb_host.device_lost, 0);
    SDL_LockSpinlock(&s_file.lock);
    s_file.backend_active = false;
    s_file.closing = true;
    s_file.readback_identity = 0;
    SDL_UnlockSpinlock(&s_file.lock);
    for (int i = 0; i < RB_SLOTS; ++i) {
        RbSlot *slot = &s_rb.slots[i];
        JCE_FREE(slot->pixels);
        memset(slot, 0, sizeof(*slot));
        slot->staging.idx = UINT16_MAX;
    }
    JCE_FREE(s_rb.half_lut);
    s_rb.half_lut = NULL;
    s_rb.seq_submit = s_rb.seq_deliver = 1;
    s_rb.format_unavailable_warned=false;
}

/* Deliver one ready slot, strictly FIFO.  Returns the poll result code. */
static int rb_deliver(RbSlot *slot)
{
    int result = 2;
    size_t npx = (size_t)slot->w * (size_t)slot->h;
    const uint16_t *src = (const uint16_t *)slot->pixels;
    const uint8_t  *lut = rb_lut();

    if (slot->mode == 1) {
        /* Recording: RGBA16F -> BGRA8 (encoder reads B,G,R,A) via the LUT and
           feed the sink with the row order the SUBMITTER declared. */
        uint8_t *bgra = lut ? (uint8_t *)JCE_MALLOC(npx * 4u) : NULL;
        if (bgra && s_capture_active) {
            for (size_t i = 0; i < npx; ++i) {
                bgra[i * 4 + 0] = lut[src[i * 4 + 2]];
                bgra[i * 4 + 1] = lut[src[i * 4 + 1]];
                bgra[i * 4 + 2] = lut[src[i * 4 + 0]];
                bgra[i * 4 + 3] = 255;
            }
            if (s_capture_sink.begin)
                s_capture_sink.begin(s_capture_sink.ud, slot->w, slot->h,
                                     (uint32_t)slot->w * 4u, slot->yflip);
            if (s_capture_sink.frame)
                s_capture_sink.frame(s_capture_sink.ud, bgra,
                                     (uint32_t)(npx * 4u));
            result = 1;
        }
        if (bgra) JCE_FREE(bgra);
        slot->state = 0;   /* staging + pixels stay cached for the next frame */
        return result;
    }

    /*
     * mode 0: convert the RGBA16F half-float readback (tonemapped 0..1)
     * to RGBA8 PNG. A saturated writer queue leaves this slot ready so
     * polling retries it on a later frame.
     */
    if (!rb_png_prepare()) {
        LOG_ERROR(LOG_TAG, "readback capture PNG service unavailable");
        screenshot_complete(slot->file_identity, false);
        slot->state = 0;
        return 2;
    }
    if (!rb_png_has_capacity())
        return 0;

    uint8_t *rgba8 = lut ? (uint8_t *)JCE_MALLOC(npx * 4u) : NULL;
    if (rgba8) {
        for (size_t i = 0; i < npx * 4u; ++i)
            rgba8[i] = lut[src[i]];
        RbPngJob job;
        job.pixels     = rgba8;
        job.w          = slot->w;
        job.h          = slot->h;
        job.pitch      = (uint32_t)slot->w * 4u;
        job.format     = (int)SDL_PIXELFORMAT_RGBA32;
        job.yflip      = slot->yflip;
        job.drop_alpha = 0;
        job.what       = "readback capture";
        job.file_identity = slot->file_identity;
        jce_strlcpy(job.path, slot->path, sizeof job.path);
        if (!rb_png_enqueue(&job)) {
            JCE_FREE(rgba8);
            return 0;
        }
        result = 1; /* queued; the outer poll still awaits real file completion */
    } else {
        screenshot_complete(slot->file_identity, false);
    }
    slot->state = 0;
    return result;
}

int jce_renderer_readback_capture_poll(void)
{
    int any_in_flight = 0;
    int last_result = -1;

    /* Deliver every slot that is ready, in strict submit order.  Stops at
       the first not-yet-ready slot so a fast later readback can never
       overtake an earlier one (the recorder timestamps at delivery). */
    for (;;) {
        RbSlot *next = NULL;
        for (int i = 0; i < RB_SLOTS; ++i) {
            if (s_rb.slots[i].state == 1) {
                any_in_flight = 1;
                if (s_rb.slots[i].seq == s_rb.seq_deliver)
                    next = &s_rb.slots[i];
            }
        }
        if (!next)
            break;
        if (!jce_renderer_frame_ready(jce_rcb_host_frame_index(),next->ready_frame))
            return 0;   /* oldest capture still on the GPU */
        last_result = rb_deliver(next);
        if (next->state == 1)
            return 0;   /* writer backpressure: preserve FIFO and retry */
        s_rb.seq_deliver++;
        any_in_flight = 0;   /* recount on the next loop iteration */
    }

    /* Everything idle and no continuous capture running: release the cached
       staging textures + CPU buffers (90 MB VRAM + 90 MB RAM at 2560x1494
       across 3 slots — worth keeping only while recording). */
    if (!any_in_flight && !s_capture_active) {
        for (int i = 0; i < RB_SLOTS; ++i)
            if (s_rb.slots[i].state == 0 && s_rb.slots[i].staging_w)
                rb_slot_release(&s_rb.slots[i]);
    }
    SDL_LockSpinlock(&s_file.lock);
    if (s_file.readback_identity) {
        int result = s_file.identity == s_file.readback_identity ? s_file.result : 2;
        if (result != 0)
            s_file.readback_identity = 0;
        SDL_UnlockSpinlock(&s_file.lock);
        return result;
    }
    SDL_UnlockSpinlock(&s_file.lock);
    return last_result != -1 ? last_result : (any_in_flight ? 0 : -1);
}

bool jce_renderer_fbo_capture_pending(void)
{
    return s_fbo_capture_pending;
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



/* -- Boundary (see jce_renderer_bgfx_callback.h) -------------------- */

void jce_rcb_backend_started(void)
{
    SDL_LockSpinlock(&s_file.lock);
    s_file.backend_active = true;
    s_file.closing = false;
    SDL_UnlockSpinlock(&s_file.lock);
}

bgfx_callback_interface_t *jce_rcb_callback_interface(void)
{
    /* Attempts are serialized by renderer admission. Preserve any omitted
     * terminal explicitly; it is not evidence of a successful backend join. */
    jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_SUPERSEDED_UNFINALIZED);
    jce_atomic_i32_store(&g_rcb_host.device_lost, 0);
    jce_atomic_i32_store(&g_rcb_host.profiler_begins, 0);
    g_rcb_host.profiler_attempt_open = true;
    jce_bgfx_cpu_profiler_publish("init_attempt", "bgfx.cpu.capability.init_attempt",
                                  JCE_TRACE_TASK_SUCCEEDED);
    return &s_bgfx_callback;
}

bool jce_rcb_capture_wants_shot(void)
{
    return s_capture_active && !s_capture_imgui_mode &&
           !s_capture_shot_pending && !jce_renderer_screenshot_pending();
}

void jce_rcb_capture_mark_shot_pending(void)
{
    s_capture_shot_pending = true;
}

void jce_rcb_capture_set_active(bool enable)
{
    if (s_capture_active == enable) return;
    s_capture_active       = enable;
    s_capture_shot_pending = false;
    if (!enable) {                 /* idle editor holds no capture scratch */
        JCE_FREE(s_swz_buf);
        s_swz_buf = NULL;
        s_swz_cap = 0;
    }
}
