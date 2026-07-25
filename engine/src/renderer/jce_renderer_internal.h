/*
 * jce_renderer_internal.h  Renderer-internal accessors.
 *
 * Only for use within the renderer/ module (e.g. jce_primitives.c).
 * External modules should use the public API in jce_renderer.h.
 */

#ifndef JCE_RENDERER_INTERNAL_H
#define JCE_RENDERER_INTERNAL_H

#include <jce/renderer/jce_renderer.h>

#include <bgfx/c99/bgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Renderer-internal accessors (not for use outside renderer/) --- */

/* Color (pos+color) vertex layout and shader program. */
const bgfx_vertex_layout_t *jce_renderer_get_layout(const JceRenderer *r);
bgfx_program_handle_t       jce_renderer_get_program(const JceRenderer *r);

/* Textured (pos+color+uv) vertex layout and shader program. */
const bgfx_vertex_layout_t *jce_renderer_get_layout_textured(const JceRenderer *r);
bgfx_program_handle_t       jce_renderer_get_program_textured(const JceRenderer *r);

/* Lighting uniforms (used by jce_lighting.c). */
bgfx_uniform_handle_t       jce_renderer_get_light_dir_uniform(const JceRenderer *r);
bgfx_uniform_handle_t       jce_renderer_get_light_color_uniform(const JceRenderer *r);

/* PBR shader programs (bgfx handles). */
bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr_skinned(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow_skinned(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_terrain(const JceRenderer *r);

/* Render-target scrub helpers (moved out of the public jce_views.h because
 * jce_zero_init_mem returns a bgfx type). No-op stub kept for ABI; the real
 * workhorse returns zeroed bgfx memory for bgfx_create_texture_2d's _mem so
 * a fresh texture starts solid-black instead of leftover VRAM. */
void jce_clear_freshly_created_fbo(unsigned short fb_idx,
                                   unsigned short width,
                                   unsigned short height);
const bgfx_memory_t *jce_zero_init_mem(unsigned int size_bytes);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDERER_INTERNAL_H */
