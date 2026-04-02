/*
 * jce_imgui_bgfx.cpp  ImGui bgfx renderer backend implementation.
 *
 * Renders ImGui draw lists using bgfx transient vertex/index buffers.
 * Each draw command is submitted on JCE_VIEW_IMGUI (view 255).
 */

#include "jce_imgui_bgfx.h"

#include <imgui.h>
#include <bgfx/c99/bgfx.h>
#include <string.h>

extern "C" {
#include <jce/graphics/jce_shaders.h>
#include <jce/graphics/jce_views.h>
#include <jce/core/jce_log.h>
}

#define LOG_TAG "imgui_bgfx"

/* ── Static state ──────────────────────────────────────────────────── */

static struct {
    bgfx_program_handle_t  program;
    bgfx_texture_handle_t  font_texture;
    bgfx_uniform_handle_t  tex_uniform;
    bgfx_vertex_layout_t   vertex_layout;
    uint8_t                view_id;
    bool                   initialized;
} s_ctx;

/* ── Ortho projection (ImGui coordinate system) ────────────────────── */

static void make_ortho(float *out,
                       float left, float right,
                       float bottom, float top,
                       float z_near, float z_far,
                       bool homogeneous_depth)
{
    memset(out, 0, 16 * sizeof(float));
    out[0]  = 2.0f / (right - left);
    out[5]  = 2.0f / (top - bottom);
    out[12] = (left + right) / (left - right);
    out[13] = (top + bottom) / (bottom - top);

    if (homogeneous_depth) {
        out[10] = 2.0f / (z_near - z_far);
        out[14] = (z_near + z_far) / (z_near - z_far);
    } else {
        out[10] = 1.0f / (z_near - z_far);
        out[14] = z_near / (z_near - z_far);
    }
    out[15] = 1.0f;
}

/* ── Font atlas ────────────────────────────────────────────────────── */

static void create_font_texture(void)
{
    ImGuiIO &io = ImGui::GetIO();
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    const bgfx_memory_t *mem =
        bgfx_copy(pixels, (uint32_t)(width * height * 4));

    s_ctx.font_texture = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height,
        false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
        BGFX_SAMPLER_MIP_POINT,
        mem);

    io.Fonts->SetTexID((ImTextureID)(uintptr_t)s_ctx.font_texture.idx);
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_imgui_bgfx_init(const PakArchive *pak, uint8_t view_id)
{
    s_ctx.view_id = view_id;

    /* Load shader program from PAK. */
    JceShaderHandle sh = shader_load_program(pak, "imgui");
    if (!jce_shader_valid(sh)) {
        LOG_ERROR(LOG_TAG, "failed to load imgui shader");
        return false;
    }
    s_ctx.program.idx = sh.idx;

    /* Sampler uniform (same name as engine textures). */
    s_ctx.tex_uniform =
        bgfx_create_uniform("s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Vertex layout matching ImDrawVert:
       { ImVec2 pos; ImVec2 uv; ImU32 col; } = 20 bytes. */
    bgfx_vertex_layout_begin(&s_ctx.vertex_layout,
        bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_ctx.vertex_layout,
        BGFX_ATTRIB_POSITION, 2,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s_ctx.vertex_layout,
        BGFX_ATTRIB_TEXCOORD0, 2,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s_ctx.vertex_layout,
        BGFX_ATTRIB_COLOR0, 4,
        BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&s_ctx.vertex_layout);

    /* Create default font atlas texture. */
    create_font_texture();

    s_ctx.initialized = true;
    LOG_SUCCESS(LOG_TAG, "initialized (view %u)", view_id);
    return true;
}

void jce_imgui_bgfx_shutdown(void)
{
    if (!s_ctx.initialized) return;

    if (s_ctx.font_texture.idx != UINT16_MAX)
        bgfx_destroy_texture(s_ctx.font_texture);
    if (s_ctx.tex_uniform.idx != UINT16_MAX)
        bgfx_destroy_uniform(s_ctx.tex_uniform);
    if (s_ctx.program.idx != UINT16_MAX)
        bgfx_destroy_program(s_ctx.program);

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.font_texture.idx = UINT16_MAX;
    s_ctx.tex_uniform.idx  = UINT16_MAX;
    s_ctx.program.idx      = UINT16_MAX;
    s_ctx.initialized      = false;

    LOG_INFO(LOG_TAG, "shutdown");
}

void jce_imgui_bgfx_setup_view(uint16_t width, uint16_t height)
{
    if (!s_ctx.initialized) return;

    bgfx_set_view_clear(s_ctx.view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(s_ctx.view_id, 0, 0, width, height);
    bgfx_set_view_mode(s_ctx.view_id, BGFX_VIEW_MODE_SEQUENTIAL);

    /* Orthographic projection: top-left origin, pixel coordinates. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    float ortho[16];
    make_ortho(ortho,
        0.0f, (float)width,
        (float)height, 0.0f,
        0.0f, 1000.0f,
        caps->homogeneousDepth);

    bgfx_set_view_transform(s_ctx.view_id, NULL, ortho);
}

void jce_imgui_bgfx_render_draw_data(void)
{
    if (!s_ctx.initialized) return;

    ImDrawData *draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->TotalVtxCount == 0)
        return;

    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList *cmd_list = draw_data->CmdLists[n];

        uint32_t num_vertices = (uint32_t)cmd_list->VtxBuffer.Size;
        uint32_t num_indices  = (uint32_t)cmd_list->IdxBuffer.Size;

        /* Check transient buffer availability. */
        uint32_t avail_vtx =
            bgfx_get_avail_transient_vertex_buffer(
                num_vertices, &s_ctx.vertex_layout);
        uint32_t avail_idx =
            bgfx_get_avail_transient_index_buffer(
                num_indices, sizeof(ImDrawIdx) == 4);

        if (avail_vtx < num_vertices || avail_idx < num_indices)
            break;

        bgfx_transient_vertex_buffer_t tvb;
        bgfx_transient_index_buffer_t  tib;
        bgfx_alloc_transient_vertex_buffer(
            &tvb, num_vertices, &s_ctx.vertex_layout);
        bgfx_alloc_transient_index_buffer(
            &tib, num_indices, sizeof(ImDrawIdx) == 4);

        memcpy(tvb.data, cmd_list->VtxBuffer.Data,
               num_vertices * sizeof(ImDrawVert));
        memcpy(tib.data, cmd_list->IdxBuffer.Data,
               num_indices * sizeof(ImDrawIdx));

        uint32_t offset = 0;
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd *pcmd = &cmd_list->CmdBuffer[cmd_i];

            if (pcmd->UserCallback) {
                pcmd->UserCallback(cmd_list, pcmd);
                continue;
            }

            /* Scissor rect (clip to viewport). */
            ImVec2 clip_off = draw_data->DisplayPos;
            float cx = pcmd->ClipRect.x - clip_off.x;
            float cy = pcmd->ClipRect.y - clip_off.y;
            float cw = pcmd->ClipRect.z - clip_off.x - cx;
            float ch = pcmd->ClipRect.w - clip_off.y - cy;

            if (cw <= 0.0f || ch <= 0.0f) {
                offset += pcmd->ElemCount;
                continue;
            }

            bgfx_set_scissor(
                (uint16_t)cx, (uint16_t)cy,
                (uint16_t)cw, (uint16_t)ch);

            /* Bind texture. */
            bgfx_texture_handle_t tex;
            tex.idx = (uint16_t)(uintptr_t)pcmd->GetTexID();
            bgfx_set_texture(
                0, s_ctx.tex_uniform, tex, UINT32_MAX);

            /* Render state: alpha blending, write RGB+A, MSAA. */
            uint64_t state =
                BGFX_STATE_WRITE_RGB |
                BGFX_STATE_WRITE_A |
                BGFX_STATE_MSAA;
            state |= BGFX_STATE_BLEND_FUNC(
                BGFX_STATE_BLEND_SRC_ALPHA,
                BGFX_STATE_BLEND_INV_SRC_ALPHA);
            bgfx_set_state(state, 0);

            /* Set vertex/index buffers and submit. */
            bgfx_set_transient_vertex_buffer(
                0, &tvb, 0, num_vertices);
            bgfx_set_transient_index_buffer(
                &tib, offset, pcmd->ElemCount);
            bgfx_submit(
                s_ctx.view_id, s_ctx.program,
                0, BGFX_DISCARD_ALL);

            offset += pcmd->ElemCount;
        }
    }
}

void jce_imgui_bgfx_rebuild_fonts(void)
{
    if (!s_ctx.initialized) return;

    if (s_ctx.font_texture.idx != UINT16_MAX) {
        bgfx_destroy_texture(s_ctx.font_texture);
        s_ctx.font_texture.idx = UINT16_MAX;
    }
    create_font_texture();
}
