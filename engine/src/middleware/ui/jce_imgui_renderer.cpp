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
#include <jce/os/core/jce_timer.h>  /* jce_time_ticks_ms: capture pacing */
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_renderer.h>   /* jce_renderer_readback_capture_submit */
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_views.h>
}
#include <stdio.h>   /* snprintf */
#include <stdlib.h>  /* getenv: JCE_REC_FPS capture-rate cap */


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

/* ── Whole-window capture (F12 screenshot / F9 recording) ────────────────────
 * The D3D flip-model swap chain cannot be screen-shot reliably: bgfx presents
 * one buffer but bgfx_request_screen_shot resolves the discarded one, so the
 * backbuffer capture comes back BLACK (reproduced: avg_lum=0).  Instead, on a
 * capture frame we render the ImGui view into an offscreen RGBA16F FBO and read
 * THAT back on the next frame via the proven jce_renderer_readback path (a pure
 * GPU->CPU copy, immune to flip-model / focus).  Cost: the captured frame shows
 * one black backbuffer (the UI went to the FBO) — imperceptible for a snapshot. */
static struct {
    bool                       request;          /* a capture was asked for         */
    bool                       readback_pending; /* FBO rendered; read it next frame */
    char                       path[512];
    bgfx_frame_buffer_handle_t fb;
    bgfx_texture_handle_t      tex;              /* fb colour attachment (blit src) */
    uint16_t                   w, h;
} s_cap = { false, false, { 0 }, { UINT16_MAX }, { UINT16_MAX }, 0, 0 };

extern "C" void jce_imgui_renderer_request_capture(const char *path)
{
    if (!path || !path[0] || s_cap.request || s_cap.readback_pending)
        return;
    snprintf(s_cap.path, sizeof s_cap.path, "%s", path);
    s_cap.request = true;
}

/* ── Whole-window video recording (F9) ───────────────────────────────────────
 * While active, the UI is rendered a SECOND time into an offscreen FBO (view+1),
 * which is read back each frame into the video sink as BGRA8.  The normal
 * backbuffer pass (view) is untouched, so the editor display stays live. */
static struct {
    bool                       active;
    bgfx_frame_buffer_handle_t fb;
    bgfx_texture_handle_t      tex;
    uint16_t                   w, h;
} s_rec = { false, { UINT16_MAX }, { UINT16_MAX }, 0, 0 };

extern "C" void jce_imgui_renderer_set_recording(bool on) { s_rec.active = on; }

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

    /* ── Whole-window capture state machine (see s_cap) ──────────────────
     * Default: render ImGui to the backbuffer (display path, unchanged).
     * Capture frame: render to an offscreen FBO instead, then read it back
     * the following frame (the FBO holds the fully-composited UI). */
    bgfx_frame_buffer_handle_t target     = { UINT16_MAX };   /* backbuffer */
    uint16_t                   clear_flags = BGFX_CLEAR_NONE;
    uint32_t                   clear_rgba  = 0;

    if (s_cap.readback_pending) {
        /* Last frame rendered the UI into s_cap.fb; it is complete now. Blit it
         * into a READ_BACK staging texture and read it back to a PNG.  The blit
         * is ordered on the imgui view; the FBO is not written this frame, so
         * there is no read/write hazard.  This frame renders to the backbuffer. */
        /* Plain FBO source: rows come back per the backend's texture origin —
         * bottom-up only on GL (same contract as the F9 recording path below;
         * hardcoded bottom-up inverted D3D11 F12/WINCAP PNGs). */
        jce_renderer_readback_capture_submit(s_cap.tex.idx, s_ctx.view_id,
                                             s_cap.w, s_cap.h, s_cap.path,
                                             bgfx_get_caps()->originBottomLeft ? 1 : 0);
        s_cap.readback_pending = false;
    } else if (s_cap.request) {
        if (s_cap.fb.idx == UINT16_MAX || s_cap.w != width || s_cap.h != height) {
            if (s_cap.fb.idx != UINT16_MAX) bgfx_destroy_frame_buffer(s_cap.fb);
            /* RGBA16F so the existing readback (RGBA16F staging -> RGBA8) applies. */
            s_cap.tex = bgfx_create_texture_2d(width, height, false, 1,
                BGFX_TEXTURE_FORMAT_RGBA16F,
                BGFX_TEXTURE_RT |
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL);
            bgfx_texture_handle_t ths[1] = { s_cap.tex };
            s_cap.fb = bgfx_create_frame_buffer_from_handles(1, ths, true);
            s_cap.w = width; s_cap.h = height;
        }
        if (BGFX_HANDLE_IS_VALID(s_cap.fb)) {
            target                 = s_cap.fb;
            clear_flags            = BGFX_CLEAR_COLOR;
            clear_rgba             = 0x1e1e1effu;  /* editor bg behind any UI gaps */
            s_cap.readback_pending = true;         /* read it back next frame      */
        }
        s_cap.request = false;
    }

    bgfx_set_view_frame_buffer(s_ctx.view_id, target);
    bgfx_set_view_clear(s_ctx.view_id, clear_flags, clear_rgba, 1.0f, 0);
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

    /* Recording: set up the parallel offscreen pass (view+1 -> s_rec.fb).  The
     * draws are double-submitted there in jce_imgui_renderer_draw, and the FBO is
     * read back into the video sink.  Display path (view, above) is unchanged. */
    if (s_rec.active) {
        if (s_rec.fb.idx == UINT16_MAX || s_rec.w != width || s_rec.h != height) {
            if (s_rec.fb.idx != UINT16_MAX) bgfx_destroy_frame_buffer(s_rec.fb);
            s_rec.tex = bgfx_create_texture_2d(width, height, false, 1,
                BGFX_TEXTURE_FORMAT_RGBA16F,
                BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL);
            bgfx_texture_handle_t ths[1] = { s_rec.tex };
            s_rec.fb = bgfx_create_frame_buffer_from_handles(1, ths, true);
            s_rec.w = width; s_rec.h = height;
        }
        const uint16_t rv = (uint16_t)(s_ctx.view_id + 1);
        bgfx_set_view_frame_buffer(rv, s_rec.fb);
        bgfx_set_view_clear(rv, BGFX_CLEAR_COLOR, 0x1e1e1effu, 1.0f, 0);
        bgfx_set_view_rect(rv, 0, 0, width, height);
        bgfx_set_view_mode(rv, BGFX_VIEW_MODE_SEQUENTIAL);
        bgfx_set_view_transform(rv, NULL, &ortho);
    }
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

        /* Allocate BOTH transient buffers via bgfx's combined atomic helper.
         *
         * Root cause of the streaming-churn crash (ACCESS_VIOLATION surfacing in
         * cJSON_malloc = heap corruption from a write elsewhere): the previous
         * code checked vertex + index availability with two SEPARATE
         * bgfx_get_avail_transient_*_buffer calls, then issued two SEPARATE
         * bgfx_alloc_transient_*_buffer calls.  The per-frame transient buffers
         * are a single fixed-size pool (bgfx default 6 MiB, shared by the scene
         * renderer's instancing/debug-draw AND ImGui).  When a large editor UI
         * (the World-Streaming hierarchy with 200+ chunk groups, churning as
         * cells load/unload) drove the ImGui geometry near that limit, the avail
         * check could report a full fit while the subsequent alloc, after the
         * intervening index alloc advanced the SHARED offset and stride-alignment
         * rounded it up, granted a buffer that ended right at the pool boundary —
         * and the memcpy of the full requested size then wrote a few hundred
         * bytes (ASAN: a 4880-byte / 244-vertex WRITE) past the 6 MiB transient
         * vertex buffer, corrupting the heap.  The corruption only faulted on the
         * NEXT allocator call (often the streamer's cJSON parse), which is why the
         * crash top was misleadingly cJSON_malloc.
         *
         * bgfx_alloc_transient_buffers (the same helper bgfx's own ImGui example
         * uses) takes the resource lock ONCE and only allocates if the FULL vertex
         * AND index counts both fit exactly; otherwise it returns false and
         * touches nothing.  So the memcpy below can never exceed the granted
         * buffers.  When it can't satisfy this cmd-list we stop (a partial copy
         * would index past the grant anyway) — a few dropped UI tris for one
         * frame under extreme load, never a heap overwrite. */
        bgfx_transient_vertex_buffer_t tvb;
        bgfx_transient_index_buffer_t  tib;
        if (!bgfx_alloc_transient_buffers(
                &tvb, &s_ctx.vertex_layout, num_vertices,
                &tib, num_indices, sizeof(ImDrawIdx) == 4))
            break;   /* transient pool exhausted this frame — stop cleanly */

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
            if (s_rec.active) {
                /* Display pass (keep bound state) + recording pass into the FBO. */
                bgfx_submit(s_ctx.view_id, s_ctx.program, 0, BGFX_DISCARD_NONE);
                bgfx_submit((uint16_t)(s_ctx.view_id + 1), s_ctx.program,
                            0, BGFX_DISCARD_ALL);
            } else {
                bgfx_submit(s_ctx.view_id, s_ctx.program, 0, BGFX_DISCARD_ALL);
            }
        }
    }

    /* Recording: read the offscreen UI FBO back into the video sink (BGRA8).
       The renderer keeps a small FIFO ring of readbacks in flight; if all
       slots are busy this frame is skipped (the encoder reorders by
       timestamp, so a dropped frame just lowers the capture rate).

       Capture-rate cap (default 30 fps, env JCE_REC_FPS 5..120): every
       captured frame costs a GPU blit + 30 MB readback + a main-thread
       LUT convert + a VP9 software encode — pacing to the target output
       rate skips all of that for frames the encoder would only spend
       bitrate on anyway (a 144 Hz editor does not need a 144 fps clip). */
    if (s_rec.active && BGFX_HANDLE_IS_VALID(s_rec.fb)) {
        static uint32_t s_interval_ms = 0;
        if (s_interval_ms == 0) {
            int fps = 30;
            const char *v = getenv("JCE_REC_FPS");
            if (v && v[0]) {
                fps = atoi(v);
                if (fps < 5)   fps = 5;
                if (fps > 120) fps = 120;
            }
            s_interval_ms = 1000u / (uint32_t)fps;
        }
        static uint64_t s_last_cap_ms = 0;
        const uint64_t now = (uint64_t)jce_time_ticks_ms();
        if (now - s_last_cap_ms >= s_interval_ms) {
            /* Plain FBO pass: rows come back per the backend's texture origin —
             * bottom-up only on GL. (Hardcoded bottom-up here is what turned
             * D3D12 recordings upside-down.) */
            if (jce_renderer_readback_capture_submit_sink(
                    s_rec.tex.idx, (uint16_t)(s_ctx.view_id + 2),
                    s_rec.w, s_rec.h,
                    bgfx_get_caps()->originBottomLeft ? 1 : 0))
                s_last_cap_ms = now;
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
