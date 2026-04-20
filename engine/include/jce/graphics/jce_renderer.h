/*
 * jce_renderer.h  Renderer initialization and per-frame management.
 */

#ifndef JCE_RENDERER_H
#define JCE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations. */
typedef struct JceWindow    JceWindow;
typedef struct JceRenderer  JceRenderer;
typedef struct JceCamera    JceCamera;
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
JceRenderer *jce_renderer_create(JceWindow *win,  const JceRendererConfig *cfg);

/* Attach pre-loaded shaders. Must be called after create
   and before the first frame. */
void jce_renderer_set_shaders(JceRenderer *r, const JceShaderSet *shaders);

/* Create a safe fallback renderer using SDL_Renderer. */
JceRenderer *jce_renderer_create_fallback(JceWindow *win);

/* Check if the renderer is running in fallback mode. */
bool         jce_renderer_is_fallback(const JceRenderer *r);

/* Render the fallback error screen. */
void         jce_renderer_render_fallback_frame(const JceRenderer *r);

/* Destroy the renderer: destroys shaders and shuts down the GPU backend. */
void         jce_renderer_destroy(JceRenderer *r);

/* -- Per-frame ------------------------------------------------------ */

/* Begin a frame: sets view 0 orthographic projection (2D mode). */
void         jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win);

/* Begin a frame with a 3D camera on the given view ID.
   Pass NULL camera for 2D ortho fallback on view 0. */
void         jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
                                          const JceCamera *cam, uint16_t view_id);

/* End a frame: submits all queued draw calls to the GPU. */
void         jce_renderer_end_frame(const JceRenderer *r);

/* -- Events --------------------------------------------------------- */

/* Handle window resize: resets the GPU swap chain. */
void         jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h);

/* Re-bind the native window handle and reset the GPU backend.
   Required after Android background/foreground cycle (ANativeWindow is recreated). */
void         jce_renderer_rebind_platform(JceRenderer *r, JceWindow *win);

/* -- Debug text ----------------------------------------------------- */

void         jce_renderer_dbg_text(uint16_t x, uint16_t y,
                                   uint8_t attr, const char *fmt, ...);

/* -- Queries (for debug HUD) --------------------------------------- */

const char  *jce_renderer_get_backend_name(const JceRenderer *r);
const char  *jce_renderer_get_gpu_name(const JceRenderer *r);
bool         jce_renderer_get_vsync(const JceRenderer *r);
void         jce_renderer_set_vsync(JceRenderer *r, bool enabled);
void         jce_renderer_set_vsync_for_size(JceRenderer *r, bool enabled,
                                             uint32_t width, uint32_t height);

/* -- Shader/uniform accessors (for 3D scene rendering) ------------- */

#include <jce/graphics/jce_gfx_types.h>
#include <jce/graphics/jce_texture_types.h>

/* Color (pos+color) shader program — flat-colored geometry (grid, debug). */
JceShaderHandle  jce_renderer_get_program_color(const JceRenderer *r);

/* Mesh (pos+normal+uv) shader program. */
JceShaderHandle  jce_renderer_get_program_mesh(const JceRenderer *r);

/* PBR shader programs. */
JceShaderHandle  jce_renderer_get_program_pbr(const JceRenderer *r);
JceShaderHandle  jce_renderer_get_program_pbr_skinned(const JceRenderer *r);
JceShaderHandle  jce_renderer_get_program_shadow(const JceRenderer *r);
JceShaderHandle  jce_renderer_get_program_shadow_skinned(const JceRenderer *r);

/* Texture sampler uniform (s_texColor). */
JceUniformHandle jce_renderer_get_tex_uniform(const JceRenderer *r);

/* -- Transform / texture binding ------------------------------------- */

/* Set the model-to-world transform for the next draw call. */
void jce_renderer_set_transform(const float *mtx);

/* Bind a texture to a sampler stage for the next draw. */
void jce_renderer_bind_texture(const JceRenderer *r, uint8_t stage, JceTexture tex);

/* Debug text: single cell with attribute byte. */
void jce_renderer_dbg_text_attr(uint16_t x, uint16_t y, uint8_t attr, const char *str);

/* -- Wireframe debug mode ------------------------------------------ */

/* Toggle wireframe rendering (F3+V debug feature). */
void jce_renderer_set_wireframe(JceRenderer *r, bool enabled);
bool jce_renderer_get_wireframe(const JceRenderer *r);

/* -- GPU capability queries --------------------------------------- */

/* Returns true if the GPU uses bottom-left framebuffer origin (OpenGL).
   Useful for UV flipping when displaying FBO textures in UI. */
bool jce_renderer_origin_bottom_left(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDERER_H */
