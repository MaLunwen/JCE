/*
 * jce_imgui_renderer.cpp  ImGui renderer backend implementation (bgfx).
 *
 * Renders ImGui draw lists using bgfx transient vertex/index buffers.
 * Each draw command is submitted on JCE_VIEW_IMGUI (view 255).
 */

#include <jce/ui/jce_imgui_renderer.h>

#include <bgfx/c99/bgfx.h>
#include <jce/tools/jce_imgui.hpp>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_views.h>
}

#define LOG_TAG "imgui_renderer"

/* ── Static state ──────────────────────────────────────────────────── */

static struct {
    bgfx_program_handle_t  program;
    bgfx_uniform_handle_t  tex_uniform;
    bgfx_vertex_layout_t   vertex_layout;
    uint8_t                view_id;
    bool                   initialized;
} s_ctx;

/* Ortho projection now delegates to engine API (jce_m4_ortho). */

/* ── Dynamic textures (ImGui 1.92 RendererHasTextures) ──────────────────
 * ImGui 1.92 owns the font atlas as one or more ImTextureData objects and
 * rasterizes glyphs ON DEMAND (no upfront full-atlas bake).  Each frame the
 * backend services create/update/destroy requests carried in
 * ImDrawData::Textures.  This removes the ~hundreds-of-ms startup cost of
 * baking every CJK/Korean glyph that the editor might never draw — they are
 * now rasterized only when first displayed.  The bgfx texture handle index
 * is stored verbatim in ImTextureData::TexID (same encoding the editor uses
 * for app textures via ImGui::Image), so the draw loop binding is unchanged. */

/* ImGui 1.92 stores a bgfx texture handle idx in ImTextureID, but
 * ImTextureID_Invalid == 0 collides with the perfectly valid bgfx idx 0
 * (first allocated handle).  Encode every idx as (idx+1) and decode (id-1) so
 * 0 stays reserved for "no texture" — otherwise a font atlas / app texture
 * landing on idx 0 would leak (never freed), corrupt bgfx handle 0 on update,
 * or trip ImGui's GetTexID assert (audit Round-3 P2-B).  Editor ImGui::Image()
 * producers apply the same +1 at their end. */
static inline ImTextureID imtex_from_bgfx(uint16_t idx)
{
    return (ImTextureID)(uintptr_t)((uint32_t)idx + 1u);
}
static inline uint16_t imtex_to_bgfx(ImTextureID id)
{
    return id == ImTextureID_Invalid ? (uint16_t)UINT16_MAX
                                     : (uint16_t)((uintptr_t)id - 1u);
}

static void destroy_imgui_texture(ImTextureData *tex)
{
    bgfx_texture_handle_t h;
    h.idx = imtex_to_bgfx(tex->GetTexID());
    if (h.idx != UINT16_MAX)
        bgfx_destroy_texture(h);
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

static void update_imgui_texture(ImTextureData *tex)
{
    if (tex->Status == ImTextureStatus_WantCreate) {
        /* Re-create path (e.g. the atlas grew to fit more on-demand glyphs):
           free the previous GPU texture first. */
        if (tex->GetTexID() != ImTextureID_Invalid) {
            bgfx_texture_handle_t old;
            old.idx = imtex_to_bgfx(tex->GetTexID());
            if (old.idx != UINT16_MAX) bgfx_destroy_texture(old);
            tex->SetTexID(ImTextureID_Invalid);
        }
        /* Create the atlas texture MUTABLE (_mem == NULL): a bgfx texture
           created WITH _mem is IMMUTABLE and cannot be updated, which would
           leave every glyph rasterized AFTER this first upload (on-demand
           WantUpdates: menus, dialogs, newly shown text) blank.  Create empty,
           then upload the full current contents via an update.  ImGui's
           default font format is RGBA32 → bgfx RGBA8. */
        bgfx_texture_handle_t h = bgfx_create_texture_2d(
            (uint16_t)tex->Width, (uint16_t)tex->Height,
            false, 1,
            BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
            BGFX_SAMPLER_MIP_POINT,
            NULL);
        const bgfx_memory_t *mem =
            bgfx_copy(tex->GetPixels(), (uint32_t)tex->GetSizeInBytes());
        bgfx_update_texture_2d(h, 0, 0, 0, 0,
            (uint16_t)tex->Width, (uint16_t)tex->Height, mem, UINT16_MAX);
        tex->SetTexID(imtex_from_bgfx(h.idx));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        /* Re-upload the bounding box of all queued sub-rect updates (glyphs
         * newly rasterized this frame).  Pack the rect's rows tightly into a
         * bgfx buffer (the source Pixels are full-atlas-width strided). */
        bgfx_texture_handle_t h;
        h.idx = imtex_to_bgfx(tex->GetTexID());
        const ImTextureRect &r = tex->UpdateRect;
        if (h.idx != UINT16_MAX && r.w > 0 && r.h > 0) {
            const uint32_t bpp   = (uint32_t)tex->BytesPerPixel;
            const uint32_t pitch = (uint32_t)r.w * bpp;
            const bgfx_memory_t *mem = bgfx_alloc(pitch * (uint32_t)r.h);
            for (int row = 0; row < r.h; ++row) {
                memcpy(mem->data + (uint32_t)row * pitch,
                       tex->GetPixelsAt(r.x, r.y + row),
                       pitch);
            }
            bgfx_update_texture_2d(h, 0, 0,
                (uint16_t)r.x, (uint16_t)r.y,
                (uint16_t)r.w, (uint16_t)r.h, mem, UINT16_MAX);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy &&
               tex->UnusedFrames > 0) {
        destroy_imgui_texture(tex);
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_imgui_renderer_init(const JcePakArchive *pak, uint8_t view_id)
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

    /* Tell ImGui we can handle per-draw VtxOffset (reordered draw commands
       such as modal dim overlays) AND dynamic textures (ImGui 1.92 creates,
       updates and destroys the font atlas on demand via ImDrawData::Textures
       — no upfront full-atlas bake; glyphs rasterize when first drawn).
       With RendererHasTextures set, the font texture is created lazily on
       the first frame's update_imgui_texture(), so there is no
       create_font_texture() call here anymore. */
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    s_ctx.initialized = true;
    LOG_SUCCESS(LOG_TAG, "initialized (view %u)", view_id);
    return true;
}

void jce_imgui_renderer_shutdown(void)
{
    if (!s_ctx.initialized) return;

    /* Destroy any GPU textures ImGui created (font atlas + any others on its
       texture list).  Guarded on a live context. */
    if (ImGui::GetCurrentContext() != NULL) {
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
        for (ImTextureData *tex : pio.Textures) {
            if (tex && tex->GetTexID() != ImTextureID_Invalid)
                destroy_imgui_texture(tex);
        }
    }

    if (s_ctx.tex_uniform.idx != UINT16_MAX)
        bgfx_destroy_uniform(s_ctx.tex_uniform);
    if (s_ctx.program.idx != UINT16_MAX)
        bgfx_destroy_program(s_ctx.program);

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.tex_uniform.idx  = UINT16_MAX;
    s_ctx.program.idx      = UINT16_MAX;
    s_ctx.initialized      = false;

    LOG_INFO(LOG_TAG, "shutdown");
}

void jce_imgui_renderer_setup_view(uint16_t width, uint16_t height)
{
    if (!s_ctx.initialized) return;

    bgfx_set_view_clear(s_ctx.view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(s_ctx.view_id, 0, 0, width, height);
    bgfx_set_view_mode(s_ctx.view_id, BGFX_VIEW_MODE_SEQUENTIAL);

    /* Orthographic projection: top-left origin, pixel coordinates. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    jce_mat4 ortho = jce_m4_ortho(
        0.0f, (float)width,
        (float)height, 0.0f,
        0.0f, 1000.0f,
        caps->homogeneousDepth);

    bgfx_set_view_transform(s_ctx.view_id, NULL, &ortho);
}

void jce_imgui_renderer_draw(void)
{
    if (!s_ctx.initialized) return;

    ImDrawData *draw_data = ImGui::GetDrawData();
    if (!draw_data)
        return;

    /* Service ImGui 1.92 dynamic-texture requests (create/update/destroy)
       BEFORE submitting draw commands that reference them — the font atlas
       grows here as new glyphs are rasterized on demand.  Must run even when
       there are no vertices yet (the very first frame creates the atlas). */
    if (draw_data->Textures != NULL) {
        for (ImTextureData *tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK)
                update_imgui_texture(tex);
        }
    }

    if (draw_data->TotalVtxCount == 0)
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

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd *pcmd = &cmd_list->CmdBuffer[cmd_i];

            if (pcmd->UserCallback) {
                pcmd->UserCallback(cmd_list, pcmd);
                continue;
            }

            /* Scissor rect — clamp to [0, 65535] like the official bgfx
               ImGui example.  Negative clip-rect coords happen when
               ImGui extends a clip rect beyond the viewport (e.g. the
               modal dim overlay uses viewport ± 1 px). */
            ImVec2 clip_off = draw_data->DisplayPos;
            float clip_min_x = pcmd->ClipRect.x - clip_off.x;
            float clip_min_y = pcmd->ClipRect.y - clip_off.y;
            float clip_max_x = pcmd->ClipRect.z - clip_off.x;
            float clip_max_y = pcmd->ClipRect.w - clip_off.y;
            if (clip_max_x <= clip_min_x || clip_max_y <= clip_min_y)
                continue;
            uint16_t sx = (uint16_t)(clip_min_x > 0.0f ? clip_min_x : 0.0f);
            uint16_t sy = (uint16_t)(clip_min_y > 0.0f ? clip_min_y : 0.0f);
            uint16_t sw = (uint16_t)((clip_max_x < 65535.0f ? clip_max_x : 65535.0f) - (float)sx);
            uint16_t sh = (uint16_t)((clip_max_y < 65535.0f ? clip_max_y : 65535.0f) - (float)sy);
            bgfx_set_scissor(sx, sy, sw, sh);

            /* Bind texture. */
            bgfx_texture_handle_t tex;
            tex.idx = imtex_to_bgfx(pcmd->GetTexID());
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

            /* Set vertex/index buffers and submit.
               Use pcmd->VtxOffset / IdxOffset so that reordered draw
               commands (e.g. modal dim overlay via push_front) render
               with the correct geometry. */
            bgfx_set_transient_vertex_buffer(
                0, &tvb, pcmd->VtxOffset,
                num_vertices - pcmd->VtxOffset);
            bgfx_set_transient_index_buffer(
                &tib, pcmd->IdxOffset, pcmd->ElemCount);
            bgfx_submit(
                s_ctx.view_id, s_ctx.program,
                0, BGFX_DISCARD_ALL);
        }
    }
}

void jce_imgui_renderer_rebuild_fonts(void)
{
    /* No-op under ImGui 1.92 dynamic textures: changing the atlas (Clear +
       AddFont in jce_editor_load_fonts) automatically marks the old font
       ImTextureData WantDestroy and the new one WantCreate, which the next
       frame's update_imgui_texture() services.  Kept for API compatibility
       with callers (e.g. font/locale changes). */
    (void)s_ctx;
}
