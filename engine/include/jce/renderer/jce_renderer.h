/*
 * jce_renderer.h  Renderer initialization and per-frame management.
 */

#ifndef JCE_RENDERER_H
#define JCE_RENDERER_H


#include <jce/os/core/jce_defs.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Forward declarations. */
typedef struct JceWindow JceWindow;
typedef struct JceRenderer JceRenderer;
typedef struct JceCamera JceCamera;
typedef struct JceShaderSet JceShaderSet;

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

/* Create a safe fallback renderer using SDL_Renderer. */
JCE_API JceRenderer *jce_renderer_create_fallback(JceWindow *win);

/* Check if the renderer is running in fallback mode. */
JCE_API bool         jce_renderer_is_fallback(const JceRenderer *r);

/* Render the fallback error screen. */
JCE_API void         jce_renderer_render_fallback_frame(const JceRenderer *r);

/* Destroy the renderer: destroys shaders and shuts down the GPU backend. */
JCE_API void         jce_renderer_destroy(JceRenderer *r);

/* -- Per-frame ------------------------------------------------------ */

/* Begin a frame: sets view 0 orthographic projection (2D mode). */
JCE_API void         jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win);

/* Begin a frame with a 3D camera on the given view ID.
   Pass NULL camera for 2D ortho fallback on view 0. */
void         jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
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

void         jce_renderer_dbg_text(uint16_t x, uint16_t y,
                                   uint8_t attr, const char *fmt, ...);

/* -- Queries (for debug HUD) --------------------------------------- */

JCE_API const char  *jce_renderer_get_backend_name(const JceRenderer *r);
JCE_API const char  *jce_renderer_get_gpu_name(const JceRenderer *r);
JCE_API bool         jce_renderer_get_vsync(const JceRenderer *r);
JCE_API void         jce_renderer_set_vsync(JceRenderer *r, bool enabled);
void         jce_renderer_set_vsync_for_size(JceRenderer *r, bool enabled,
                                             uint32_t width, uint32_t height);

/* -- Shader/uniform accessors (for 3D scene rendering) ------------- */

#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_texture_types.h>

/* Color (pos+color) shader program — flat-colored geometry (grid, debug). */
JCE_API JceShaderHandle  jce_renderer_get_program_color(const JceRenderer *r);

/* Mesh (pos+normal+uv) shader program. */
JCE_API JceShaderHandle  jce_renderer_get_program_mesh(const JceRenderer *r);

/* PBR shader programs. */
JCE_API JceShaderHandle  jce_renderer_get_program_pbr(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_pbr_skinned(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_shadow(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_shadow_skinned(const JceRenderer *r);
JCE_API JceShaderHandle  jce_renderer_get_program_terrain(const JceRenderer *r);

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

JCE_EXTERN_C_END

#endif /* JCE_RENDERER_H */
