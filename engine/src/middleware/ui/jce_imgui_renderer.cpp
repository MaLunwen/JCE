/*
 * jce_imgui_renderer.cpp  ImGui renderer backend implementation (bgfx).
 *
 * Renders ImGui draw lists using bgfx transient vertex/index buffers.
 * Each draw command is submitted on JCE_VIEW_IMGUI (view 250).
 */

#include <jce/ui/jce_imgui_renderer.h>

#include <bgfx/c99/bgfx.h>
#include <jce/tools/jce_imgui.hpp>
#include "jce_imgui_texture_upload_internal.h"
#include "jce_imgui_clip_internal.h"
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
    uint16_t               framebuffer_width, framebuffer_height;
} s_ctx;

/* Ortho projection now delegates to engine API (jce_m4_ortho). */

/* The target owns one texture reference separately from the FBO's attachment
 * reference. Passing false on create keeps rollback ownership unambiguous
 * even if bgfx exhausts FBO handles after validating the attachment. */
template<class Capture>
static void release_capture_target(Capture &capture, uint16_t view)
{
    if (BGFX_HANDLE_IS_VALID(capture.fb)) {
        bgfx_set_view_frame_buffer(view, { UINT16_MAX });
        bgfx_destroy_frame_buffer(capture.fb);
    }
    if (BGFX_HANDLE_IS_VALID(capture.tex))
        bgfx_destroy_texture(capture.tex);
    capture.fb.idx = UINT16_MAX;
    capture.tex.idx = UINT16_MAX;
    capture.w = capture.h = 0;
}

template<class Capture>
static bool prepare_capture_target(Capture &capture, uint16_t view,
                                   uint16_t width, uint16_t height)
{
    if (BGFX_HANDLE_IS_VALID(capture.fb) && capture.w == width && capture.h == height)
        return true;
    release_capture_target(capture, view);
    if (width == 0 || height == 0)
        return false;

    const bgfx_texture_handle_t texture = bgfx_create_texture_2d(width, height, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(texture))
        return false;

    const bgfx_frame_buffer_handle_t framebuffer =
        bgfx_create_frame_buffer_from_handles(1, &texture, false);
    if (!BGFX_HANDLE_IS_VALID(framebuffer)) {
        bgfx_destroy_texture(texture);
        return false;
    }
    capture.tex = texture;
    capture.fb = framebuffer;
    capture.w = width;
    capture.h = height;
    return true;
}

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
 * is encoded as idx+1 in ImTextureData::TexID, matching editor app textures.
 * A pending or invalid atlas is rejected before draw-command ID resolution. */

/* ImGui 1.92 stores a bgfx texture handle idx in ImTextureID, but
 * ImTextureID_Invalid == 0 collides with the perfectly valid bgfx idx 0
 * (first allocated handle).  Encode every idx as (idx+1) and decode (id-1) so
 * 0 stays reserved for "no texture" — otherwise a font atlas / app texture
 * landing on idx 0 would leak (never freed), corrupt bgfx handle 0 on update,
 * or trip ImGui's GetTexID assert (audit Round-3 P2-B).  Editor ImGui::Image()
 * producers apply the same +1 at their end. */
static const JceImGuiTextureCodes s_texture_codes = {
    ImTextureStatus_OK, ImTextureStatus_WantCreate, ImTextureStatus_WantUpdates,
    ImTextureStatus_WantDestroy, ImTextureStatus_Destroyed, ImTextureFormat_RGBA32
};
struct ImGuiTextureGpu {
    uint32_t max_dimension() const {
        const bgfx_caps_t *caps = bgfx_get_caps();
        return caps ? caps->limits.maxTextureSize : 0;
    }
    uint16_t create(uint16_t width, uint16_t height) const {
        return bgfx_create_texture_2d(width, height, false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT,
            NULL, 0).idx;
    }
    const bgfx_memory_t *copy(const void *pixels, uint32_t bytes) const { return bgfx_copy(pixels, bytes); }
    const bgfx_memory_t *alloc(uint32_t bytes) const { return bgfx_alloc(bytes); }
    void upload(uint16_t handle, uint16_t x, uint16_t y, uint16_t width,
                uint16_t height, const bgfx_memory_t *memory) const {
        bgfx_texture_handle_t texture = {handle};
        // Packed rows use bgfx's automatic pitch, without uint16 narrowing.
        bgfx_update_texture_2d(texture, 0, 0, x, y, width, height, memory, UINT16_MAX);
    }
    void destroy(uint16_t handle) const { bgfx_texture_handle_t texture = {handle}; bgfx_destroy_texture(texture); }
    void warning(const char *stage, const char *reason, int owner, int status,
                 int width, int height, uint64_t bytes) const {
        LOG_WARN(LOG_TAG, "atlas phase=%s owner=%d status=%d dims=%dx%d logical_request_bytes=%llu staging_accepted_bytes=0 reason=%s retry=%s",
            stage, owner, status, width, height, (unsigned long long)bytes, reason,
            strcmp(stage, "destroy") ? "pending" : "unowned_id_cleared");
    }
    void recovered(const char *stage, int owner) const {
        LOG_INFO(LOG_TAG, "atlas phase=%s owner=%d recovered=cpu_transaction_accepted", stage, owner);
    }
};
static ImGuiTextureGpu s_texture_gpu;
static void destroy_imgui_texture(ImTextureData *tex)
{ (void)jce_imgui_texture_destroy(tex, s_texture_gpu, s_texture_codes); }
static void update_imgui_texture(ImTextureData *tex)
{ (void)jce_imgui_texture_update(tex, s_texture_gpu, s_texture_codes); }

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_imgui_renderer_init(const JcePakArchive *pak, uint8_t view_id)
{
    s_ctx.view_id = view_id;
    bgfx_set_view_name(view_id, "UI/ImGui", INT32_MAX);

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

    /* Cancel only UI intent that has not reached the readback owner. Submitted
     * blits precede these bgfx destroy commands. Staging and pixels stay with
     * the readback owner until its existing shutdown completes. */
    s_cap.request = false;
    s_cap.readback_pending = false;
    s_cap.path[0] = '\0';
    s_rec.active = false;
    release_capture_target(s_cap, s_ctx.view_id);
    release_capture_target(s_rec, (uint16_t)(s_ctx.view_id + 1));

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
    s_ctx.framebuffer_width = width;
    s_ctx.framebuffer_height = height;

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
        if (prepare_capture_target(s_cap, s_ctx.view_id, width, height)) {
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
    const uint16_t rv = (uint16_t)(s_ctx.view_id + 1);
    if (s_rec.active) {
        if (!prepare_capture_target(s_rec, rv, width, height))
            return;
        bgfx_set_view_name(rv, "UI/Recording", INT32_MAX);
        bgfx_set_view_frame_buffer(rv, s_rec.fb);
        bgfx_set_view_clear(rv, BGFX_CLEAR_COLOR, 0x1e1e1effu, 1.0f, 0);
        bgfx_set_view_rect(rv, 0, 0, width, height);
        bgfx_set_view_mode(rv, BGFX_VIEW_MODE_SEQUENTIAL);
        bgfx_set_view_transform(rv, NULL, &ortho);
    } else if (BGFX_HANDLE_IS_VALID(s_rec.fb)) {
        /* Setup is the next UI frame boundary after Stop. Prior-frame draw
         * commands still refer to the old target until bgfx retires them. */
        release_capture_target(s_rec, rv);
    }
}

struct ImGuiDrawObservationState {
    JceImGuiDrawObservation value;
    uint16_t watched_texture;
    bool overflow;
};

static void imgui_observation_increment(ImGuiDrawObservationState *observation,
                                        uint32_t *count)
{
    if (*count == UINT32_MAX)
        observation->overflow = true;
    else
        ++*count;
}

static void imgui_observe_queue(const ImDrawData *draw_data,
                                ImGuiDrawObservationState *observation)
{
    observation->value.flags = JCE_IMGUI_DRAW_QUEUE_OBSERVED |
        JCE_IMGUI_DRAW_SUBMISSIONS_OBSERVED;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList *list = draw_data->CmdLists[n];
        for (int i = 0; i < list->CmdBuffer.Size; ++i) {
            const ImDrawCmd *command = &list->CmdBuffer[i];
            /* Pending font creation/destruction must never reach GetTexID's
             * assertion; reuse the exact draw admission decoder. */
            if (!command->UserCallback && command->ElemCount != 0 &&
                jce_imgui_texture_draw_handle(command->TexRef, s_texture_codes) ==
                    observation->watched_texture)
                imgui_observation_increment(observation,
                    &observation->value.queued_commands);
        }
    }
}

static void imgui_renderer_draw(ImGuiDrawObservationState *observation)
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

    if (observation)
        imgui_observe_queue(draw_data, observation);

    if (draw_data->TotalVtxCount == 0)
        return;

    uint32_t atlas_draw_rejected = 0;
    uint32_t clip_draw_rejected = 0;
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

            /* Reject nonfinite or framebuffer-empty clips before any draw
             * state/texture/submit. The private owner checks integer narrowing. */
            JceImGuiClipRect clip;
            if (!jce_imgui_clip_intersect(pcmd->ClipRect.x, pcmd->ClipRect.y,
                    pcmd->ClipRect.z, pcmd->ClipRect.w,
                    draw_data->DisplayPos.x, draw_data->DisplayPos.y,
                    s_ctx.framebuffer_width, s_ctx.framebuffer_height, &clip)) {
                ++clip_draw_rejected;
                continue;
            }
            bgfx_set_scissor(clip.x, clip.y, clip.width, clip.height);

            /* Bind texture. */
            bgfx_texture_handle_t tex;
            tex.idx = jce_imgui_texture_draw_handle(pcmd->TexRef, s_texture_codes);
            if (tex.idx == UINT16_MAX) { ++atlas_draw_rejected; continue; }
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
            const bool watched = observation && pcmd->ElemCount != 0 &&
                tex.idx == observation->watched_texture;
            if (s_rec.active && BGFX_HANDLE_IS_VALID(s_rec.fb)) {
                /* Display pass (keep bound state) + recording pass into the FBO. */
                bgfx_submit(s_ctx.view_id, s_ctx.program, 0, BGFX_DISCARD_NONE);
                if (watched)
                    imgui_observation_increment(observation, &observation->value.main_submits);
                bgfx_submit((uint16_t)(s_ctx.view_id + 1), s_ctx.program,
                            0, BGFX_DISCARD_ALL);
                if (watched)
                    imgui_observation_increment(observation, &observation->value.recording_submits);
            } else {
                bgfx_submit(s_ctx.view_id, s_ctx.program, 0, BGFX_DISCARD_ALL);
                if (watched)
                    imgui_observation_increment(observation, &observation->value.main_submits);
            }
        }
    }

    if (atlas_draw_rejected)
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "ui.atlas.draw.rejected", atlas_draw_rejected);
    if (clip_draw_rejected)
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "ui.clip.draw.rejected", clip_draw_rejected);

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

void jce_imgui_renderer_draw(void)
{
    imgui_renderer_draw(NULL);
}

bool jce_imgui_renderer_draw_observed(uint16_t watched_texture,
    JceImGuiDrawObservation *out, size_t out_size)
{
    if (!out || out_size < sizeof(*out) || watched_texture == UINT16_MAX) {
        imgui_renderer_draw(NULL);
        return false;
    }
    ImGuiDrawObservationState observation = {};
    observation.value.struct_size = (uint32_t)sizeof(observation.value);
    observation.watched_texture = watched_texture;
    imgui_renderer_draw(&observation);
    if (observation.overflow)
        return false;
    memcpy(out, &observation.value, sizeof(observation.value));
    return true;
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
