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
/* Set MSAA level (0/1=off, else snapped to 2/4/8/16) — toggles the swapchain
 * reset flags + resets the GPU, so a shipped game can apply the authored
 * Project Settings > Graphics MSAA at runtime (from render_settings.json). */
JCE_API void         jce_renderer_set_msaa(JceRenderer *r, int samples);

/* -- Backbuffer screenshot ----------------------------------------- */

/* Request an asynchronous capture of the current frame's backbuffer to `path`.
 * The shot is taken at the next frame and written off the main flow; output
 * format is chosen by the file extension (.png default, .bmp).  Returns false
 * if a capture is already pending or `path` is invalid.  Prefer the
 * application-layer jce_screenshot_save() wrapper. */
JCE_API bool jce_renderer_request_screenshot(const char *path);

/* Like jce_renderer_request_screenshot, but captures a specific offscreen
 * framebuffer (by bgfx handle index) to `path` instead of the backbuffer.
 * On a D3D flip-model swap chain the BACKBUFFER cannot be copied after Present
 * (the shot returns all-black even when the window is focused); capturing an
 * offscreen FBO — e.g. the editor scene-view "bridge" — is reliable.
 * fbo_idx == UINT16_MAX falls back to the backbuffer path. */
JCE_API bool jce_renderer_request_screenshot_fbo(uint16_t fbo_idx, const char *path);

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

/* -- One-shot offscreen-FBO RGBA readback (impostor bake) ----------- */
/* Request an async capture of a specific framebuffer's color attachment,
 * delivered as RAW RGBA8 (alpha preserved — unlike the screenshot path which
 * drops alpha to RGB24).  Pass the bgfx framebuffer handle index (e.g. from
 * jce_offscreen_target_get_frame_buffer or a bake FBO).  `sink` fires once on
 * the render thread at the next frame with the captured pixels (BGRA8 source is
 * converted to RGBA8 by the renderer; `yflip` reported so the caller can flip).
 * Used by the octahedral-impostor bake to read the rendered atlas back to CPU.
 * Returns false if a capture is already pending. One capture in flight. */
typedef void (*JceFboCaptureFn)(void *ud, const void *rgba, uint32_t width,
                                uint32_t height, int yflip);
JCE_API bool jce_renderer_request_fbo_capture(uint16_t fbo_idx,
                                              JceFboCaptureFn sink, void *ud);
JCE_API bool jce_renderer_fbo_capture_pending(void);

/* Headless offscreen capture: blit `src_tex_idx` (an LDR/RGBA8 texture, e.g. the
 * editor scene-view's postfx output) into a read-back staging texture on
 * `blit_view`, read it back to CPU, and write a PNG to `path`.  Unlike
 * jce_renderer_request_screenshot (which only completes on a foreground present),
 * this is a pure GPU->CPU copy that finishes during normal frame processing, so
 * it works with NO window focus (headless).  Poll _poll() each frame until != 0.
 * One capture in flight; returns false if busy or args are invalid.
 * `yflip`: 1 when the SOURCE's readback rows are bottom-up — same contract as
 * _submit_sink: pass 1 for the engine postfx RT, `bgfx caps originBottomLeft`
 * for plain FBO passes (top-down on D3D/VK/Metal, bottom-up only on GL). */
JCE_API bool jce_renderer_readback_capture_submit(uint16_t src_tex_idx, uint16_t blit_view,
                                                  uint16_t w, uint16_t h, const char *path,
                                                  int yflip);
/* -1 = idle, 0 = pending (call again next frame), 1 = wrote PNG, 2 = write failed. */
JCE_API int  jce_renderer_readback_capture_poll(void);

/* Recording variant of _submit: instead of writing a PNG, the poll converts the
 * read-back to BGRA8 and feeds it to the capture sink (video encoder).  Shares
 * the single in-flight slot with _submit; returns false if busy (skip the frame).
 * `yflip`: 1 when the source's readback rows are bottom-up.  This is a property
 * of how the source texture was rendered: pass 1 for the engine postfx RT,
 * `bgfx caps originBottomLeft` for plain FBO passes (the ImGui recording FBO). */
JCE_API bool jce_renderer_readback_capture_submit_sink(uint16_t src_tex_idx,
                                                       uint16_t blit_view,
                                                       uint16_t w, uint16_t h,
                                                       int yflip);

/* While true, video recording is fed by the ImGui renderer reading its offscreen
 * FBO back into the sink (whole editor window), not the backbuffer screen_shot
 * (which is black on D3D flip-model swap chains). */
JCE_API void jce_renderer_set_capture_imgui_mode(bool on);

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
/* Per-instance-tint instanced PBR (large-world-opt P1 #7): vs_pbr_inst_tint +
 * fs_pbr_tint, reading a 5th per-instance vec4 (i_data4 = baseColor tint).
 * INVALID when the variant didn't load → caller draws tinted entities solo. */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_inst_tint(const JceRenderer *r);
/* Texture-diverse instanced variant: vs_pbr_inst_tex_array + fs_pbr_inst_tex_array,
 * reading a 6th per-instance vec4 (i_data5.x = albedo 2D-array layer) with a
 * SAMPLER2DARRAY albedo. INVALID when the variant didn't load (older pak /
 * essl1) → caller keeps texture-diverse entities solo. */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_inst_tex_array(const JceRenderer *r);
/* LOD cross-fade instanced PBR (千万 ②): vs_pbr_inst_fade + fs_pbr_fade —
 * i_data3.w carries the band-transition coverage, the fragment screen-door
 * dithers.  Invalid when the pak predates the variant (fall back to
 * jce_renderer_get_program_pbr_inst = hard band switches). */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_inst_fade(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_skinned(const JceRenderer *r);
/* Forward+ clustered fragment variants (fs_pbr_fwdplus).  Return an INVALID
 * handle when the variant program failed to load (e.g. an older pak) so the
 * caller falls back to the non-variant program. */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_fwdplus(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_inst_fwdplus(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_skinned_fwdplus(const JceRenderer *r);
/* Toon (cel/rim) skinned program (stylized-slice §5.6).  INVALID when the
 * variant didn't load (older pak) → caller falls back to standard skinned PBR. */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_skinned_toon(const JceRenderer *r);
/* Inverted-hull skinned silhouette outline program.  INVALID => no outline. */
JCE_API JceShaderHandle  jce_renderer_get_program_outline_skinned(const JceRenderer *r);
/* When active, the pbr/pbr_inst/pbr_skinned getters above return the Forward+
 * fragment variant (if it loaded), so every existing PBR submit path picks it
 * up.  Set per-frame by the scene renderer from the r.forwardplus cvar.  The
 * caller MUST also bind the cluster texture per submit (jce_forwardplus_bind)
 * so the variant has its froxel data.  Default false => non-variant programs. */
JCE_API void jce_renderer_set_forwardplus_program_active(JceRenderer *r, bool active);
JCE_API bool jce_renderer_get_forwardplus_program_active(const JceRenderer *r);
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
