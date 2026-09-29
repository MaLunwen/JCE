/*
 * jce_fullscreen_pass.h -- the parts every screen-space effect had a copy of.
 *
 * SSAO, SSR, volumetric fog and SSGI are four instances of one shape: draw a
 * fullscreen quad into an offscreen target, sampling the frame's colour /
 * depth / normal buffers.  Each carried its own copy of the quad, its own
 * create_target(), and its own four-line resize() -- and the dedup audit had
 * been counting them for a while: the resize body was FOUR token-identical
 * copies and create_target THREE before SSGI made it five and four.
 *
 * INTERNAL, not public.  This is engine plumbing between renderer modules,
 * not something a game or the editor should reach: it speaks bgfx handles
 * directly, which the public headers deliberately do not.
 *
 * It stops at the boundary where the four genuinely differ.  Each still owns
 * its own shaders, its own uniforms and its own submit -- those are the
 * effect, and folding them behind a table of names would replace four honest
 * files with one indirection nobody can read.
 */
#ifndef JCE_FULLSCREEN_PASS_H
#define JCE_FULLSCREEN_PASS_H

#include <bgfx/c99/bgfx.h>

#include <stdbool.h>
#include <stdint.h>

/* The unit quad every one of these passes draws, and the layout it needs.
 * Position is 2D NDC and the UV is already y-flipped for a texture whose
 * origin is top-left -- the convention all four were written against. */
typedef struct {
    bgfx_vertex_layout_t        layout;
    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;
} JceFsQuad;

/* Creates the VB/IB.  Both handles are UINT16_MAX-invalid on failure, which
 * jce_fs_quad_destroy tolerates. */
void jce_fs_quad_init(JceFsQuad *q);
void jce_fs_quad_destroy(JceFsQuad *q);
/* set_vertex_buffer + set_index_buffer for the six indices. */
void jce_fs_quad_bind(const JceFsQuad *q);

/* One offscreen colour target plus its texture.  `w`/`h` are kept so resize
 * can answer "unchanged" without the caller tracking it separately -- which
 * is the four-line function that was copied four times. */
typedef struct {
    bgfx_frame_buffer_handle_t fb;
    bgfx_texture_handle_t      tex;
    int                        w, h;
    bgfx_texture_format_t      format;
    uint64_t                   flags;   /* sampler flags, RT flag added here */
} JceFsTarget;

/* Mark both handles invalid.  MUST be called before the first create on a
 * zero-initialised struct: bgfx handle 0 is a VALID handle, so a calloc'd
 * JceFsTarget would otherwise look like it owns framebuffer 0 and the first
 * create would destroy a stranger's. */
void jce_fs_target_init(JceFsTarget *t);

/* (Re)create at w x h.  Destroys any previous framebuffer first.  Records the
 * format and flags so a later resize reproduces exactly this target. */
void jce_fs_target_create(JceFsTarget *t, int w, int h,
                          bgfx_texture_format_t format, uint64_t sampler_flags);
/* No-op when the size is unchanged; otherwise re-creates with the recorded
 * format and flags.  Returns true when it actually re-created. */
bool jce_fs_target_resize(JceFsTarget *t, int w, int h);
void jce_fs_target_destroy(JceFsTarget *t);

#endif /* JCE_FULLSCREEN_PASS_H */
