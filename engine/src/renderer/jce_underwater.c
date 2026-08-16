#include "renderer/jce_underwater.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_shader_load.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>

#define LOG_TAG "underwater"

typedef struct { float pos[2]; float uv[2]; } UwQuadV;

struct JceUnderwater {
    bgfx_vertex_layout_t       layout;
    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;
    bgfx_program_handle_t      prog;
    bgfx_uniform_handle_t      u_sigma;
    bgfx_uniform_handle_t      u_tint;
    bgfx_uniform_handle_t      u_params;
    bgfx_uniform_handle_t      s_depth;
};

JceUnderwater *jce_underwater_create(const JcePakArchive *pak)
{
    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) return NULL;

    JceUnderwater *u = (JceUnderwater *)JCE_CALLOC(1, sizeof(*u));
    if (!u) return NULL;

    bgfx_vertex_layout_begin(&u->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&u->layout, BGFX_ATTRIB_POSITION,  2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&u->layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&u->layout);

    static const UwQuadV verts[4] = {
        { { -1.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  1.0f, -1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f }, { 1.0f, 0.0f } },
        { { -1.0f,  1.0f }, { 0.0f, 0.0f } },
    };
    static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    u->vbh = bgfx_create_vertex_buffer(bgfx_copy(verts, sizeof(verts)),
                                       &u->layout, BGFX_BUFFER_NONE);
    u->ibh = bgfx_create_index_buffer(bgfx_copy(idx, sizeof(idx)), BGFX_BUFFER_NONE);

    /* Reuses the postfx fullscreen vertex shader: the quad and the varying are
     * identical, and a second copy would be a second thing to keep in step. */
    bgfx_shader_handle_t vsh = jce_shader_load_from_pak(pak, "vs_postfx",     sfx, LOG_TAG);
    bgfx_shader_handle_t fsh = jce_shader_load_from_pak(pak, "fs_underwater", sfx, LOG_TAG);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        bgfx_destroy_vertex_buffer(u->vbh);
        bgfx_destroy_index_buffer(u->ibh);
        JCE_FREE(u);
        return NULL;
    }
    u->prog = bgfx_create_program(vsh, fsh, true);

    u->u_sigma  = bgfx_create_uniform("u_uw_sigma",  BGFX_UNIFORM_TYPE_VEC4, 1);
    u->u_tint   = bgfx_create_uniform("u_uw_tint",   BGFX_UNIFORM_TYPE_VEC4, 1);
    u->u_params = bgfx_create_uniform("u_uw_params", BGFX_UNIFORM_TYPE_VEC4, 1);
    u->s_depth  = bgfx_create_uniform("s_uw_depth",  BGFX_UNIFORM_TYPE_SAMPLER, 1);
    return u;
}

void jce_underwater_destroy(JceUnderwater *u)
{
    if (!u) return;
    if (u->prog.idx     != UINT16_MAX) bgfx_destroy_program(u->prog);
    if (u->u_sigma.idx  != UINT16_MAX) bgfx_destroy_uniform(u->u_sigma);
    if (u->u_tint.idx   != UINT16_MAX) bgfx_destroy_uniform(u->u_tint);
    if (u->u_params.idx != UINT16_MAX) bgfx_destroy_uniform(u->u_params);
    if (u->s_depth.idx  != UINT16_MAX) bgfx_destroy_uniform(u->s_depth);
    if (u->vbh.idx      != UINT16_MAX) bgfx_destroy_vertex_buffer(u->vbh);
    if (u->ibh.idx      != UINT16_MAX) bgfx_destroy_index_buffer(u->ibh);
    JCE_FREE(u);
}

void jce_underwater_render(JceUnderwater *u,
                           uint16_t depth_tex_handle,
                           uint16_t dst_fb_idx,
                           uint16_t width, uint16_t height,
                           const JceUnderwaterParams *p,
                           uint16_t view_id)
{
    if (!u || !p || u->prog.idx == UINT16_MAX) return;
    if (depth_tex_handle == UINT16_MAX) return;
    if (width == 0u || height == 0u) return;
    /* No extinction is not "clear water", it is a caller that has nothing to
     * say; drawing two fullscreen quads to multiply by exactly 1 is pure cost. */
    if (!(p->sigma[0] > 0.0f || p->sigma[1] > 0.0f || p->sigma[2] > 0.0f)) return;

    JCE_PROFILE_ZONE_N("Renderer::Underwater");

    bgfx_frame_buffer_handle_t dst = { dst_fb_idx };
    bgfx_set_view_frame_buffer(view_id, dst);
    bgfx_set_view_rect(view_id, 0, 0, width, height);
    /* SEQUENTIAL is load-bearing: both submits share this view, and the
     * multiply MUST land before the add.  Sorted order would let bgfx reorder
     * them into (dst + tint*(1-T)) * T -- dimmer, still smooth, and looking
     * exactly like a different tuning of the same effect rather than a bug. */
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);

    bgfx_texture_handle_t depth = { depth_tex_handle };
    const uint32_t depth_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                               | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                               | BGFX_SAMPLER_MIP_POINT;

    float params[4] = { p->near_plane, p->far_plane,
                        (p->max_path > 0.0f) ? p->max_path : 200.0f, 0.0f };
    float tint[4]   = { p->tint[0], p->tint[1], p->tint[2], 0.0f };

    for (int pass = 0; pass < 2; ++pass) {
        float sigma[4] = { p->sigma[0], p->sigma[1], p->sigma[2],
                           (pass == 0) ? 0.0f : 1.0f };
        bgfx_set_uniform(u->u_sigma,  sigma,  1);
        bgfx_set_uniform(u->u_tint,   tint,   1);
        bgfx_set_uniform(u->u_params, params, 1);
        bgfx_set_texture(0, u->s_depth, depth, depth_flags);
        bgfx_set_vertex_buffer(0, u->vbh, 0, 4);
        bgfx_set_index_buffer(u->ibh, 0, 6);

        const uint64_t blend = (pass == 0)
            /* dst *= T */
            ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO,
                                    BGFX_STATE_BLEND_SRC_COLOR)
            /* dst += tint*(1-T) */
            : BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                    BGFX_STATE_BLEND_ONE);
        bgfx_set_state(BGFX_STATE_WRITE_RGB | blend, 0);
        bgfx_submit(view_id, u->prog, 0, BGFX_DISCARD_ALL);
    }
    JCE_PROFILE_ZONE_END;
}
