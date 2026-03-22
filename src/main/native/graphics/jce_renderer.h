/*
 * jce_renderer.h  bgfx renderer initialization and per-frame management.
 */

#ifndef JCE_RENDERER_H
#define JCE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

/* Forward declarations. */
typedef struct JceWindow   JceWindow;
typedef struct PakArchive  PakArchive;
typedef struct JceRenderer JceRenderer;
typedef struct JceCamera   JceCamera;

/* -- Renderer configuration ---------------------------------------- */

typedef struct JceRendererConfig {
    int      backend;       /* JceRendererBackend value (0 = auto) */
    bool     vsync;
    bool     debug_text;
    uint32_t clear_color;   /* RGBA, e.g. 0x000000FF */
} JceRendererConfig;

/* -- Lifecycle ------------------------------------------------------ */

/* Create the renderer: initializes bgfx, loads the color shader,
   sets up the vertex layout and view 0 clear state.
   Returns NULL on failure. */
JceRenderer *jce_renderer_create(JceWindow *win, const PakArchive *pak,
                                  const JceRendererConfig *cfg);

/* Create a safe fallback renderer using SDL_Renderer. */
JceRenderer *jce_renderer_create_fallback(JceWindow *win);

/* Check if the renderer is running in fallback mode. */
bool         jce_renderer_is_fallback(const JceRenderer *r);

/* Render the fallback error screen. */
void         jce_renderer_render_fallback_frame(const JceRenderer *r);

/* Destroy the renderer: destroys the shader program and shuts down bgfx. */
void         jce_renderer_destroy(JceRenderer *r);

/* -- Per-frame ------------------------------------------------------ */

/* Begin a frame: sets view 0 orthographic projection (2D mode). */
void         jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win);

/* Begin a frame with a 3D camera on the given view ID.
   Pass NULL camera for 2D ortho fallback on view 0. */
void         jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
                                          const JceCamera *cam, uint16_t view_id);

/* End a frame: calls bgfx_frame. */
void         jce_renderer_end_frame(const JceRenderer *r);

/* -- Events --------------------------------------------------------- */

/* Handle window resize: calls bgfx_reset. */
void         jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h);

/* Re-bind the native window handle and reset bgfx.
   Required after Android background/foreground cycle (ANativeWindow is recreated). */
void         jce_renderer_rebind_platform(JceRenderer *r, JceWindow *win);

/* -- Debug text ----------------------------------------------------- */

void         jce_renderer_dbg_text(uint16_t x, uint16_t y,
                                   uint8_t attr, const char *fmt, ...);

/* -- Queries (for debug HUD) --------------------------------------- */

const char  *jce_renderer_get_backend_name(const JceRenderer *r);
const char  *jce_renderer_get_gpu_name(const JceRenderer *r);
bool         jce_renderer_get_vsync(const JceRenderer *r);

/* -- Shader/uniform accessors (for 3D scene rendering) ------------- */

#include "jce_gfx_types.h"

/* Mesh (pos+normal+uv) shader program. */
JceShaderHandle  jce_renderer_get_program_mesh(const JceRenderer *r);

/* Texture sampler uniform (s_texColor). */
JceUniformHandle jce_renderer_get_tex_uniform(const JceRenderer *r);

#endif /* JCE_RENDERER_H */
