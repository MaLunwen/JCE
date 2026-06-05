/*
 * jce_renderer.h  Renderer initialization and per-frame management.
 */

#ifndef JCE_RENDERER_H
#define JCE_RENDERER_H


#include <jce/os/core/jce_defs.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jce/renderer/jce_renderer_caps.h>   /* JceRendererBackend */

JCE_EXTERN_C_BEGIN

/* Forward declarations. */
typedef struct JceWindow JceWindow;
typedef struct JceRenderer JceRenderer;
typedef struct JceCamera JceCamera;
typedef struct JceShaderSet JceShaderSet;
typedef struct JcePakArchive JcePakArchive;

/* -- Renderer configuration ---------------------------------------- */

typedef struct JceRendererConfig {
    int      backend;       /* JceRendererBackend value (0 = auto) */
    bool     vsync;
    bool     debug_text;
    uint32_t clear_color;   /* RGBA, e.g. 0x000000FF */
} JceRendererConfig;

/* -- Lifecycle ------------------------------------------------------ */

/* Create the renderer: initializes the GPU backend, sets up vertex
   layouts and view 0 clear state.
   Returns NULL on failure. */
JCE_API JceRenderer *jce_renderer_create(JceWindow *win,  const JceRendererConfig *cfg);

/* Attach pre-loaded shaders. Must be called after create
   and before the first frame. */
JCE_API void jce_renderer_set_shaders(JceRenderer *r, const JceShaderSet *shaders);

/* Hot-reload all standard shader programs from <dev_dir>/shaders on
   disk (with PAK fallback per-shader), destroy the previous bgfx
   programs, and swap in the new set.  Safe to call mid-application;
   bgfx defers handle destruction to end-of-frame.  Returns true if
   at least the color program reloaded successfully. */
JCE_API bool jce_renderer_reload_shaders_fs(JceRenderer        *r,
                                            const char         *dev_dir,
                                            const JcePakArchive *pak);

/* Create a safe fallback renderer using SDL_Renderer. */
JCE_API JceRenderer *jce_renderer_create_fallback(JceWindow *win);

/* Check if the renderer is running in fallback mode. */
JCE_API bool         jce_renderer_is_fallback(const JceRenderer *r);

/* Check if the EGL swap-buffers call has hung (Android only). */
JCE_API bool         jce_renderer_is_egl_hung(void);

/* Render the fallback error screen. */
JCE_API void         jce_renderer_render_fallback_frame(const JceRenderer *r);

/* Destroy the renderer: destroys shaders and shuts down the GPU backend. */
JCE_API void         jce_renderer_destroy(JceRenderer *r);

/* -- Per-frame ------------------------------------------------------ */

/* Begin a frame: sets view 0 orthographic projection (2D mode). */
JCE_API void         jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win);

/* Begin a frame with a 3D camera on the given view ID.
   Pass NULL camera for 2D ortho fallback on view 0. */
JCE_API void JCE_CALL jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
                                                  const JceCamera *cam, uint16_t view_id);

/* End a frame: submits all queued draw calls to the GPU. */
JCE_API void         jce_renderer_end_frame(const JceRenderer *r);

/* Submit a single "splash" frame that just clears view 0 to the given
   RGBA8 colour and presents. Intended to be called once at app-init
   entry, BEFORE heavy synchronous initialisation, so the OS window
   shows a themed background instead of the white "not responding"
   surface while subsystems warm up. Safe to call after the renderer
   has been created and the window is attached. */
JCE_API void         jce_renderer_present_splash(const JceRenderer *r,
                                                  JceWindow *win,
                                                  uint32_t rgba_color);

/* -- Events --------------------------------------------------------- */

/* Handle window resize: resets the GPU swap chain. */
JCE_API void         jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h);

/* Re-bind the native window handle and reset the GPU backend.
   Required after Android background/foreground cycle (ANativeWindow is recreated). */
JCE_API void         jce_renderer_rebind_platform(JceRenderer *r, JceWindow *win);

/* -- Debug text ----------------------------------------------------- */

JCE_API void JCE_CALL jce_renderer_dbg_text(uint16_t x, uint16_t y,
                                            uint8_t attr, const char *fmt, ...);

/* va_list variant for FFI bindings that cannot call variadic functions. */
JCE_API void JCE_CALL jce_renderer_dbg_text_v(uint16_t x, uint16_t y,
                                              uint8_t attr, const char *fmt, va_list ap);

/* -- Queries (for debug HUD) --------------------------------------- */

JCE_API const char  *jce_renderer_get_backend_name(const JceRenderer *r);

/* Canonical JceRendererBackend enum for the live bgfx renderer.
 * Mirrors bgfx::getRendererType() at call time.  Returns
 * JCE_BACKEND_AUTO if bgfx is not yet initialised. */
JCE_API JceRendererBackend jce_renderer_get_backend(const JceRenderer *r);
JCE_API const char  *jce_renderer_get_gpu_name(const JceRenderer *r);
JCE_API bool         jce_renderer_get_vsync(const JceRenderer *r);
JCE_API void         jce_renderer_set_vsync(JceRenderer *r, bool enabled);
JCE_API void JCE_CALL jce_renderer_set_vsync_for_size(JceRenderer *r, bool enabled,
                                                      uint32_t width, uint32_t height);

/* -- Backbuffer screenshot ----------------------------------------- */

/* Request an asynchronous capture of the current frame's backbuffer to `path`.
 * The shot is taken at the next frame and written off the main flow; output
 * format is chosen by the file extension (.png default, .bmp).  Returns false
 * if a capture is already pending or `path` is invalid.  Prefer the
 * application-layer jce_screenshot_save() wrapper. */
JCE_API bool jce_renderer_request_screenshot(const char *path);
JCE_API bool jce_renderer_screenshot_pending(void);

/* -- Continuous backbuffer capture (video recording) -------------- */
/* bgfx invokes the sink on the render thread, once per frame, while capture is
 * enabled. `data` is the raw backbuffer (BGRA8, `pitch` bytes/row, `size`
 * bytes total); `yflip` is 1 when the backend delivers bottom-up rows. */
typedef void (*JceCaptureBeginFn)(void *ud, uint32_t width, uint32_t height,
                                  uint32_t pitch, int yflip);
typedef void (*JceCaptureFrameFn)(void *ud, const void *data, uint32_t size);
typedef void (*JceCaptureEndFn)(void *ud);

/* Register the capture sink (pass NULLs to clear). One global sink. */
JCE_API void jce_renderer_set_capture_sink(JceCaptureBeginFn begin,
                                           JceCaptureFrameFn frame,
                                           JceCaptureEndFn end, void *ud);

/* Enable/disable bgfx continuous backbuffer capture (BGFX_RESET_CAPTURE).
 * Triggers a device reset; the registered sink then receives every frame. */
JCE_API void jce_renderer_set_backbuffer_capture(JceRenderer *r, bool enable);

/* -- Shader/uniform accessors (for 3D scene rendering) ------------- */

#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_texture_types.h>

/* Color (pos+color) shader program — flat-colored geometry (grid, debug). */
JCE_API JceShaderHandle  jce_renderer_get_program_color(const JceRenderer *r);

/* Mesh (pos+normal+uv) shader program. */
JCE_API JceShaderHandle  jce_renderer_get_program_mesh(const JceRenderer *r);

/* PBR shader programs. */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_inst(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_skinned(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_shadow(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_shadow_inst(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_shadow_skinned(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_terrain(const JceRenderer *r);

/* Create a linked graphics program from raw vertex + fragment shader
 * blobs (bgfx .bin format — output of shaderc).  Used by the editor's
 * shader graph "Compile & Bind" workflow to install graph-generated
 * fragment shaders at runtime.  Both blobs are copied internally; the
 * caller retains ownership of the input buffers.
 *
 * Returns JCE_INVALID_SHADER on failure.  Caller is responsible for
 * destroying the returned program via jce_renderer_destroy_program()
 * (which also destroys the two underlying shader objects). */
JCE_API JceShaderHandle  jce_renderer_create_program_from_blobs(
    const void *vs_blob, size_t vs_size,
    const void *fs_blob, size_t fs_size);

/* Destroy a program created via jce_renderer_create_program_from_blobs.
 * Safe to call with JCE_INVALID_SHADER.  bgfx defers actual destruction
 * to end-of-frame, so in-flight draws remain valid. */
JCE_API void jce_renderer_destroy_program(JceShaderHandle prog);

/* Texture sampler uniform (s_texColor). */
JCE_API JceUniformHandle jce_renderer_get_tex_uniform(const JceRenderer *r);

/* -- Transform / texture binding ------------------------------------- */

/* Set the model-to-world transform for the next draw call. */
JCE_API void jce_renderer_set_transform(const float *mtx);

/* Bind a texture to a sampler stage for the next draw. */
JCE_API void jce_renderer_bind_texture(const JceRenderer *r, uint8_t stage, JceTexture tex);

/* Debug text: single cell with attribute byte. */
JCE_API void jce_renderer_dbg_text_attr(uint16_t x, uint16_t y, uint8_t attr, const char *str);

/* -- Wireframe debug mode ------------------------------------------ */

/* Toggle wireframe rendering (F3+V debug feature). */
JCE_API void jce_renderer_set_wireframe(JceRenderer *r, bool enabled);
JCE_API bool jce_renderer_get_wireframe(const JceRenderer *r);

/* -- GPU capability queries --------------------------------------- */

/* Returns true if the GPU uses bottom-left framebuffer origin (OpenGL).
   Useful for UV flipping when displaying FBO textures in UI. */
JCE_API bool jce_renderer_origin_bottom_left(void);

/* -- GPU frame statistics ------------------------------------------ */

/* Maximum per-view timing rows reported by jce_renderer_get_gpu_stats. */
#define JCE_GPU_MAX_VIEW_STATS 64

/* One render view's timing, with names/durations already resolved so
   consumers (editor profiler, in-game overlays) never touch the GPU
   backend directly. */
typedef struct JceGpuViewStat {
    uint16_t view_id;
    char     name[64];
    double   gpu_ms;
    double   cpu_ms;
} JceGpuViewStat;

/* Flat, backend-agnostic snapshot of the last submitted frame.  All
   timings are pre-resolved to milliseconds; memory counters are bytes.
   This is the public surface that replaces direct bgfx_get_stats()
   access in consumer code. */
typedef struct JceGpuStats {
    bool     valid;             /* false when the backend has no stats  */

    /* Timing (milliseconds). */
    double   cpu_frame_ms;
    double   cpu_submit_ms;
    double   gpu_ms;
    double   wait_submit_ms;
    double   wait_render_ms;

    /* Per-frame counts. */
    uint32_t num_draw;
    uint32_t num_compute;
    uint32_t num_blit;

    /* Backbuffer + latency. */
    uint16_t backbuffer_width;
    uint16_t backbuffer_height;
    uint32_t max_gpu_latency;

    /* Resource counts. */
    uint16_t num_textures;
    uint16_t num_frame_buffers;
    uint16_t num_programs;
    uint16_t num_shaders;

    /* Memory (bytes; <0 when the backend cannot report it). */
    int64_t  rt_memory_used;
    int64_t  texture_memory_used;
    int64_t  gpu_memory_used;
    int64_t  gpu_memory_max;

    /* Per-view timing. */
    uint16_t       num_views;
    JceGpuViewStat views[JCE_GPU_MAX_VIEW_STATS];
} JceGpuStats;

/* Fill `out` with the most recent frame's GPU statistics.  Returns true
   and sets out->valid when the backend reports stats, false otherwise
   (out is still zero-initialized so callers can render placeholders). */
JCE_API bool jce_renderer_get_gpu_stats(JceGpuStats *out);

/* Monotonic bgfx frame index returned by the most recent bgfx_frame().
 * Used by asynchronous GPU readbacks to know when their data is ready. */
JCE_API uint32_t jce_renderer_get_frame_index(const JceRenderer *r);

JCE_EXTERN_C_END

#endif /* JCE_RENDERER_H */
