/*
 * jce_renderer.c  bgfx renderer implementation.
 */

#include <jce/os/core/jce_config.h>   /* settings S5: machine_class */
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_str.h>   /* jce_strlcpy for the PNG writer job path */
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/platform/jce_library.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_gpu_capture.h>
#include <jce/renderer/jce_impostor.h>
#include "jce_impostor_internal.h"
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_shaders.h>
#include "renderer/jce_render_encoder.h"   /* jce_dbg_xform_matrices (frame reset) */
#include <jce/renderer/jce_ies_profile.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/platform/jce_window_internal.h"
#include "os/platform/jce_gl_entry_native_internal.h"
#include "jce_renderer_caps_internal.h"
#include "jce_offscreen_retirement_internal.h"
#include "jce_render_readback_internal.h"
#include "jce_upload_metrics.h"
#include "jce_instance_upload.h"
#include "jce_renderer_lifecycle_internal.h"
#include "jce_renderer_launch_policy_internal.h"
#include "jce_renderer_snapshot_internal.h"
#include "jce_renderer_submission_trace_internal.h"
#include "jce_bgfx_allocator_trace_internal.h"
#include "jce_renderer_budget_internal.h"
#include "jce_shader_set_owned_internal.h"
#include <jce/os/core/jce_trace.h>
#include "jce_lighting_system_internal.h"

#include <bgfx/c99/bgfx.h>
#include "jce_renderer_reset_trace_internal.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>   /* IMG_SavePNG for backbuffer screenshots */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "jce_renderer_bgfx_callback.h"
#include "jce_shader_deferred.h"

#define LOG_TAG "jce_renderer"

/* One authoritative owner for the singleton bgfx device lifecycle. The
 * borrowed upload target and generation share the existing frame owner;
 * neither the decode workers nor Android frame worker mutates this bridge. */
#include "jce_renderer_admission_internal.h"

typedef struct JceRendererDeviceState {
    JceRenderer *owner;
    JceRendererAdmission admission;
    JceBgfxAllocatorTrace allocation_trace;
    uint32_t frame_index;
    uint32_t offscreen_submission_index;
    JceUploadMetricsBridge payload;
    JceShaderDeferredState shader_deferred;
    bool render_resource_diag;
} JceRendererDeviceState;
static JceRendererDeviceState s_device;
/* Rejected duplicates reach neither bgfx_init nor mutable platform/caps state.
 * Registered engine main-thread policy applies; unmarked standalone callers
 * still own the documented bgfx API-thread lifecycle. */
bool jce_renderer_backend_create_begin_internal(void)
{
    const bool main_owner = jce_thread_is_main();
    if (main_owner && jce_renderer_admission_try_begin(&s_device.admission)) {
        s_device.render_resource_diag = jce_env_flag_enabled(getenv("JCE_RENDER_RESOURCE_DIAG"));
        return true;
    }
    /* One bounded counter for this refused call, correlated with its existing
     * create scope; trace-off has no ring activity or per-frame log spam. */
    const uint64_t owner = jce_trace_task_current_id();
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
        "renderer.create.rejection_reason",
        main_owner ? JCE_RENDERER_ADMISSION_REFUSED_BUSY
                   : JCE_RENDERER_ADMISSION_REFUSED_FOREIGN, owner, owner);
    return false;
}

void jce_renderer_backend_create_end_internal(bool created)
{
    jce_renderer_admission_finish(&s_device.admission, created);
}

void jce_renderer_backend_destroy_end_internal(void)
{
    jce_renderer_admission_release_joined(&s_device.admission);
}

bool jce_renderer_shader_provenance_policy_internal(void)
{ return !JCE_PLATFORM_WEB && s_device.render_resource_diag; }

JceShaderDeferredState *jce_renderer_shader_deferred_state(void)
{
    return &s_device.shader_deferred;
}

/* Read by jce_renderer_bgfx_callback.c: the trace callback stamps each line
 * with the frame it belongs to, and the frame counter is the renderer's. */
uint32_t jce_rcb_host_frame_index(void) { return s_device.frame_index; }

bool jce_renderer_gpu_observation_context_internal(uint64_t *generation,
                                                    uint32_t *host_frame)
{
    if (generation) *generation = 0;
    if (host_frame) *host_frame = 0;
    const JceSubmittedPayloadStats *active = s_device.payload.active;
    if (!active || !active->valid || !active->context_generation ||
        !jce_renderer_caps_api_web_context_current()) return false;
    if (generation) *generation = active->context_generation;
    if (host_frame) *host_frame = s_device.frame_index;
    return true;
}

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
        s_device.frame_index = bgfx_frame(false); /* may block forever in eglSwapBuffers on WSA */
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
/* Backend probe uses jce_library_exists (platform loader) for vulkan/d3d
 * library presence — keeps <windows.h> and <dlfcn.h> out of engine sources. */
#else
#include <setjmp.h>
#include <signal.h>
#if JCE_PLATFORM_ANDROID
#include <sys/system_properties.h>
#endif
#endif

struct JceRenderer {
    JceOffscreenTarget *offscreen_targets;
    JceInstanceUploadOwner instance_upload;
    JceSubmittedPayloadStats submitted_payload;
    bool is_fallback;
    /* NullRHI: bgfx NOOP backend, no window (dedicated-server / headless).
     * Render entry points early-out like is_fallback so nothing is submitted,
     * but resource/scene calls that create bgfx handles stay valid no-ops. */
    bool headless;
    SDL_Renderer *sdl_renderer;

    bgfx_program_handle_t program;          /* color (pos+color) */
    bgfx_vertex_layout_t layout;            /* color vertex layout */
    bgfx_program_handle_t program_textured; /* textured (pos+color+uv) */
    bgfx_program_handle_t program_text_sdf; /* glyphs from a distance field */
    bgfx_vertex_layout_t layout_textured;   /* textured vertex layout */
    bgfx_uniform_handle_t u_tex_color;      /* sampler uniform for textures */
    /* x = SDF smoothing half-width, y = the outline's distance value,
     * z = outline width. */
    bgfx_uniform_handle_t u_sdf_params;
    bgfx_uniform_handle_t u_sdf_outline;        /* outline rgba */
    bgfx_uniform_handle_t u_sdf_shadow_offset;  /* xy in texture space */
    bgfx_uniform_handle_t u_sdf_shadow_color;   /* rgba; .w 0 = disabled */
    bgfx_program_handle_t program_mesh;     /* mesh (pos+normal+uv) */
    /* PBR programs */
    bgfx_program_handle_t program_pbr;
    bgfx_program_handle_t program_pbr_inst;     /* GPU-instanced PBR */
    bgfx_program_handle_t program_pbr_inst_tint;/* instanced PBR + per-instance tint */
    bgfx_program_handle_t program_pbr_inst_tex_array;/* instanced PBR + per-instance albedo array layer */
    bgfx_program_handle_t program_pbr_inst_fade;    /* instanced PBR + LOD cross-fade dither (千万 ②) */
    bgfx_program_handle_t program_pbr_skinned;
    /* Forward+ clustered fragment variants (fs_pbr_fwdplus). */
    bgfx_program_handle_t program_pbr_fwdplus;
    bgfx_program_handle_t program_pbr_inst_fwdplus;
    bgfx_program_handle_t program_pbr_skinned_fwdplus;
    bgfx_program_handle_t program_pbr_toon;       /* skinned cel/rim (stylized §5.6) */
    bgfx_program_handle_t program_outline_skinned;/* inverted-hull silhouette */
    /* When true, jce_renderer_get_program_pbr* return the fwdplus variant
     * (if it loaded).  Set per-frame by the scene renderer from the
     * r.forwardplus cvar; default false => unchanged non-variant programs. */
    bool                  forwardplus_program_active;
    /* KEYWORD BITS THAT BELONG TO THE FRAME, not to any material: Forward+,
     * and whatever JCE_SHADER_FORCE_KEYS asks for.  Recomputed whenever
     * either changes, and OR'd into every pick. */
    uint32_t              frame_shader_keys;
    int                   force_shader_keys;   /* -1 = env not read yet */
    bgfx_program_handle_t program_shadow;
    bgfx_program_handle_t program_shadow_inst;     /* GPU-instanced shadow */
    bgfx_program_handle_t program_shadow_skinned;
    bgfx_program_handle_t program_terrain;
    /* THE GENERATED PBR PROGRAM TABLE, copied from the shader set.
     * [vertex variant][keyword bits]; jce_renderer_get_program_variant
     * is the only reader, and every named PBR getter above is a call
     * into it. */
    JceShaderHandle program_variant[JCE_SHADER_VARIANT_COUNT]
                                   [JCE_SHADER_KEY_COUNT];
    bgfx_uniform_handle_t u_light_dir;   /* vec4: xyz = light direction */
    bgfx_uniform_handle_t u_light_color; /* vec4: xyz = color, w = ambient */
    uint32_t reset_flags;
    JceRendererLaunchVsync launch_vsync;
    struct { char *root; bool bound; } shader_dev;
    uint32_t debug_flags;
    char gpu_name[128];
    uint16_t max_encoders;   /* effective bgfx encoder-pool cap (parallel submit) */
};

#include "jce_renderer_launch_policy.inc.h"
#include "jce_renderer_shader_dev.inc.h"
static bool s_dbg_text_enabled = false;

/* Raise bgfx's per-frame TRANSIENT buffer pool above the game-tuned default
 * (6 MiB VB / 2 MiB IB).  The pool is a single fixed-size ring SHARED every
 * frame by the scene renderer (GPU-driven instancing, debug-draw, primitives),
 * the sprite/decal batchers AND ImGui.  The editor's worst case is far heavier
 * than a shipped game's HUD: the World-Streaming Hierarchy can list 200+ chunk
 * groups with thousands of streamed-entity rows, and as cells churn (load /
 * unload while the camera traverses) that ImGui geometry — plus the streamed
 * scene's instancing records — momentarily blows past 6 MiB.  When the ring is
 * exhausted mid-frame bgfx's transient allocator clamps unevenly across its
 * consumers and a downstream copy overruns the ring, corrupting the heap (the
 * churn crash; ASAN caught a WRITE past the 6 MiB transient VB).  Sizing the
 * ring for the editor's peak avoids that exhaustion. Capacity reserves fixed
 * CPU/backend storage at initialization; per-frame usage is a separate metric. */
/* ── bgfx → engine allocator bridge ─────────────────────────────────
 * Routes ALL bgfx heap traffic (frame buffers, transient pools, resource
 * tables, bgfx_alloc blobs) through the engine allocator, so it is visible
 * to jce_mem_stats and reclaimable by the periodic jce_alloc_trim — bgfx
 * otherwise sits on the CRT heap, the largest unmanaged block in the
 * editor's memory attribution.  bgfx requires the allocator to be thread
 * safe (render thread + encoders); the engine allocator (mimalloc) is.
 * The interface must outlive bgfx, hence static.  JCE_NO_ALLOC_HOOKS=1
 * keeps bgfx on the CRT heap (escape hatch shared by all bridge users). */
static void *bgfx_reallocate_default(void *ptr, size_t size, size_t align, void *user)
{
    (void)user;
    return jce_realloc_aligned(ptr, size, align);
}

static void *bgfx_alloc_bridge(bgfx_allocator_interface_t *self, void *ptr,
                               size_t size, size_t align,
                               const char *file, uint32_t line)
{
    (void)self; (void)file; /* Never capture source paths, addresses or payload. */
    return jce_bgfx_trace_reallocate(&s_device.allocation_trace, bgfx_reallocate_default,
                                    NULL, ptr, size, align, line);
}
static const bgfx_allocator_vtbl_t s_jce_alloc_vtbl = { bgfx_alloc_bridge };
static bgfx_allocator_interface_t  s_jce_alloc_iface = { &s_jce_alloc_vtbl };

static bgfx_allocator_interface_t *jce_bgfx_allocator(void)
{
    static int s_no_hooks = -1;
    if (s_no_hooks < 0) {
        const char *v = getenv("JCE_NO_ALLOC_HOOKS");
        s_no_hooks = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    /* Each call precedes a new bgfx init attempt, after the old backend is
     * quiescent. Opt-in is independent of general trace/debug defaults. */
    jce_bgfx_trace_begin(&s_device.allocation_trace,
        jce_env_flag_enabled(getenv("JCE_BGFX_ALLOC_TRACE")) != 0, !s_no_hooks);
    return s_no_hooks ? NULL : &s_jce_alloc_iface;
}

/* Adapter policy remains compiled in this single renderer init TU. */
#include "jce_renderer_adapter.inc.h"

static void apply_transient_limits(bgfx_init_t *init)
{
    init->allocator = jce_bgfx_allocator();   /* NULL => bgfx CRT default */
    /* Startup pools belong to each bgfx Frame. Native multithreaded bgfx
     * keeps two frames: the default 8/2 MiB pair therefore reserves 20 MiB
     * of logical CPU transient storage before backend buffers are counted.
     * This is capacity, not a process RSS measurement. Preserve native
     * encoder headroom separately; larger geometry/UI workloads opt into
     * explicit pool budgets. Machine-class detection selects the smaller
     * 6/2 MiB path without changing authored material or asset quality.
     * GPU tier is unavailable before bgfx_init, so use RAM and core count. */
    const int ram_mb = SDL_GetSystemRAM();            /* 0 if unknown */
    const int cores  = SDL_GetNumLogicalCPUCores();   /* >= 1 */
    bool low_mem = (ram_mb > 0 && ram_mb < 2048) || cores <= 1;
    /* Settings S5: the user's jce.ini [performance] machine_class overrides the
     * RAM+core auto-detect (low = force small pools, full = force big). */
    switch (jce_config_machine_class()) {
    case JCE_MACHINE_CLASS_LOW:  low_mem = true;  break;
    case JCE_MACHINE_CLASS_FULL: low_mem = false; break;
    default: break;   /* AUTO: keep the detected value */
    }
    /* JCE_LOW_MEM=1 forces the small pools on a strong box (charter QA /
     * profiling); =0 forces the full pools on a weak one (escape hatch).
     * The env keeps final precedence over the config. */
    {
        const char *lm = getenv("JCE_LOW_MEM");
        if (lm && lm[0]) low_mem = (lm[0] != '0');
    }

    const char *vb_env = getenv("JCE_TRANSIENT_VB_MB");
    const char *ib_env = getenv("JCE_TRANSIENT_IB_MB");
    const uint32_t vb_override = jce_renderer_pool_mb(vb_env);
    const uint32_t ib_override = jce_renderer_pool_mb(ib_env);
    if (vb_env && vb_env[0] && !vb_override)
        LOG_WARN(LOG_TAG, "JCE_TRANSIENT_VB_MB must be 1..256; using default");
    if (ib_env && ib_env[0] && !ib_override)
        LOG_WARN(LOG_TAG, "JCE_TRANSIENT_IB_MB must be 1..256; using default");
    const JceRendererPoolBudget b = jce_renderer_pool_budget(
        JCE_PLATFORM_WEB != 0, low_mem, cores, vb_override, ib_override);
    init->limits.maxTransientVbSize = b.vertex_bytes;
    init->limits.maxTransientIbSize = b.index_bytes;
    init->limits.minUniformBufferSize = b.uniform_bytes;
    init->limits.maxEncoders = b.encoders;
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                       "renderer.transient_vb_capacity_bytes", b.vertex_bytes);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                       "renderer.transient_ib_capacity_bytes", b.index_bytes);
    /* Initial command capacity per encoder per bgfx Frame; backend growth
     * remains enabled. This counter is neither total heap bytes nor RSS. */
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                       "renderer.uniform_initial_encoder_frame_bytes", b.uniform_bytes);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                       "renderer.encoder_capacity", b.encoders);

}

/* ── Per-backend availability probe ──────────────────────────────── *
 *                                                                    *
 * Called before bgfx_init() to verify a backend is actually usable. *
 * Runs entirely before bgfx allocates any state, so a crash in the  *
 * probe is caught here (main thread, clean stack) and the fallback  *
 * loop can safely continue to the next backend.                     *
 * ─────────────────────────────────────────────────────────────────*/
/* Ask the GL/GLES driver what it actually is, BEFORE bgfx sees the window.
 *
 * WHY THIS IS NOT REDUNDANT WITH bgfx.  Measured 2026-09-01, on this bgfx:
 * BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION appears ZERO times in
 * renderer_gl.cpp; `m_gles3` is set from a compile-time constant off
 * Emscripten rather than from the device; and BGFX_RENDERER_OPENGL_NAME --
 * the "OpenGL 3.3" that shows up in our own startup log -- is a compile-time
 * string built from the floor, not a reading.  bgfx_init() therefore SUCCEEDS
 * on a context it cannot serve, and the failure arrives later as a shader that
 * would not compile.  Nothing upstream is going to tell us; we have to look.
 *
 * WHY A THROWAWAY WINDOW.  Creating a GL context on the real window and then
 * choosing D3D or Vulkan would leave that window with a GL pixel format it
 * cannot take back on Windows.  A hidden 1x1 window costs a few milliseconds
 * once and cannot poison anything.
 *
 * A probe that fails to run is NOT a failed probe: it publishes nothing, the
 * version stays unverified, and the backend is tried anyway. */
static bool s_probe_gl_version(bgfx_renderer_type_t type)
{
#if JCE_PLATFORM_WEB
    /* SDL Web has one canvas. A second hidden probe window reinitializes
     * browser drop state and throws EEXIST instead of returning an SDL error.
     * Verify the actual bgfx WebGL context after initialization instead. */
    (void)type;
    return true;
#else
    const bool gles = (type == BGFX_RENDERER_TYPE_OPENGLES);
    const JceRendererBackend backend =
        gles ? JCE_BACKEND_OPENGLES : JCE_BACKEND_OPENGL;
    JceNativeGlGetStringFn get_string = NULL;
    SDL_Window    *win = NULL;
    SDL_GLContext  ctx = NULL;
    const char    *api = NULL, *shading = NULL;
    bool           ok  = true;

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        LOG_INFO(LOG_TAG, "%s probe: SDL video is not up, skipping",
                 jce_renderer_backend_name(backend));
        return true;
    }
    if (!SDL_GL_LoadLibrary(NULL)) {
        LOG_INFO(LOG_TAG, "%s probe: no GL library on this system (%s)",
                 jce_renderer_backend_name(backend), SDL_GetError());
        return false;
    }

    /* Ask for EXACTLY what bgfx is about to ask for, or this measures the
     * wrong context.  glcontext_wgl.cpp requests
     *   MAJOR = BGFX_CONFIG_RENDERER_OPENGL / 10, MINOR = % 10, CORE profile
     * when the floor is >= 31, and a plain 2.1 otherwise -- and the floor is
     * published to us as JCE_BGFX_OPENGL_VERSION.  A compatibility context
     * with no version requested (what this probe asked for first) reports the
     * driver's maximum, which is a different question: it says what the card
     * COULD do, not what bgfx will be handed.
     *
     * Getting this wrong is not academic.  bgfx retries once without the
     * profile mask and then BGFX_FATALs, so "3.3 core refused" is a hard stop
     * -- exactly the case this probe exists to catch before it happens. */
    if (gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,
                            JCE_BGFX_OPENGLES_VERSION / 10);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,
                            JCE_BGFX_OPENGLES_VERSION % 10);
    } else if (JCE_BGFX_OPENGL_VERSION >= 31) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,
                            JCE_BGFX_OPENGL_VERSION / 10);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,
                            JCE_BGFX_OPENGL_VERSION % 10);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    }
    win = SDL_CreateWindow("jce-gl-probe", 1, 1,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (win)
        ctx = SDL_GL_CreateContext(win);

    if (ctx && SDL_GL_MakeCurrent(win, ctx)) {
        /* Query only; the private native type preserves the GL entry ABI. */
        get_string = (JceNativeGlGetStringFn)SDL_GL_GetProcAddress("glGetString");
        if (get_string) {
            api     = (const char *)get_string(0x1F02u);
            shading = (const char *)get_string(0x8B8Cu);
        }
    }

    if (api) {
        LOG_INFO(LOG_TAG, "%s probe: driver reports \"%s\" / shading \"%s\"",
                 jce_renderer_backend_name(backend), api,
                 shading ? shading : "(none)");
        ok = jce_renderer_caps_api_publish_probe(backend, api, shading);
        if (!ok) {
            JceRendererApiInfo info = jce_renderer_get_api_info();
            LOG_WARN(LOG_TAG,
                     "%s probe: %u.%u is below the %s tier floor of %u.%u; "
                     "skipping this backend and trying the next one",
                     jce_renderer_backend_name(backend),
                     (unsigned)info.runtime_version.major,
                     (unsigned)info.runtime_version.minor,
                     jce_graphics_api_tier_name(info.build_tier),
                     (unsigned)info.minimum_version.major,
                     (unsigned)info.minimum_version.minor);
        }
    } else {
        LOG_WARN(LOG_TAG,
                 "%s probe: could not create a probe context (%s); trying the "
                 "backend without a floor check",
                 jce_renderer_backend_name(backend), SDL_GetError());
    }

    if (ctx) SDL_GL_DestroyContext(ctx);
    if (win) SDL_DestroyWindow(win);
    return ok;
#endif /* single browser canvas / native probe window */
}

#if JCE_PLATFORM_WINDOWS

static bool backend_probe(bgfx_renderer_type_t type)
{
#if defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
    if (!jce_renderer_caps_xp_backend_available(type)) return false;
    return type != BGFX_RENDERER_TYPE_OPENGL || s_probe_gl_version(type);
#else
    switch (type) {
    case BGFX_RENDERER_TYPE_VULKAN:
        /* Loader must export its entry point, not merely map — a stub
         * vulkan-1.dll without a real ICD behind it is reported unusable. */
        return jce_library_has_symbol("vulkan-1.dll", "vkGetInstanceProcAddr");
    case BGFX_RENDERER_TYPE_DIRECT3D12:
        return jce_library_exists("d3d12.dll");
    case BGFX_RENDERER_TYPE_DIRECT3D11:
        return jce_library_exists("d3d11.dll");
    case BGFX_RENDERER_TYPE_OPENGL:
    case BGFX_RENDERER_TYPE_OPENGLES:
        return s_probe_gl_version(type);
    default:
        return true;
    }
#endif
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
     * (jce_library_open/jce_library_symbol through the platform layer).
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
    if (type == BGFX_RENDERER_TYPE_OPENGL || type == BGFX_RENDERER_TYPE_OPENGLES)
        return s_probe_gl_version(type);
    return true;
}

#endif /* JCE_PLATFORM_WINDOWS / POSIX */

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

static JceRendererBackend from_bgfx_type(bgfx_renderer_type_t type)
{
    switch (type) {
    case BGFX_RENDERER_TYPE_DIRECT3D11: return JCE_BACKEND_D3D11;
    case BGFX_RENDERER_TYPE_DIRECT3D12: return JCE_BACKEND_D3D12;
    case BGFX_RENDERER_TYPE_VULKAN:     return JCE_BACKEND_VULKAN;
    case BGFX_RENDERER_TYPE_OPENGL:     return JCE_BACKEND_OPENGL;
    case BGFX_RENDERER_TYPE_OPENGLES:   return JCE_BACKEND_OPENGLES;
    case BGFX_RENDERER_TYPE_METAL:      return JCE_BACKEND_METAL;
    case BGFX_RENDERER_TYPE_NOOP:       return JCE_BACKEND_NOOP;
    default:                            return JCE_BACKEND_AUTO;
    }
}

/* The app window, so the GL context ladder below can build on the real one.
 * Set by jce_renderer_create() before any attempt; NULL when headless. */
static SDL_Window   *s_gl_ladder_window  = NULL;
static SDL_GLContext s_gl_ladder_context = NULL;
static int           s_gl_ladder_version = 0;

/* Run desktop OpenGL at the highest core version this driver grants, instead
 * of exactly the version the build was compiled for.
 *
 * WHY THIS IS NEEDED, measured 2026-09-01 and contrary to the obvious guess:
 * asking for a 3.3 CORE context on a card that reports 4.6 in a compatibility
 * context yields exactly "3.3.0 NVIDIA".  Core profiles are honoured
 * literally.  bgfx asks for BGFX_CONFIG_RENDERER_OPENGL and nothing else, so
 * the build floor was not a minimum -- it was the version we ran at, on every
 * machine, forever.  This driver grants 4.6, 4.3, 3.3 and 3.1; we were taking
 * 3.3.
 *
 * WHY IT IS SAFE.  bgfx writes `#version 140` for any floor >= 31 and 1.40 is
 * valid from GL 3.1 through 4.6 core, so the fixed shader dialect does not
 * pin the context.  bgfx also accepts a context we create -- glcontext_wgl.cpp
 * skips its own creation when platformData.context is set, and its destroy()
 * explicitly does NOT delete a context it did not make, so ownership stays
 * here.
 *
 * WHY IT ONLY LADDERS UP.  59 entries in bgfx's extension table are seeded
 * "assume core at BGFX_CONFIG_RENDERER_OPENGL >= N".  Those defaults are
 * compiled for the FLOOR, so handing bgfx a context BELOW it would have it
 * assume entry points that are not there.  Above the floor the seeds are
 * merely conservative and the runtime GL_EXTENSIONS scan fills the rest in. */
static void gl_ladder_acquire(bgfx_platform_data_t *pd)
{
    static const int kRungs[] = {46, 45, 44, 43, 42, 41, 40, 33, 32, 31};

    if (!s_gl_ladder_window || JCE_BGFX_OPENGL_VERSION < 31)
        return;                       /* legacy floor: bgfx's own 2.1 path */
    if (s_gl_ladder_context) {        /* a previous attempt already built one */
        pd->context = s_gl_ladder_context;
        return;
    }

    for (size_t i = 0; i < sizeof(kRungs) / sizeof(kRungs[0]); ++i) {
        SDL_GLContext c;
        if (kRungs[i] < JCE_BGFX_OPENGL_VERSION)
            break;                    /* never below the floor -- see above */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kRungs[i] / 10);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kRungs[i] % 10);
        c = SDL_GL_CreateContext(s_gl_ladder_window);
        if (!c)
            continue;
        /* Report the version this context ACTUALLY is, not the floor.  The
         * throwaway probe measures a floor-matching context by design, so
         * without this the log and the editor would say 3.3 while the engine
         * ran at 4.6. */
        {
            JceNativeGlGetStringFn gs =
                (JceNativeGlGetStringFn)SDL_GL_GetProcAddress("glGetString");
            if (gs)
                jce_renderer_caps_api_publish_probe(
                    JCE_BACKEND_OPENGL, (const char *)gs(0x1F02u),
                    (const char *)gs(0x8B8Cu));
        }

        /* Hand it over UNBOUND.  SDL_GL_CreateContext makes the context
         * current on the calling thread, and a WGL context can be current on
         * exactly one thread at a time -- bgfx's render thread then fails
         * wglMakeCurrent and every wglGetProcAddress after it, which surfaces
         * as ~45 "Failed to create OpenGL context. wglGetProcAddress(...)"
         * fatals that look nothing like the cause.  Measured. */
        SDL_GL_MakeCurrent(s_gl_ladder_window, NULL);
        s_gl_ladder_context = c;
        s_gl_ladder_version = kRungs[i];
        pd->context = c;
        LOG_INFO(LOG_TAG,
                 "OpenGL: running at %d.%d core, the highest this driver "
                 "grants at or above the %d.%d build floor",
                 kRungs[i] / 10, kRungs[i] % 10,
                 JCE_BGFX_OPENGL_VERSION / 10, JCE_BGFX_OPENGL_VERSION % 10);
        return;
    }
    LOG_INFO(LOG_TAG, "OpenGL: no core context at or above the %d.%d floor; "
             "letting bgfx create its own",
             JCE_BGFX_OPENGL_VERSION / 10, JCE_BGFX_OPENGL_VERSION % 10);
}

static void gl_ladder_release(void)
{
    if (s_gl_ladder_context) {
        SDL_GL_DestroyContext(s_gl_ladder_context);
        s_gl_ladder_context = NULL;
        s_gl_ladder_version = 0;
    }
}

static bool init_backend_attempt(bgfx_renderer_type_t type,
                                 const bgfx_platform_data_t *pd,
                                 uint32_t width, uint32_t height,
                                 uint32_t reset_flags, bool debug,
                                 JceWindow *window, JceRendererDisplayAdapterState *display_adapter,
                                 JceRendererLaunchVsync *presentation, uint32_t *effective_reset_flags,
                                 const JceRendererLaunchDiagnostics *launch)
{
    if (launch && launch->invalid) { renderer_launch_diagnostics_observe(launch, NULL, 0); return false; }
    bgfx_init_t init;
    JceRendererApiInfo api;
    JceRendererLaunchVsync candidate = {0}; candidate.requested = (reset_flags & BGFX_RESET_VSYNC) != 0;

retry_adapter:
    jce_renderer_caps_api_begin_attempt(from_bgfx_type(type));
    bgfx_init_ctor(&init);
    init.type              = type;
#if JCE_PLATFORM_WINDOWS && defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
    /* The JCE ladder, not the provider, admits each packaged candidate. */
    init.fallback          = false;
#endif
    apply_adapter_preference(&init);
    (void)jce_renderer_apply_display_adapter(&init,window,display_adapter,jce_window_observe_display_adapter,jce_window_observe_performance_adapter);
    apply_transient_limits(&init);
    init.resolution.width  = width;
    init.resolution.height = height;
    init.resolution.reset  = renderer_presentation_vsync_refresh(&candidate, type, window, reset_flags);
    renderer_launch_diagnostics_apply(launch, &init);
    init.platformData      = *pd;
    /* Desktop GL only: swap in a context at the highest core version this
     * driver grants at or above the floor.  A no-op for every other backend
     * and for a legacy (< 3.1) floor. */
    if (type == BGFX_RENDERER_TYPE_OPENGL)
        gl_ladder_acquire(&init.platformData);
    init.callback          = jce_rcb_callback_interface();
    init.debug             = debug;

    if (!bgfx_init(&init)) {
        renderer_launch_diagnostics_observe(launch, &init, 1);
        /* bgfx init(false) deletes its Context, joining any owned render thread,
         * before returning. There is no admitted backend to shut down here. */
        jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_INIT_FAILED);
        jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_INIT_FAILED);
        /* Our context is useless to any other backend, and leaving it
         * current would have the next attempt inherit it. */
        gl_ladder_release();
        goto retry_or_fail;
    }
    const char *backend_refusal = NULL;
#if JCE_PLATFORM_WINDOWS && defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
    if (bgfx_get_renderer_type() != type) {
        backend_refusal = "provider_selected_unrequested_backend";
    } else if (type == BGFX_RENDERER_TYPE_DIRECT3D11) {
        const bgfx_caps_t *caps = bgfx_get_caps();
        /* In the pinned D3D11 provider this bit means feature level >= 11_0.
         * The embedded D3D stock was compiled for Shader Model 5. */
        if (!caps || !(caps->supported & BGFX_CAPS_PRIMITIVE_ID))
            backend_refusal = "d3d11_shader_model_5_unavailable";
        else {
            const bgfx_internal_data_t *data = bgfx_get_internal_data();
            if (data && jce_window_legacy_d3d11_is_software(data->context))
                backend_refusal = "d3d11_software_device";
        }
    }
#endif
    /* Post-init Web API admission uses that exact active context. */
    const bool web_api_ok = from_bgfx_type(type) != JCE_BACKEND_OPENGLES ||
        jce_renderer_caps_api_admit_web_context();
    if (!backend_refusal && web_api_ok && jce_renderer_caps_api_accept_active_backend()) {
        s_device.frame_index = 0;
        s_device.offscreen_submission_index = 0;
        if (presentation) *presentation = candidate;
        if (effective_reset_flags) *effective_reset_flags = init.resolution.reset;
        renderer_launch_diagnostics_observe(launch, &init, 2);
        return true;
    }

    api = jce_renderer_get_api_info();
    if (backend_refusal) {
        LOG_WARN(LOG_TAG, "backend %s refused: %s; trying the next backend",
                 bgfx_get_renderer_name(type), backend_refusal);
    } else if (api.runtime_version_verified) {
        LOG_WARN(LOG_TAG,
                 "%s negotiated API %u.%u.%u below %s floor %u.%u.%u; "
                 "trying the next backend",
                 jce_renderer_backend_name(api.backend),
                 (unsigned)api.runtime_version.major,
                 (unsigned)api.runtime_version.minor,
                 (unsigned)api.runtime_version.patch,
                 jce_graphics_api_tier_name(api.build_tier),
                 (unsigned)api.minimum_version.major,
                 (unsigned)api.minimum_version.minor,
                 (unsigned)api.minimum_version.patch);
    } else {
        LOG_WARN(LOG_TAG,
                 "%s did not report its negotiated API version; %s tier "
                 "cannot be verified, trying the next backend",
                 jce_renderer_backend_name(api.backend),
                 jce_graphics_api_tier_name(api.build_tier));
    }
    renderer_launch_diagnostics_observe(launch, &init, 3);
    JCE_TRACE_SCOPE_CALL("renderer.backend_shutdown_cpu", bgfx_shutdown());
    jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_API_REFUSED);
    jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    /* Same reason as the bgfx_init-failed path above, and it was missing here:
     * bgfx does not destroy a context it did not create, so returning without
     * this leaks the laddered context AND leaves s_gl_ladder_context set --
     * which makes gl_ladder_acquire() hand the SAME context to a later
     * jce_renderer_create(), after the bgfx instance it was given to has been
     * shut down.  Order matters: bgfx first, then the context it was using. */
    gl_ladder_release();
    jce_renderer_caps_api_reset();

retry_or_fail:
    if (display_adapter && display_adapter->performance_applied) {
        /* The preferred device is optional. Retire the failed attempt first,
         * then try the same backend once with the legacy adapter policy. */
        display_adapter->performance_blocked = true;
        goto retry_adapter;
    }
    return false;
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

/* JCE_BACKEND=auto|d3d11|d3d12|vulkan|opengl|gles|metal|noop — overrides the
 * caller-selected backend (same diagnostic env family as JCE_FORCE_FALLBACK).
 * Primary consumer: tools/render_parity.py, which boots the SAME binary on
 * several backends and pixel-compares the output to catch backend-divergent
 * shader/render behavior (the class of bug where a raw mat3 ctor flipped TBN
 * on GLSL only) without touching any per-user config.
 *
 * Resolved BEFORE the native-window handle is acquired so the windowless NOOP
 * backend is reachable (dependency audit P0 `headless-forced-window-init`). */
static int resolve_backend_choice(int cfg_backend)
{
    int backend_choice = cfg_backend;
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
        if (backend_choice != cfg_backend)
            LOG_WARN(LOG_TAG,
                "DEBUG TOGGLE: JCE_BACKEND=%s -> backend override", bv);
    }
    return backend_choice;
}

JceRenderer *jce_renderer_create_impl(JceWindow *win,
                                  const JceRendererConfig *cfg)
{
    if (!win || !cfg) return NULL;
    const JceRendererLaunchDiagnostics launch = renderer_launch_diagnostics_snapshot();
    if (launch.invalid) { renderer_launch_diagnostics_observe(&launch, NULL, 0); return NULL; }

    /* The ladder builds its context on the real window, so publish it before
     * any backend attempt runs. */
    s_gl_ladder_window = jce_window_sdl(win);

    /* JCE_FORCE_FALLBACK: any nonempty value not beginning with '0' skips
     * bgfx initialization so the application can select its software fallback. */
    {
        const char *force = getenv("JCE_FORCE_FALLBACK");
        if (force && force[0] && force[0] != '0') {
            LOG_WARN(LOG_TAG,
                "JCE_FORCE_FALLBACK=%s set — skipping bgfx init, "
                "engine will use SDL software renderer", force);
            renderer_launch_diagnostics_observe(&launch, NULL, 5); return NULL;
        }
    }

    /* Resolve the backend BEFORE touching the window.  The NOOP backend needs
     * no window at all, so demanding a native handle first made it
     * unreachable on a display-less host (dependency audit P0
     * `headless-forced-window-init`).  The dedicated headless entry point is
     * jce_renderer_create_headless(); this keeps the windowed entry honest
     * when a caller forces JCE_BACKEND=noop. */
    int backend_choice = resolve_backend_choice(cfg->backend);
    const bool want_noop = (backend_choice == JCE_BACKEND_NOOP);

    bgfx_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));

    if (!want_noop) {
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

        pd.nwh = nw.nwh;
        pd.ndt = nw.ndt;
        bgfx_set_platform_data(&pd);
    } else {
        LOG_WARN(LOG_TAG, "JCE_BACKEND=noop — initialising the null backend "
                 "without a native window handle");
    }

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

    bgfx_renderer_type_t requested_type = map_backend(backend_choice);
    const char *backend_name =
        requested_type == BGFX_RENDERER_TYPE_COUNT
            ? "auto"
            : bgfx_get_renderer_name(requested_type);
    LOG_INFO(LOG_TAG,
        "init request: backend=%s nwh=%p ndt=%p size=%ux%u",
        backend_name, pd.nwh, pd.ndt, w, h);

    /* macOS CAMetalLayer creation calls the main thread via GCD: pre-init
     * render_frame selects single-threaded bgfx to avoid that init deadlock.
     * iOS UIKit permits its render thread and keeps the default mode. */
#if JCE_PLATFORM_MACOS
    bgfx_render_frame(-1);
#endif

    /* Try to initialise bgfx.  When the user picked a specific backend we
     * attempt that first; on failure (or AUTO) we walk the platform-specific
     * preferred list until one succeeds. */
    bool ok = false;
    JceRendererDisplayAdapterState display_adapter = {0};
    JceRendererLaunchVsync presentation = {0};

    if (requested_type != BGFX_RENDERER_TYPE_COUNT) {
        if (!backend_probe(requested_type)) {
            LOG_WARN(LOG_TAG, "requested backend %s probe failed — trying fallback chain",
                     bgfx_get_renderer_name(requested_type));
        } else {
            ok = init_backend_attempt(requested_type, &pd, w, h,
                                      reset_flags, gfx_debug, win, &display_adapter, &presentation, &reset_flags, &launch);
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
            if (init_backend_attempt(chain[i], &pd, w, h,
                                     reset_flags, gfx_debug, win, &display_adapter, &presentation, &reset_flags, &launch)) {
                ok = true;
                break;
            }
        }
    }

    if (!ok) {
        /* Every attempt failed.  A GL attempt in the chain may have left its
         * laddered context behind; nothing else will free it on this path. */
        log_display_adapter(win,&display_adapter,NULL);
        gl_ladder_release();
        jce_renderer_caps_api_reset();
        LOG_ERROR(LOG_TAG, "bgfx_init failed - all backends exhausted "
                  "(nwh=%p, w=%u, h=%u)", pd.nwh, w, h);
        /* 
         * We do NOT show a messagebox here, as the engine will handle
         * falling back to the SDL renderer or show an error screen later.
         */
        return NULL;
    }

    {
        JceRendererApiInfo api = jce_renderer_get_api_info();
        if (api.runtime_version_verified) {
            jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                     "renderer: %s API %u.%u.%u (%s floor %u.%u.%u)",
                     bgfx_get_renderer_name(bgfx_get_renderer_type()),
                     (unsigned)api.runtime_version.major,
                     (unsigned)api.runtime_version.minor,
                     (unsigned)api.runtime_version.patch,
                     jce_graphics_api_tier_name(api.build_tier),
                     (unsigned)api.minimum_version.major,
                     (unsigned)api.minimum_version.minor,
                     (unsigned)api.minimum_version.patch);
        } else {
            jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                     "renderer: %s (graphics tier %s)",
                     bgfx_get_renderer_name(bgfx_get_renderer_type()),
                     jce_graphics_api_tier_name(api.build_tier));
        }
    }
    log_gpu_adapters();
    log_display_adapter(win,&display_adapter,bgfx_get_caps());

    /* bgfx's built-in debug text is an ALLOWLIST, not a denylist.
     *
     * It was a denylist with exactly one entry -- OpenGL -- and Vulkan then
     * crashed the render thread on the first submitted frame:
     *
     *     bgfx::vk::RendererContextVK::dbgTextRenderBegin
     *     bgfx::dbgTextSubmit
     *     bgfx::vk::RendererContextVK::submit
     *     bgfx::Context::renderFrame
     *
     * -- i.e. the editor could not start at all on Vulkan, on a machine where
     * D3D11, D3D12 and GL all run. A denylist makes that the DEFAULT outcome
     * for every backend nobody has tried yet: the untested case is the one
     * that gets the feature.
     *
     * Inverting it costs nothing that matters. Debug text is a development
     * overlay, and a backend that does not draw it is missing an overlay,
     * while a backend that crashes on it is missing everything. */
    bool enable_debug_text = cfg->debug_text;
    if (enable_debug_text) {
        const bgfx_renderer_type_t rt = bgfx_get_renderer_type();
        const bool known_good = (rt == BGFX_RENDERER_TYPE_DIRECT3D11 ||
                                 rt == BGFX_RENDERER_TYPE_DIRECT3D12);
        if (!known_good) {
            LOG_INFO(LOG_TAG,
                     "disabling bgfx debug text on the %s backend "
                     "(allowlisted on D3D11/D3D12 only)",
                     jce_renderer_running_backend_name());
            enable_debug_text = false;
        }
    }

    uint32_t debug_flags = enable_debug_text ? BGFX_DEBUG_TEXT : 0;
#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)
    debug_flags |= BGFX_DEBUG_PROFILER;
    LOG_INFO(LOG_TAG, "bgfx GPU profiler enabled (Tracy mode)");
#endif
    /* JCE_PERF_LOG requests optional per-view GPU timers through the bgfx
     * profiler bit. Preserve the shared nonempty, nonzero-prefix flag policy;
     * an empty value or a value starting with 0 must not enable these queries. */
    if (jce_env_flag_enabled(getenv("JCE_PERF_LOG"))) {
        debug_flags |= BGFX_DEBUG_PROFILER;
        LOG_INFO(LOG_TAG, "bgfx GPU profiler enabled (JCE_PERF_LOG per-view timing)");
    }
    if (debug_flags)
        bgfx_set_debug(debug_flags);

    /* View 0 (3D): clear color + depth. */
    bgfx_set_view_clear(JCE_VIEW_MAIN_3D,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL,
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
        JCE_TRACE_SCOPE_CALL("renderer.backend_shutdown_cpu", bgfx_shutdown());
        jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_ALLOCATION_FAILED);
        jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_JOINED);
        gl_ladder_release();
        jce_renderer_caps_api_reset();
        return NULL;
    }
    r->reset_flags = reset_flags;
    r->launch_vsync = presentation;
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
    /* THE REAL path as well as the NOOP one below.  A uniform created in only
     * one of the two leaves the other holding a zero handle -- which is a
     * VALID handle belonging to some other uniform, so the write lands
     * somewhere unrelated instead of failing. */
    r->u_sdf_params = bgfx_create_uniform(
        "u_sdfParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_sdf_outline = bgfx_create_uniform(
        "u_sdfOutline", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_sdf_shadow_offset = bgfx_create_uniform(
        "u_sdfShadowOffset", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_sdf_shadow_color = bgfx_create_uniform(
        "u_sdfShadowColor", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_dir = bgfx_create_uniform(
        "u_lightDir", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_color = bgfx_create_uniform(
        "u_lightColor", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Shader programs default to invalid; call
       jce_renderer_set_shaders() after creation. */
    r->program.idx              = UINT16_MAX;
    r->program_textured.idx     = UINT16_MAX;
    r->program_text_sdf.idx     = UINT16_MAX;
    r->program_mesh.idx         = UINT16_MAX;
    r->program_pbr.idx          = UINT16_MAX;
    r->program_pbr_inst.idx     = UINT16_MAX;
    r->program_pbr_inst_tint.idx = UINT16_MAX;
    r->program_pbr_inst_tex_array.idx = UINT16_MAX;
    r->program_pbr_inst_fade.idx      = UINT16_MAX;
    r->program_pbr_skinned.idx  = UINT16_MAX;
    r->program_pbr_fwdplus.idx         = UINT16_MAX;
    r->program_pbr_inst_fwdplus.idx    = UINT16_MAX;
    r->program_pbr_skinned_fwdplus.idx = UINT16_MAX;
    r->program_pbr_toon.idx        = UINT16_MAX;
    r->program_outline_skinned.idx = UINT16_MAX;
    r->program_shadow.idx       = UINT16_MAX;
    r->program_shadow_inst.idx  = UINT16_MAX;
    r->program_shadow_skinned.idx = UINT16_MAX;
    r->program_terrain.idx        = UINT16_MAX;
    r->frame_shader_keys          = 0u;
    r->force_shader_keys          = -1;
    for (int _v = 0; _v < JCE_SHADER_VARIANT_COUNT; _v++)
        for (int _k = 0; _k < JCE_SHADER_KEY_COUNT; _k++)
            r->program_variant[_v][_k].idx = UINT16_MAX;

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
                 vendor, jce_renderer_running_backend_name());
        /* Effective encoder-pool cap (after caps clamp) — the ceiling for the
         * opt-in parallel command-submission path (JCE_PARALLEL_SUBMIT). */
        r->max_encoders = caps->limits.maxEncoders;
        LOG_INFO(LOG_TAG, "encoder pool: maxEncoders=%u",
                 (unsigned)r->max_encoders);
    }

    LOG_SUCCESS(LOG_TAG, "initialized (%s)", r->gpu_name);
    /* JCE_DBG_BGFX_REPRO=1: minimal standalone repro of the benchmark exit
     * crash — create the exact GPU-cull buffer trio (big RW compute VB +
     * tiny RW compute counter VB + indirect buffer), never use or destroy
     * them, and exit.  Crash here == bgfx-internal, zero engine involvement. */
    if (getenv("JCE_DBG_BGFX_REPRO") && getenv("JCE_DBG_BGFX_REPRO")[0] == '1') {
        bgfx_vertex_layout_t vl4, vl1;
        bgfx_vertex_layout_begin(&vl4, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&vl4, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vl4, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vl4, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vl4, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&vl4);
        bgfx_vertex_layout_begin(&vl1, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&vl1, BGFX_ATTRIB_TEXCOORD0, 1, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&vl1);
        bgfx_dynamic_vertex_buffer_handle_t vis = bgfx_create_dynamic_vertex_buffer(
            2048, &vl4, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X4 | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
        bgfx_dynamic_vertex_buffer_handle_t cnt = bgfx_create_dynamic_vertex_buffer(
            64, &vl1, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X1 | BGFX_BUFFER_COMPUTE_TYPE_UINT);
        bgfx_indirect_buffer_handle_t ind = bgfx_create_indirect_buffer(1);
        LOG_INFO(LOG_TAG, "[bgfx-repro] trio created: vis=%u cnt=%u ind=%u",
                 vis.idx, cnt.idx, ind.idx);
    }

    s_device.owner = r; jce_upload_metrics_bind(&s_device.payload, &r->submitted_payload);
    jce_rcb_backend_started();
    jce_render_readback_backend_open(r);
    return r;
}
/* NullRHI: a bgfx NOOP renderer with NO window / platform data, for the
 * dedicated-server / headless boot (cf. Unreal's NullRHI, Unity's headless
 * graphics device).  bgfx NOOP never touches a GPU or a display, so this
 * boots on a display-less host.  Render entry points early-out on r->headless;
 * resource/scene calls that create bgfx handles remain valid no-ops. */
JceRenderer *jce_renderer_create_headless_impl(void)
{
    const JceRendererLaunchDiagnostics launch = renderer_launch_diagnostics_snapshot();
    renderer_launch_diagnostics_observe(&launch, NULL, 4);
    /* macOS single-thread guard mirrors the windowed path (harmless for NOOP). */
#if JCE_PLATFORM_MACOS
    bgfx_render_frame(-1);
#endif

    bgfx_init_t init;
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_NOOP);
    bgfx_init_ctor(&init);
    apply_adapter_preference(&init);
    init.allocator = jce_bgfx_allocator();
    init.type              = BGFX_RENDERER_TYPE_NOOP;
    init.resolution.width  = 1;
    init.resolution.height = 1;
    init.resolution.reset  = BGFX_RESET_NONE;
    /* NOOP has no window/capture capability, but diagnostics must use the
     * same process-owned callback as the windowed backend. */
    init.callback = jce_rcb_callback_interface();
    if (!bgfx_init(&init)) {
        jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_INIT_FAILED);
        jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_INIT_FAILED);
        jce_renderer_caps_api_reset();
        LOG_ERROR(LOG_TAG, "headless bgfx NOOP init failed");
        return NULL;
    }
    if (!jce_renderer_caps_api_accept_active_backend()) {
        JCE_TRACE_SCOPE_CALL("renderer.backend_shutdown_cpu", bgfx_shutdown());
        jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_API_REFUSED);
        jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_JOINED);
        jce_renderer_caps_api_reset();
        return NULL;
    }

    s_device.frame_index = 0;
    s_device.offscreen_submission_index = 0;
    JceRenderer *r = (JceRenderer *)JCE_CALLOC(1, sizeof(*r));
    if (!r) {
        JCE_TRACE_SCOPE_CALL("renderer.backend_shutdown_cpu", bgfx_shutdown());
        jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_ALLOCATION_FAILED);
        jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_JOINED);
        gl_ladder_release();
        jce_renderer_caps_api_reset();
        return NULL;
    }
    r->headless    = true;
    r->reset_flags = BGFX_RESET_NONE;

    /* Vertex layouts + uniforms: valid NOOP handles so any resource/scene code
     * that queries them stays well-formed. */
    bgfx_vertex_layout_begin(&r->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&r->layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout, BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&r->layout);

    bgfx_vertex_layout_begin(&r->layout_textured, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&r->layout_textured, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout_textured, BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_add(&r->layout_textured, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&r->layout_textured);

    r->u_tex_color   = bgfx_create_uniform("s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    r->u_sdf_params  = bgfx_create_uniform("u_sdfParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_sdf_outline = bgfx_create_uniform("u_sdfOutline", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_sdf_shadow_offset =
        bgfx_create_uniform("u_sdfShadowOffset", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_sdf_shadow_color =
        bgfx_create_uniform("u_sdfShadowColor", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_dir   = bgfx_create_uniform("u_lightDir", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_color = bgfx_create_uniform("u_lightColor", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* All shader programs start invalid (never set — headless never draws). */
    r->program.idx                    = UINT16_MAX;
    r->program_textured.idx           = UINT16_MAX;
    r->program_text_sdf.idx           = UINT16_MAX;
    r->program_mesh.idx               = UINT16_MAX;
    r->program_pbr.idx                = UINT16_MAX;
    r->program_pbr_inst.idx           = UINT16_MAX;
    r->program_pbr_inst_tint.idx      = UINT16_MAX;
    r->program_pbr_inst_tex_array.idx = UINT16_MAX;
    r->program_pbr_inst_fade.idx      = UINT16_MAX;
    r->program_pbr_skinned.idx        = UINT16_MAX;
    r->program_pbr_fwdplus.idx        = UINT16_MAX;
    r->program_pbr_inst_fwdplus.idx   = UINT16_MAX;
    r->program_pbr_skinned_fwdplus.idx = UINT16_MAX;
    r->program_pbr_toon.idx           = UINT16_MAX;
    r->program_outline_skinned.idx    = UINT16_MAX;
    r->program_shadow.idx             = UINT16_MAX;
    r->program_shadow_inst.idx        = UINT16_MAX;
    r->program_shadow_skinned.idx     = UINT16_MAX;
    r->program_terrain.idx            = UINT16_MAX;
    for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
        for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
            r->program_variant[v][k].idx = UINT16_MAX;

    SDL_strlcpy(r->gpu_name, "Null / Noop (headless)", sizeof(r->gpu_name));
    LOG_SUCCESS(LOG_TAG, "headless renderer initialized (NullRHI: bgfx NOOP, no window)");
    s_device.owner = r; jce_upload_metrics_bind(&s_device.payload, &r->submitted_payload);
    jce_render_readback_backend_open(r);
    return r;
}

void jce_renderer_set_shaders(JceRenderer *r,
                              const JceShaderSet *shaders)
{
    if (!r || !shaders || r->is_fallback || r->headless) return;

    r->program = (bgfx_program_handle_t){
        shaders->color.idx };
    r->program_textured = (bgfx_program_handle_t){
        shaders->textured.idx };
    /* Optional: a pak built before fs_text_sdf existed leaves this invalid,
     * and the text renderer keeps every font on the bitmap path rather than
     * drawing nothing. */
    r->program_text_sdf = (bgfx_program_handle_t){
        shaders->text_sdf.idx };
    r->program_mesh = (bgfx_program_handle_t){
        shaders->mesh.idx };
    r->program_pbr = (bgfx_program_handle_t){ shaders->pbr.idx };
    r->program_pbr_inst = (bgfx_program_handle_t){ shaders->pbr_inst.idx };
    r->program_pbr_inst_tint = (bgfx_program_handle_t){ shaders->pbr_inst_tint.idx };
    r->program_pbr_inst_tex_array = (bgfx_program_handle_t){ shaders->pbr_inst_tex_array.idx };
    r->program_pbr_inst_fade      = (bgfx_program_handle_t){ shaders->pbr_inst_fade.idx };
    r->program_pbr_skinned = (bgfx_program_handle_t){ shaders->pbr_skinned.idx };
    r->program_pbr_fwdplus = (bgfx_program_handle_t){ shaders->pbr_fwdplus.idx };
    r->program_pbr_inst_fwdplus = (bgfx_program_handle_t){ shaders->pbr_inst_fwdplus.idx };
    r->program_pbr_skinned_fwdplus = (bgfx_program_handle_t){ shaders->pbr_skinned_fwdplus.idx };
    r->program_pbr_toon        = (bgfx_program_handle_t){ shaders->pbr_toon.idx };
    r->program_outline_skinned = (bgfx_program_handle_t){ shaders->outline_skinned.idx };
    r->program_shadow = (bgfx_program_handle_t){ shaders->shadow.idx };
    r->program_shadow_inst = (bgfx_program_handle_t){ shaders->shadow_inst.idx };
    r->program_shadow_skinned = (bgfx_program_handle_t){ shaders->shadow_skinned.idx };
    r->program_terrain = (bgfx_program_handle_t){ shaders->terrain.idx };
    memcpy(r->program_variant, shaders->variant, sizeof r->program_variant);

    if (r->program.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "color shader not provided");
}

static JceShaderSet renderer_owned_shader_set(const JceRenderer *r)
{
    JceShaderSet set;
    memset(&set, 0xff, sizeof(set));
#define JCE_SNAPSHOT_PROGRAM(member_, renderer_) \
    set.member_ = (JceShaderHandle){ r->renderer_.idx };
    JCE_SHADER_SET_OWNED_FIELDS(JCE_SNAPSHOT_PROGRAM)
#undef JCE_SNAPSHOT_PROGRAM
    memcpy(set.variant, r->program_variant, sizeof(set.variant));
    return set;
}

bool jce_renderer_reload_shaders_fs(JceRenderer        *r,
                                    const char         *dev_dir,
                                    const JcePakArchive *pak)
{
    if (!r || r->is_fallback || r->headless || !pak) return false;

    /* Retire one owner after the new set is installed. bgfx defers GPU
     * destruction until queued draws finish. Partial-load failure owns the
     * complete new set too, including SDF and all deduplicated variants. */
    const JceShaderSet old = renderer_owned_shader_set(r);
    const JceShaderSet next = jce_shaders_load_all_fs(dev_dir, pak);
    if (!jce_shader_valid(next.color)) {
        LOG_ERROR(LOG_TAG, "reload: color shader load failed, aborting swap");
        jce_shader_set_release_owned(&next);
        return false;
    }
    jce_renderer_set_shaders(r, &next);
    jce_shader_set_release_owned(&old);
    LOG_INFO(LOG_TAG, "shaders reloaded (dev_dir=%s)", dev_dir ? dev_dir : "(none)");
    return true;
}

JceRenderer *jce_renderer_create_fallback_impl(JceWindow *win)
{
    if (!win) return NULL;
    const JceRendererLaunchDiagnostics launch = renderer_launch_diagnostics_snapshot();
    renderer_launch_diagnostics_observe(&launch, NULL, 5);
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

bool jce_renderer_is_headless(const JceRenderer *r)
{
    return r ? r->headless : false;
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

/* Instance buffers share one logical budget across shader stride families.
 * Reservations and commits run on the renderer owner thread; encoder binding
 * of committed tokens is read-only. Backend release callbacks only free CPU
 * staging, which uses the existing thread-safe engine allocator. */
static uint32_t instance_available(void *user, uint32_t count, uint16_t stride)
{
    (void)user;
    /* Bound backend count/size arithmetic. Large complete batches use the
     * reusable dynamic route instead of trusting a large transient query. */
    if (count > 65536u) count = 65536u;
    const uint32_t available = bgfx_get_avail_instance_data_buffer(count, stride);
    return available < count ? available : count;
}
static bool instance_allocate_transient(void *user, uint32_t count, uint16_t stride,
                                         JceInstanceTransient *out)
{
    (void)user;
    bgfx_instance_data_buffer_t idb;
    memset(&idb, 0, sizeof idb);
    bgfx_alloc_instance_data_buffer(&idb, count, stride);
    out->data = idb.data; out->size = idb.size; out->offset = idb.offset;
    out->num = idb.num; out->stride = idb.stride; out->handle = idb.handle.idx;
    return idb.data != NULL;
}
static uint16_t instance_create_dynamic(void *user, uint32_t count, uint16_t stride)
{
    (void)user;
    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, BGFX_RENDERER_TYPE_NOOP);
    for (uint16_t i = 0; i < stride / 16u; ++i)
        bgfx_vertex_layout_add(&layout, (bgfx_attrib_t)(BGFX_ATTRIB_TEXCOORD7 - i),
                                 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);
    return bgfx_create_dynamic_vertex_buffer(count, &layout, BGFX_BUFFER_NONE).idx;
}
static void instance_destroy_dynamic(void *user, uint16_t idx)
{
    (void)user;
    bgfx_dynamic_vertex_buffer_handle_t handle = {idx};
    bgfx_destroy_dynamic_vertex_buffer(handle);
}
static void *instance_allocate_staging(void *user, uint32_t bytes)
{
    (void)user;
    return JCE_MALLOC(bytes);
}
static void instance_free_staging(void *user, void *data)
{
    (void)user;
    JCE_FREE(data);
}
static void instance_release_staging(void *data, void *user)
{
    /* CPU staging is freed exactly once by bgfx's release callback. The
     * optional immutable trace lease never borrows the renderer owner. */
    instance_free_staging(NULL, data);
    jce_instance_upload_trace_release((JceInstanceStageLease *)user);
}
static bool instance_update_dynamic(void *user, uint16_t idx, uint32_t offset,
                                     void *data, uint32_t bytes)
{
    (void)user;
    const bgfx_memory_t *memory = bgfx_make_ref_release(data, bytes, instance_release_staging, NULL);
    if (!memory) return false;
    bgfx_dynamic_vertex_buffer_handle_t handle = {idx};
    bgfx_update_dynamic_vertex_buffer(handle, offset, memory);
    return true;
}
static bool instance_update_dynamic_traced(void *user, uint16_t idx, uint32_t offset,
                                            void *data, uint32_t bytes, JceInstanceStageLease *lease)
{
    (void)user;
    const bgfx_memory_t *memory = bgfx_make_ref_release(data, bytes, instance_release_staging, lease);
    if (!memory) return false; /* Caller cancels metadata; staging was not transferred. */
    jce_instance_upload_trace_transfer(lease);
    bgfx_dynamic_vertex_buffer_handle_t handle = {idx};
    bgfx_update_dynamic_vertex_buffer(handle, offset, memory);
    return true;
}
static void instance_bind_dynamic(void *user, void *encoder, uint16_t idx,
                                   uint32_t offset, uint32_t count)
{
    (void)user;
    bgfx_dynamic_vertex_buffer_handle_t handle = {idx};
    if (encoder)
        bgfx_encoder_set_instance_data_from_dynamic_vertex_buffer((bgfx_encoder_t *)encoder, handle, offset, count);
    else bgfx_set_instance_data_from_dynamic_vertex_buffer(handle, offset, count);
}
static void instance_bind_transient(void *user, void *encoder,
                                     const JceInstanceTransient *t, uint32_t count)
{
    (void)user;
    bgfx_instance_data_buffer_t idb;
    idb.data = t->data; idb.size = t->size; idb.offset = t->offset;
    idb.num = t->num; idb.stride = t->stride; idb.handle.idx = t->handle;
    if (encoder) bgfx_encoder_set_instance_data_buffer((bgfx_encoder_t *)encoder, &idb, 0, count);
    else bgfx_set_instance_data_buffer(&idb, 0, count);
}
static void instance_warn_partial(void *user, uint32_t epoch, uint32_t requested,
                                   uint32_t admitted, uint16_t stride)
{
    (void)user;
    LOG_WARN(LOG_TAG, "CPU instance upload unavailable: requested=%u admitted=%u stride=%u submission=%u",
             requested, admitted, (unsigned)stride, epoch);
}
static const JceInstanceUploadProvider s_instance_provider = {
    instance_available, instance_allocate_transient, instance_create_dynamic,
    instance_destroy_dynamic, instance_allocate_staging, instance_free_staging,
    instance_update_dynamic, instance_bind_dynamic, instance_bind_transient, instance_warn_partial,
    instance_update_dynamic_traced, NULL, NULL
};
static uint32_t instance_transient_reserve_bytes(void)
{
    /* Conservative policy for later UI/geometry submissions, not a measured
     * guarantee for arbitrary UI. Small-pool editor captures showed UI loss
     * when instance chunks consumed the entire vertex pool. Default 1 MiB;
     * JCE_INSTANCE_TRANSIENT_RESERVE_KB explicitly accepts 0..131072 KiB. */
    const char *text = getenv("JCE_INSTANCE_TRANSIENT_RESERVE_KB");
    if (!text || !text[0]) return 1048576u;
    uint32_t kb = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || kb > 131072u / 10u) goto invalid;
        kb = kb * 10u + (uint32_t)(*p - '0');
        if (kb > 131072u) goto invalid;
    }
    return kb * 1024u;
invalid:
    LOG_WARN(LOG_TAG, "JCE_INSTANCE_TRANSIENT_RESERVE_KB must be 0..131072; using 1024");
    return 1048576u;
}
JceInstanceUploadOwner *jce_renderer_instance_upload_owner(const JceRenderer *r)
{
    if (!r || r->is_fallback) return NULL;
    JceInstanceUploadOwner *owner = &((JceRenderer *)r)->instance_upload;
    if (!owner->initialized)
        jce_instance_upload_owner_init(owner, &s_instance_provider, NULL, instance_transient_reserve_bytes(),
                                       r->submitted_payload.context_generation);
    return owner;
}
uint32_t jce_renderer_instance_upload_epoch(void)
{
    return s_device.offscreen_submission_index;
}

JceOffscreenTarget **jce_renderer_offscreen_head(const JceRenderer *r)
{
    return &((JceRenderer *)r)->offscreen_targets;
}

bool jce_renderer_offscreen_diag_enabled(const JceRenderer *r)
{
    return r && !r->is_fallback && s_device.render_resource_diag;
}

uint32_t jce_renderer_offscreen_submission_index(void)
{
    return s_device.offscreen_submission_index;
}

bool jce_renderer_frame_admission_current_internal(const JceRenderer *r)
{
    if (!r) return false;
    /* Noop and SDL fallback keep their separate existing lifetimes. */
    if (r->is_fallback || r->headless) return true;
    return jce_renderer_caps_api_web_context_current();
}

static bool renderer_submit_frame_checked(const JceRenderer *r)
{
    if (!r || r->is_fallback || !jce_renderer_frame_admission_current_internal(r)) {
        JceTraceScope refused = jce_trace_scope_begin("renderer.submit_refused");
        jce_trace_scope_end(refused, r ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED);
        return false;
    }
    /* Main-thread CPU call interval, nested within end_frame. It can include
     * synchronization or scheduling; it does not measure GPU execution or
     * isolated presentation time. Keep resource retirement outside this span. */
    JceTraceScope advance = jce_trace_scope_begin("renderer.frame_advance_cpu");
    uint64_t trace_generation = 0;
    uint32_t trace_backend_before = 0, trace_ordinal_before = 0;
    if (advance.id) {
        trace_generation = r->submitted_payload.context_generation;
        trace_backend_before = s_device.frame_index;
        trace_ordinal_before = s_device.offscreen_submission_index;
    }
    const bool measure = jce_perf_phase_enabled() != 0;
    const bool timed = measure || advance.id != 0;
    const uint64_t started = timed ? jce_time_perf_counter() : 0;
    s_device.frame_index = bgfx_frame(false);
    const uint64_t ended = timed ? jce_time_perf_counter() : 0;
    if (measure) jce_perf_phase_add("renderer_frame_advance_cpu", jce_time_perf_to_ms(started, ended));
    jce_renderer_submission_trace_emit(advance, trace_generation,
        trace_backend_before, s_device.frame_index, trace_ordinal_before);
    jce_renderer_submission_clock_emit(advance, started, ended);
    jce_trace_scope_end(advance, JCE_TRACE_TASK_SUCCEEDED);
    jce_render_readback_collect(r, s_device.frame_index);
    ++s_device.offscreen_submission_index;
    jce_offscreen_retirement_collect(r, s_device.offscreen_submission_index);
    jce_instance_upload_owner_advance(&((JceRenderer *)r)->instance_upload,
                                       s_device.offscreen_submission_index);
    return true;
}

uint32_t jce_renderer_submit_frame(const JceRenderer *r)
{
    (void)renderer_submit_frame_checked(r);
    return s_device.frame_index;
}
void jce_renderer_destroy_impl(JceRenderer *r)
{
    if (!r) return;
    renderer_shader_dev_release(r);
    jce_upload_metrics_unbind(&s_device.payload, &r->submitted_payload);
    /* Drain and join the PNG writer BEFORE anything else goes away: a capture
     * delivered on the last frame is still only pixels in a queue, and losing
     * it on exit would defeat every harness that reads the files back. */
    jce_renderer_readback_capture_shutdown();
    jce_render_readback_before_backend_shutdown(r);
    jce_offscreen_retirement_shutdown(r);
    jce_instance_upload_owner_destroy(&r->instance_upload);
    if (r->is_fallback) {
        jce_rcb_after_backend_shutdown();
        if (r->sdl_renderer) {
            SDL_DestroyRenderer(r->sdl_renderer);
        }
        JCE_FREE(r);
        LOG_INFO(LOG_TAG, "renderer destroyed (fallback)");
        return;
    }
    s_dbg_text_enabled = false;
    const JceShaderSet owned = renderer_owned_shader_set(r);
    jce_shader_set_release_owned(&owned);
    if (r->u_sdf_params.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_sdf_params);
    if (r->u_sdf_outline.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_sdf_outline);
    if (r->u_sdf_shadow_offset.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_sdf_shadow_offset);
    if (r->u_sdf_shadow_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_sdf_shadow_color);
    if (r->u_tex_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_tex_color);
    if (r->u_light_dir.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_dir);
    if (r->u_light_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_color);

    /* Subsystems and path-keyed bgfx-handle caches, torn down while bgfx is
     * still alive: FreeType (bgfx-agnostic today, but routing it through
     * renderer destroy preserves the LIFO contract documented in
     * jce_engine.c), jce_pbr_material_load_json's graph programs, and the
     * IES LUTs baked from authored .ies paths. */
    jce_text_shutdown();
    jce_pbr_material_shutdown();
    jce_lighting_system_shutdown();
    jce_ies_cache_shutdown();

    /* Free the octahedral-impostor shared GPU resources (program, quad VB,
     * uniforms, any in-flight bake FBO) while bgfx is still alive. */
    jce_impostor_shutdown();

    /* JCE_DBG_LEAKSTATS=1: preserve the legacy fields as a pre-shutdown
     * backend-reported handle snapshot. Destroy commands may still be queued;
     * nonzero counts are not a leak verdict, GPU idle or physical residency. */
    if (getenv("JCE_DBG_LEAKSTATS") && getenv("JCE_DBG_LEAKSTATS")[0] == '1') {
        const bgfx_stats_t *st = bgfx_get_stats();
        if (st)
            LOG_INFO(LOG_TAG, "[leakstats] scope=pre_shutdown_backend_snapshot gpu_completion=unavailable "
                     "tex=%u fb=%u vb=%u ib=%u dvb=%u dib=%u "
                     "prog=%u shader=%u uniform=%u occ=%u",
                     (unsigned)st->numTextures, (unsigned)st->numFrameBuffers,
                     (unsigned)st->numVertexBuffers, (unsigned)st->numIndexBuffers,
                     (unsigned)st->numDynamicVertexBuffers,
                     (unsigned)st->numDynamicIndexBuffers,
                     (unsigned)st->numPrograms, (unsigned)st->numShaders,
                     (unsigned)st->numUniforms, (unsigned)st->numOcclusionQueries);
    }

    jce_shader_deferred_shutdown();
    JCE_TRACE_SCOPE_CALL("renderer.backend_shutdown_cpu", bgfx_shutdown());
    jce_render_readback_after_backend_shutdown(r);
    jce_bgfx_trace_end(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    jce_impostor_after_backend_shutdown();
    jce_rcb_after_backend_shutdown();
    /* After bgfx is down: it never deletes a context it did not create, so
     * the ladder's context is ours to release and only now is it unused. */
    gl_ladder_release();
    jce_renderer_caps_api_reset();
    JCE_FREE(r);
    LOG_INFO(LOG_TAG, "renderer destroyed");
}

/* -- Per-frame ------------------------------------------------------ */

bool jce_renderer_begin_frame_impl(const JceRenderer *r, JceWindow *win)
{
    JCE_PROFILE_ZONE_N("Renderer::BeginFrame");
    if (!r || !win) { JCE_PROFILE_ZONE_END; return false; }
    if (r->is_fallback || r->headless) { JCE_PROFILE_ZONE_END; return true; }
    if (!jce_renderer_frame_admission_current_internal(r)) { JCE_PROFILE_ZONE_END; return false; }

    /* Full-backbuffer viewport for all views. */
    uint16_t vp_x, vp_y, vp_w, vp_h;
    jce_window_calc_viewport(win, &vp_x, &vp_y, &vp_w, &vp_h);

    const bgfx_caps_t *caps = bgfx_get_caps();

    bgfx_set_view_name(JCE_VIEW_MAIN_3D, "Renderer/Main", INT32_MAX);
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
    return true;
}

bool jce_renderer_begin_frame_3d_impl(const JceRenderer *r, JceWindow *win,
                                  const JceCamera *cam, uint16_t view_id)
{
    JCE_PROFILE_ZONE_N("Renderer::BeginFrame3D");
    if (!r || !win) { JCE_PROFILE_ZONE_END; return false; }
    if (r->is_fallback || r->headless) { JCE_PROFILE_ZONE_END; return true; }
    if (!jce_renderer_frame_admission_current_internal(r)) { JCE_PROFILE_ZONE_END; return false; }

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
    /* AND BIND THE VIEW TO THE BACKBUFFER, because that is what this function
     * means: configure this view to draw the 3D scene TO THE WINDOW.
     *
     * A bgfx view's framebuffer binding is sticky.  The shipped runtime's
     * offscreen bridge and its direct-to-backbuffer fallback share one view
     * id, and the bridge binds its FBO in prepare_keep -- before the caller
     * has decided whether any post-processing is enabled.  When none is, the
     * bridge silently declines to present and the fallback renders the whole
     * game into an offscreen target nobody shows.  The result is a black
     * window, and the shipped runtime shipped that way for every project with
     * no post-processing: measured on a two-sphere scene, avg_lum 217.4 with
     * one postfx effect enabled and 0.0 with none, same frame, same scene.
     *
     * Binding here fixes the class rather than the instance: any view handed
     * to this function is being pointed at the window, whatever it was
     * pointed at before. */
    {
        bgfx_frame_buffer_handle_t backbuffer = { UINT16_MAX };
        bgfx_set_view_frame_buffer(view_id, backbuffer);
    }
    /* Force sequential submission order for the 3D view so the scene
     * renderer's draw order (sky → shadows → opaques → transparents) is
     * preserved when rendering directly to the backbuffer.  The editor's
     * offscreen bridge already enables sequential mode, so this matches
     * that behaviour for runtime games that bypass the bridge. */
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);
    JCE_PROFILE_ZONE_END;
    return true;
}

void jce_renderer_present_splash(const JceRenderer *r,
                                 JceWindow *win,
                                 uint32_t rgba_color)
{
    if (!r || r->is_fallback || !win ||
        !jce_renderer_frame_admission_current_internal(r)) return;

    uint32_t w = 0, h = 0;
    jce_window_get_size(win, &w, &h);
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    bgfx_set_view_name(0, "Renderer/Splash", INT32_MAX);
    bgfx_set_view_clear(0,
                        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL,
                        rgba_color, 1.0f, 0);
    bgfx_set_view_rect(0, 0, 0, (uint16_t)w, (uint16_t)h);
    bgfx_touch(0);
    jce_renderer_submit_frame(r);
}

/* Most recent backend snapshot, not a GPU-completion or all-heap claim.
 * Sample counters at a bounded cadence; no readback, query or allocation. */
void jce_renderer_trace_snapshot(const JceRenderer *r)
{
    if (!r || r->is_fallback || r->headless ||
        !jce_renderer_snapshot_due_at_submission(s_device.frame_index,
            s_device.offscreen_submission_index))
        return;
    /* PERF_LOG independently consumes this existing bounded node. Read its
     * environment flag only after cadence admission, never on every frame. */
    const bool trace_on = jce_trace_enabled();
    const bool perf_on = jce_env_flag_enabled(getenv("JCE_PERF_LOG"));
    if (!trace_on && !perf_on) return;
    if (trace_on)
        jce_bgfx_trace_summary(&s_device.allocation_trace, JCE_BGFX_ALLOC_SUMMARY_SNAPSHOT);
    const bgfx_stats_t *st = bgfx_get_stats();
    if (!st) return;
    const bgfx_caps_t *caps = bgfx_get_caps();
    const JceRendererSnapshot snapshot = {
        .draw_count = st->numDraw,
        .compute_count = st->numCompute,
        .texture_count = st->numTextures,
        .framebuffer_count = st->numFrameBuffers,
        .program_count = st->numPrograms,
        .shader_count = st->numShaders,
        .vertex_buffer_count = st->numVertexBuffers,
        .index_buffer_count = st->numIndexBuffers,
        .transient_vb_used_bytes = st->transientVbUsed,
        .transient_ib_used_bytes = st->transientIbUsed,
        .texture_estimate_bytes = st->textureMemoryUsed,
        .rt_estimate_bytes = st->rtMemoryUsed,
        .adapter_reported_used_bytes = st->gpuMemoryUsed,
        .gpu_timer_frequency = st->gpuTimerFreq,
        .gpu_time_begin = st->gpuTimeBegin,
        .gpu_time_end = st->gpuTimeEnd,
        .encoder_frame_used = st->numEncoders,
        .adapter_reported_vendor_id = caps ? caps->vendorId : 0,
        .adapter_reported_device_id = caps ? caps->deviceId : 0,
        .bgfx_renderer_type = caps ? (uint32_t)caps->rendererType : 0,
        .host_submission_ordinal = s_device.offscreen_submission_index,
        .device_epoch = r->submitted_payload.valid ? r->submitted_payload.context_generation : 0,
        .gpu_frame_num = st->gpuFrameNum,
        .gpu_view_count = st->numViews,
        .gpu_view_data_available = st->numViews == 0 || st->viewStats != NULL,
    };
    if (trace_on)
        jce_renderer_snapshot_emit(s_device.frame_index, &snapshot,
            caps ? &caps->limits.maxEncoders : NULL);
    if (trace_on && st->viewStats) {
        /* numViews bounds this borrowed provider array; no sampled view is
         * silently truncated to the public overlay's smaller view list. */
        for (uint16_t i = 0; i < st->numViews; ++i) {
            const bgfx_view_stats_t *source = &st->viewStats[i];
            const JceRendererViewSnapshot view = {
                .view_id = source->view,
                .gpu_frame_num = source->gpuFrameNum,
                .gpu_time_begin = source->gpuTimeBegin,
                .gpu_time_end = source->gpuTimeEnd,
            };
            jce_renderer_snapshot_emit_view(s_device.frame_index, &snapshot, &view);
        }
    }
    if (perf_on) {
        /* Explicit diagnostic survives DIST; all raw values stay integers.
         * This one line is timing provenance, never a GPU/present fence. */
        jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
            "gpu-provenance: api_frame=%u submission=%u device_epoch=%llu epoch_available=%u "
            "source_frame=%u begin_ticks=%lld end_ticks=%lld frequency=%lld interval_available=%u "
            "views=%u view_data_available=%u",
            s_device.frame_index, snapshot.host_submission_ordinal,
            (unsigned long long)snapshot.device_epoch,
            jce_renderer_snapshot_epoch_available(snapshot.device_epoch) ? 1u : 0u,
            snapshot.gpu_frame_num, (long long)snapshot.gpu_time_begin,
            (long long)snapshot.gpu_time_end, (long long)snapshot.gpu_timer_frequency,
            jce_renderer_gpu_interval_available(snapshot.gpu_timer_frequency,
                snapshot.gpu_time_begin, snapshot.gpu_time_end) ? 1u : 0u,
            (unsigned)snapshot.gpu_view_count, snapshot.gpu_view_data_available ? 1u : 0u);
        JceRendererViewNames names = jce_renderer_view_names_begin(st->numViews,
            snapshot.host_submission_ordinal, snapshot.gpu_view_data_available);
        if (st->viewStats) for (uint16_t i = 0; i < st->numViews; ++i) {
            const uint16_t index = (uint16_t)(((uint32_t)names.start + i) % st->numViews);
            const bgfx_view_stats_t *view = &st->viewStats[index];
            if (!jce_renderer_view_names_append(&names, view->view, view->gpuFrameNum,
                    view->name, sizeof(view->name))) break;
        }
        jce_renderer_snapshot_log_view_names(s_device.frame_index, &snapshot, &names);
    }
}

bool jce_renderer_end_frame_impl(const JceRenderer *r)
{
    JCE_PROFILE_ZONE_N("Renderer::EndFrame");
    if (!r) { JCE_PROFILE_ZONE_END; return false; }
    if (r->is_fallback || r->headless) { JCE_PROFILE_ZONE_END; return true; }
    if (!jce_renderer_frame_admission_current_internal(r)) { JCE_PROFILE_ZONE_END; return false; }

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
            s_device.frame_index >= (uint32_t)s_cap_frame) {
            /* THE HOST FIRST, THE BACKBUFFER OTHERWISE.
             *
             * jce_renderer_request_screenshot photographs the BACKBUFFER,
             * which in the editor carries the ImGui layer -- so an automated
             * capture there contains the profiler panel's clock, measured at
             * 53,089 of 3,911,680 pixels differing between two runs of one
             * input digest, all inside rows 1080..1526 with the scene above
             * byte-identical.  A host that has a UI-free image of its own
             * says so by installing a hook.
             *
             * DECLINING IS NORMAL, NOT AN ERROR: the editor's Game View panel
             * can be closed, and then there is no offscreen target to read.
             * False falls through to exactly the behaviour every host had
             * before the hook existed. */
            bool taken = jce_rcb_host_took_capture(s_cap_path);
            if (!taken)
                taken = jce_renderer_request_screenshot(s_cap_path);
            if (taken)
                s_cap_done = true;
        }
    }

    /* Recording: request a backbuffer capture for this frame (one in flight;
       the screen_shot callback routes it to the capture sink). Reuses the
       proven screenshot path since BGFX_RESET_CAPTURE is inert here. */
    if (jce_rcb_capture_wants_shot()) {
        bgfx_frame_buffer_handle_t bb = { UINT16_MAX };  /* backbuffer */
        jce_rcb_capture_mark_shot_pending();
        bgfx_request_screen_shot(bb, JCE_CAPTURE_SENTINEL);
    }

    /* Arm a RenderDoc GPU capture for this frame if JCE_RDOC_FRAME targets it
     * (no-op unless the build enables RenderDoc + a capture is injected). Must
     * precede bgfx_frame() — the backend API calls the capture wraps happen as
     * bgfx flushes the command buffer there. */
    jce_gpu_capture_tick();

    /* Transform-matrix cache pressure check (frame boundary — the bgfx matrix
     * cache resets inside bgfx_frame).  bgfx caches every set_transform matrix
     * in a fixed BGFX_CONFIG_MAX_MATRIX_CACHE pool that saturates SILENTLY in
     * release: once full, later draws in the same frame get clamped transforms
     * and vanish — measured as per-char skinned crowds losing their color pass
     * (bone palettes are ~24-128 matrices per draw; ~104k matrices demanded at
     * 1000 chars).  The counter is a lower bound (shim + set_bones producers),
     * so tripping the threshold is always real.  Fix at scale = the crowd
     * instancing paths (JCE_CROWD_BINDPOSE/_INSTANCE), which upload no
     * per-char matrices (~1.5k at 1000 chars). */
    {
        /* bgfx's compiled cap — kept in sync with conan/hooks/
         * hook_bgfx_wasm_fix.py (which doubles the desktop default; Emscripten
         * keeps bgfx's 65536 default for the wasm memory budget). */
#if JCE_PLATFORM_WEB
        static const uint32_t k_matrix_cache_cap = 65536u;
#else
        static const uint32_t k_matrix_cache_cap = 131072u;
#endif
        static uint32_t s_xform_peak = 0;
        static bool     s_xform_warned = false;
        if (jce_dbg_xform_matrices > s_xform_peak) {
            s_xform_peak = jce_dbg_xform_matrices;
            if (!s_xform_warned && s_xform_peak > (k_matrix_cache_cap / 5u * 4u)) {
                s_xform_warned = true;
                LOG_WARN(LOG_TAG,
                    "transform matrices this frame: %u — near/over the bgfx "
                    "matrix-cache cap (%u); later draws this frame will "
                    "silently lose their transforms. Enable crowd instancing "
                    "(JCE_CROWD_BINDPOSE / JCE_CROWD_INSTANCE) for large "
                    "skinned crowds. (warned once; peak in JCE_PERF_LOG)",
                    s_xform_peak, k_matrix_cache_cap);
            }
        }
        static int s_perf_log = -1;
        if (s_perf_log < 0) {
            const char *v = getenv("JCE_PERF_LOG");
            s_perf_log = jce_env_flag_enabled(v);
        }
        if (s_perf_log && (s_device.frame_index % 120u) == 0u && s_xform_peak > 0)
            LOG_INFO(LOG_TAG, "perf-xform: peak %u matrices/frame (cache cap %u)",
                     s_xform_peak, k_matrix_cache_cap);
        jce_dbg_xform_matrices = 0;
    }

    const bool submitted = renderer_submit_frame_checked(r);

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
            /* Backend-reported direct primitives, not pre-occlusion submits
             * or an exact count of GPU indirect argument contents. */
            uint64_t reported_primitives = 0;
            for (unsigned topology = 0; topology < BGFX_TOPOLOGY_COUNT; ++topology)
                reported_primitives += st->numPrims[topology];
            JCE_PROFILE_PLOT_I("render.reported_primitives", (int64_t)reported_primitives);
            JCE_PROFILE_PLOT_I("render.textures",    (int64_t)st->numTextures);
            JCE_PROFILE_PLOT_I("render.gpu_mem_mb",  (int64_t)(st->gpuMemoryUsed >> 20));
        }
    }
#endif

    JCE_PROFILE_ZONE_END;
    return submitted;
}

/* -- Events --------------------------------------------------------- */

void jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h)
{
    if (!jce_renderer_frame_admission_current_internal(r)) return;
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
    const uint32_t requested_w = w, requested_h = h;
    if (w < 1u) w = 1u;
    if (h < 1u) h = 1u;
    w = (w + 3u) & ~3u;

    renderer_reset_cpu(JCE_RENDERER_RESET_RESIZE, requested_w, requested_h, w, h, r->reset_flags);
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
    renderer_presentation_vsync_rebind(r, win);
    renderer_reset_cpu(JCE_RENDERER_RESET_REBIND, w, h, w, h, r->reset_flags);
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

bgfx_program_handle_t jce_renderer_get_program_text_sdf(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_text_sdf.idx) } : invalid;
}

JceUniformHandle jce_renderer_get_sdf_params_uniform(const JceRenderer *r)
{
    JceUniformHandle h;
    h.idx = r ? r->u_sdf_params.idx : UINT16_MAX;
    return h;
}

JceUniformHandle jce_renderer_get_sdf_outline_uniform(const JceRenderer *r)
{
    JceUniformHandle h;
    h.idx = r ? r->u_sdf_outline.idx : UINT16_MAX;
    return h;
}

JceUniformHandle jce_renderer_get_sdf_shadow_offset_uniform(const JceRenderer *r)
{
    JceUniformHandle h;
    h.idx = r ? r->u_sdf_shadow_offset.idx : UINT16_MAX;
    return h;
}

JceUniformHandle jce_renderer_get_sdf_shadow_color_uniform(const JceRenderer *r)
{
    JceUniformHandle h;
    h.idx = r ? r->u_sdf_shadow_color.idx : UINT16_MAX;
    return h;
}

bgfx_program_handle_t jce_renderer_get_program_textured(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_textured.idx) } : invalid;
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


/* THE ONE PLACE A PBR DRAW'S PROGRAM IS DECIDED.
 *
 * It used to be seven places, and the last attempt at a keyword axis wired
 * six of them: forcing the variant on every draw moved ZERO pixels, because
 * shape primitives take a seventh path nobody had touched, while the
 * engine's own enable_csm=false moved 7979 on the same frame.  Half-wired is
 * worse than absent -- it is absent AND it looks done.  So the sites no
 * longer choose; they say what the draw IS and what the material ASKS FOR,
 * and this turns the pair into a program.
 *
 * THE FALLBACK IS A DEGRADE, NOT A GUESS.  An entry is invalid when its .bin
 * is not in the pak -- an older pak, or a backend the variant is excluded
 * from.  Dropping keywords from the HIGHEST bit down is the right order
 * because the manifest puts the cheaper-to-lose axis first: losing NOSHADOW
 * costs instructions, losing FWDPLUS costs a lighting model.  Key 0 is always
 * present, so this terminates; if even that failed to load the caller gets an
 * invalid handle and skips the draw, which is what it did before.
 *
 * Fold frame-wide keys into every material selection here, including
 * JCE_SHADER_FORCE_KEYS, so normal draws and diagnostic controls use the
 * same path rather than relying on each call site to add the right bits. */
JceShaderHandle jce_renderer_get_program_variant(const JceRenderer *r,
                                                 JceShaderVertexVariant vv,
                                                 uint32_t keys)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    if ((int)vv < 0 || (int)vv >= JCE_SHADER_VARIANT_COUNT) return invalid;

    uint32_t k = (keys | r->frame_shader_keys)
               & (uint32_t)(JCE_SHADER_KEY_COUNT - 1);
    for (;;) {
        const uint16_t idx = r->program_variant[(int)vv][k].idx;
        if (idx != UINT16_MAX) return (JceShaderHandle){ idx };
        if (k == 0u) return invalid;
        /* Clear the highest set bit and try again. */
        uint32_t high = k;
        high |= high >> 1; high |= high >> 2; high |= high >> 4;
        high |= high >> 8; high |= high >> 16;
        high = high ^ (high >> 1);
        k &= ~high;
    }
}

JceShaderHandle jce_renderer_get_program_pbr(const JceRenderer *r)
{
    if (!r) { JceShaderHandle iv = JCE_INVALID_SHADER; return iv; }
    return jce_renderer_get_program_variant(r, JCE_SHADER_VV_PBR,
        0u);
}

JceShaderHandle jce_renderer_get_program_pbr_inst(const JceRenderer *r)
{
    if (!r) { JceShaderHandle iv = JCE_INVALID_SHADER; return iv; }
    return jce_renderer_get_program_variant(r, JCE_SHADER_VV_PBR_INST,
        0u);
}


/* Per-instance-tint instanced PBR program (large-world-opt P1 #7).  Returns
 * invalid when the variant didn't load (older pak): callers fall back to solo
 * draws for tinted entities, so the feature degrades gracefully.  No Forward+
 * counterpart yet — tinted runs use the brute-force lighting path. */
JceShaderHandle jce_renderer_get_program_pbr_inst_tint(const JceRenderer *r)
{
    if (!r) { JceShaderHandle iv = JCE_INVALID_SHADER; return iv; }
    return jce_renderer_get_program_variant(r, JCE_SHADER_VV_PBR_INST_TINT, 0u);
}

JceShaderHandle jce_renderer_get_program_pbr_inst_tex_array(const JceRenderer *r)
{
    if (!r) { JceShaderHandle iv = JCE_INVALID_SHADER; return iv; }
    return jce_renderer_get_program_variant(r, JCE_SHADER_VV_PBR_INST_TEX_ARRAY, 0u);
}

JceShaderHandle jce_renderer_get_program_pbr_inst_fade(const JceRenderer *r)
{
    if (!r) { JceShaderHandle iv = JCE_INVALID_SHADER; return iv; }
    return jce_renderer_get_program_variant(r, JCE_SHADER_VV_PBR_INST_FADE, 0u);
}

JceShaderHandle jce_renderer_get_program_pbr_skinned(const JceRenderer *r)
{
    if (!r) { JceShaderHandle iv = JCE_INVALID_SHADER; return iv; }
    return jce_renderer_get_program_variant(r, JCE_SHADER_VV_PBR_SKINNED,
        0u);
}

void jce_renderer_set_forwardplus_program_active(JceRenderer *r, bool on)
{
    if (!r) return;
    r->forwardplus_program_active = on;

    /* JCE_SHADER_FORCE_KEYS: OR diagnostic bits into the shared variant
     * selection path, read once. */
    if (r->force_shader_keys < 0) {
        const char *v = getenv("JCE_SHADER_FORCE_KEYS");
        r->force_shader_keys = (v && v[0]) ? (int)strtol(v, NULL, 0) : 0;
        if (r->force_shader_keys)
            LOG_WARN(LOG_TAG, "JCE_SHADER_FORCE_KEYS=0x%x -- every PBR draw "
                              "gets these keyword bits", r->force_shader_keys);
    }
    r->frame_shader_keys = (uint32_t)r->force_shader_keys
                         | (on ? JCE_SHADER_KEY_FWDPLUS : 0u);
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

JceShaderHandle jce_renderer_get_program_pbr_skinned_toon(const JceRenderer *r)
{
    /* NOT ROUTED THROUGH THE VARIANT TABLE, and that is a statement rather
     * than an omission: toon is a fragment FAMILY, not a keyword -- it
     * replaces the shading model instead of compiling a block out of it --
     * and the table's coordinates are (vertex variant, keywords).  Making it
     * a key would mean every other family gaining a toon column that names
     * the same program as its non-toon one, which is a bigger lie than this
     * one exception.  When a second family needs a second shading model the
     * manifest grows a family axis; until then this is the honest shape. */
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_pbr_toon.idx };
}

JceShaderHandle jce_renderer_get_program_outline_skinned(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_outline_skinned.idx };
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
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_terrain.idx) } : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_pbr.idx) } : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr_skinned(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_pbr_skinned.idx) } : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_shadow.idx) } : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow_skinned(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? (bgfx_program_handle_t){ jce_shader_deferred_resolve(r->program_shadow_skinned.idx) } : invalid;
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
    enabled = renderer_launch_vsync_resolve(r, enabled);
    enabled = renderer_presentation_vsync_resolve(&r->launch_vsync, enabled);
    bool current = (r->reset_flags & BGFX_RESET_VSYNC) != 0;
    if (current == enabled) { jce_renderer_launch_vsync_observe_internal(r, 2); return; }

    if (enabled)
        r->reset_flags |= BGFX_RESET_VSYNC;
    else
        r->reset_flags &= ~BGFX_RESET_VSYNC;

    const uint32_t requested_w = width, requested_h = height;
    if (width == 0 || height == 0) {
        const bgfx_stats_t *stats = bgfx_get_stats();
        width = stats->width;
        height = stats->height;
    }

    renderer_reset_cpu(JCE_RENDERER_RESET_VSYNC, requested_w, requested_h, width, height, r->reset_flags);
    jce_renderer_launch_vsync_observe_internal(r, 2);
}

void jce_renderer_set_vsync(JceRenderer *r, bool enabled)
{
    /* The size-aware owner rejects null/fallback and unchanged state before
     * querying the backend. Resolve dimensions only for an actual reset. */
    jce_renderer_set_vsync_for_size(r, enabled, 0, 0);
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
    renderer_reset_cpu(JCE_RENDERER_RESET_MSAA, 0, 0, stats->width, stats->height, r->reset_flags);
}

void jce_renderer_set_backbuffer_capture(JceRenderer *r, bool enable)
{
    (void)r;   /* the screenshot-based path needs no device reset */
    jce_rcb_capture_set_active(enable);
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
    return s_device.frame_index;
}

bool jce_renderer_get_submitted_payload_stats(const JceRenderer *r,
    JceSubmittedPayloadStats *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!r || !r->submitted_payload.valid) return false;
    *out = r->submitted_payload;
    out->frame_index = jce_renderer_get_frame_index(r);
    return true;
}

void jce_upload_metrics_record(JceUploadPayloadKind kind, uint64_t bytes,
    bool admitted)
{
    jce_upload_metrics_add(s_device.payload.active, kind, bytes, admitted);
}
