/*
 * jce_offscreen_target.c  Engine-owned bridge for editor scene viewport.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <limits.h>

#define LOG_TAG "editor_bridge"

/* Number of frames to keep a freshly-retired FBO alive before destroying
 * it. bgfx defers texture deletion by 1 frame, but during ImGui drag-
 * resize the previous frame's draw list (built BEFORE we recreated the
 * FBO this frame) may still reference the old color texture's idx via
 * ImGui::Image. If we let bgfx free + immediately reuse that idx for
 * the new texture, the driver's descriptor-set update sees a mismatch
 * between the bound texture and its real backing image, and on
 * Vulkan/D3D12 ICDs this surfaces as an access violation deep in the
 * driver. Holding the retired FBO for a few frames lets every consumer
 * (ImGui draw list, render thread, driver) finish with the old handle
 * before its slot is recycled. */
/* Raised 3->8 / 4->8: on Vulkan the deeper swapchain pipelining meant a
 * recycled texture idx could still be referenced by an in-flight ImGui draw
 * list, showing up as the scene viewport flashing ANOTHER render target's
 * content for a single frame ("object strobing"). Eight frames comfortably
 * exceeds any backend's in-flight depth. */
#define BRIDGE_RETIRE_DELAY 8
#define BRIDGE_RETIRE_SLOTS 8

typedef struct {
    bgfx_frame_buffer_handle_t fbo;
    int frames_left;
} RetiredTarget;

struct JceOffscreenTarget {
    JceRenderer *renderer;
    uint16_t view_id;

    bgfx_frame_buffer_handle_t target_fbo;
    bgfx_texture_handle_t target_color;
    uint32_t target_w;
    uint32_t target_h;
    bgfx_texture_format_t color_format; /* RGBA16F (HDR) or RGBA8 fallback */

    RetiredTarget retired[BRIDGE_RETIRE_SLOTS];
};

static void retire_destroy_due(JceOffscreenTarget *bridge)
{
    if (!bridge)
        return;
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        RetiredTarget *r = &bridge->retired[i];
        if (r->frames_left <= 0)
            continue;
        if (--r->frames_left == 0 && BGFX_HANDLE_IS_VALID(r->fbo)) {
            bgfx_destroy_frame_buffer(r->fbo);
            r->fbo.idx = UINT16_MAX;
        }
    }
}

static void retire_target(JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return;

    /* Find a free slot. If none free, fall back to immediate destroy
     * (best effort). */
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        RetiredTarget *r = &bridge->retired[i];
        if (r->frames_left == 0) {
            r->fbo = bridge->target_fbo;
            r->frames_left = BRIDGE_RETIRE_DELAY;
            bridge->target_fbo.idx = UINT16_MAX;
            bridge->target_color.idx = UINT16_MAX;
            bridge->target_w = 0;
            bridge->target_h = 0;
            return;
        }
    }

    /* Pool exhausted — destroy immediately. */
    bgfx_destroy_frame_buffer(bridge->target_fbo);
    bridge->target_fbo.idx = UINT16_MAX;
    bridge->target_color.idx = UINT16_MAX;
    bridge->target_w = 0;
    bridge->target_h = 0;
}

static void retire_destroy_all(JceOffscreenTarget *bridge)
{
    if (!bridge)
        return;
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        RetiredTarget *r = &bridge->retired[i];
        if (BGFX_HANDLE_IS_VALID(r->fbo))
            bgfx_destroy_frame_buffer(r->fbo);
        r->fbo.idx = UINT16_MAX;
        r->frames_left = 0;
    }
}

static bool bridge_ensure_target(JceOffscreenTarget *bridge,
                                 uint32_t width,
                                 uint32_t height)
{
    if (!bridge || width == 0 || height == 0)
        return false;

    /* Clamp to safe GPU-friendly range. */
    if (width  < 16u)   width  = 16u;
    if (height < 16u)   height = 16u;
    if (width  > 8192u) width  = 8192u;
    if (height > 8192u) height = 8192u;

    /* Tick the retire pool every prepare(). */
    retire_destroy_due(bridge);

    if (bridge->target_w == width
        && bridge->target_h == height
        && BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return true;

    /* Park the previous FBO for a few frames so any in-flight ImGui /
     * driver references finish before its texture slots are reused. */
    retire_target(bridge);

    /* HDR color target so smooth light-falloff gradients don't quantize into
       concentric "ring" bands on the 8-bit path. Fall back to RGBA8 where
       RGBA16F render targets are unsupported (ES2/WebGL1). The editor always
       tonemaps the HDR bridge back to LDR for display (jce_editor_scene_render
       force-enables the tonemap pass when the bridge is HDR). */
    bgfx_texture_format_t color_fmt = BGFX_TEXTURE_FORMAT_RGBA16F;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || (caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F]
                  & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) == 0) {
        color_fmt = BGFX_TEXTURE_FORMAT_RGBA8;
        LOG_WARN(LOG_TAG, "RGBA16F render target unsupported; using RGBA8 "
                          "(light-falloff banding may remain)");
    }

    /* NO MSAA, deliberately, and the MSAA SETTINGS DO NOT REACH HERE.
     *
     * Neither texture carries BGFX_TEXTURE_RT_MSAA_Xn, so this target is
     * single-sampled whatever Project Settings > Graphics > MSAA or an
     * .rp.json msaa_samples says. Both of those configure the BACKBUFFER; the
     * editor viewport and Game View render into THIS target instead, so for
     * everything inside them the sample count is 1 and changing the setting is
     * measurably inert -- 2x, 4x and 8x were all measured and returned an
     * identical flicker residual of 5.936, which reads as "MSAA does not help"
     * when the truth is that no MSAA was ever enabled.
     *
     * It is not an oversight to fix in passing. The colour and DEPTH textures
     * here are sampled as ordinary textures by SSAO, the volumetric fog march
     * and underwater absorption; a multisampled depth target cannot be read
     * that way without an explicit resolve, so enabling MSAA means adding one
     * and reworking every pass that reads depth. This pipeline's antialiasing
     * is TAA, which is why TAA is on by default at HIGH.
     *
     * If thin geometry shimmers under fast camera motion, that is TAA
     * rejecting history, and the answers are on the TAA/LOD side (motion
     * vectors, history clamping, impostors) -- not the MSAA slider. */
    bgfx_texture_handle_t textures[2];
    textures[0] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        color_fmt,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL, 0);

    /* CLAMP, not the bgfx default REPEAT.  The comment above already says this
     * depth texture is sampled as an ordinary texture by SSAO, the volumetric
     * fog march and underwater absorption -- and fs_ssao.sc's kernel
     * deliberately taps outside [0,1] near the frame border with no bounds
     * test (unlike fs_ssr.sc and the contact-shadow march, which both break
     * out of range).  Under REPEAT those taps wrap to the OPPOSITE edge's
     * depth and paint an AO rim around all four edges, on every backend. */
    textures[1] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL, 0);

    if (!BGFX_HANDLE_IS_VALID(textures[0])
        || !BGFX_HANDLE_IS_VALID(textures[1])) {
        if (BGFX_HANDLE_IS_VALID(textures[0]))
            bgfx_destroy_texture(textures[0]);
        if (BGFX_HANDLE_IS_VALID(textures[1]))
            bgfx_destroy_texture(textures[1]);
        LOG_WARN(LOG_TAG, "failed to allocate editor target textures %ux%u", width, height);
        return false;
    }

    bgfx_attachment_t attachments[2];
    attachments[0] = (bgfx_attachment_t){ 0 };
    attachments[1] = (bgfx_attachment_t){ 0 };

    bgfx_attachment_init(&attachments[0], textures[0], BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&attachments[1], textures[1], BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);

    bridge->target_fbo = bgfx_create_frame_buffer_from_attachment(2, attachments, true);
    if (!BGFX_HANDLE_IS_VALID(bridge->target_fbo)) {
        bgfx_destroy_texture(textures[0]);
        bgfx_destroy_texture(textures[1]);
        LOG_WARN(LOG_TAG, "failed to create editor target %ux%u", width, height);
        bridge->target_fbo.idx = UINT16_MAX;
        bridge->target_color.idx = UINT16_MAX;
        return false;
    }

    bridge->target_color = bgfx_get_texture(bridge->target_fbo, 0);
    if (!BGFX_HANDLE_IS_VALID(bridge->target_color)) {
        LOG_WARN(LOG_TAG, "failed to query editor color target %ux%u", width, height);
        bgfx_destroy_frame_buffer(bridge->target_fbo);
        bridge->target_fbo.idx = UINT16_MAX;
        bridge->target_color.idx = UINT16_MAX;
        return false;
    }

    bridge->target_w = width;
    bridge->target_h = height;
    bridge->color_format = color_fmt;
    return true;
}

JceOffscreenTarget *jce_offscreen_target_create(JceRenderer *renderer,
                                                       uint16_t view_id)
{
    if (!renderer)
        return NULL;

    JceOffscreenTarget *bridge = (JceOffscreenTarget *)JCE_CALLOC(1, sizeof(*bridge));
    if (!bridge)
        return NULL;

    bridge->renderer = renderer;
    bridge->view_id = (view_id == 0u) ? (uint16_t)JCE_VIEW_EDITOR_SCENE : view_id;
    bridge->target_fbo.idx = UINT16_MAX;
    bridge->target_color.idx = UINT16_MAX;
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        bridge->retired[i].fbo.idx = UINT16_MAX;
        bridge->retired[i].frames_left = 0;
    }
    return bridge;
}

void jce_offscreen_target_destroy(JceOffscreenTarget *bridge)
{
    if (!bridge)
        return;

    if (BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        bgfx_destroy_frame_buffer(bridge->target_fbo);
    bridge->target_fbo.idx = UINT16_MAX;
    bridge->target_color.idx = UINT16_MAX;
    retire_destroy_all(bridge);
    JCE_FREE(bridge);
}

bool jce_offscreen_target_prepare(JceOffscreenTarget *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      const char *view_name)
{
    if (!bridge || !view16 || !proj16)
        return false;

    if (!bridge_ensure_target(bridge, width, height))
        return false;

    const char *name = (view_name && view_name[0] != '\0')
        ? view_name
        : "EditorScene";

    bgfx_set_view_name(bridge->view_id, name, INT32_MAX);
    bgfx_set_view_rect(bridge->view_id, 0, 0,
                       (uint16_t)bridge->target_w,
                       (uint16_t)bridge->target_h);
    bgfx_set_view_clear(bridge->view_id,
                        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        clear_rgba, 1.0f, 0);
    bgfx_set_view_transform(bridge->view_id, view16, proj16);
    bgfx_set_view_frame_buffer(bridge->view_id, bridge->target_fbo);
    bgfx_set_view_mode(bridge->view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(bridge->view_id);
    return true;
}

bool jce_offscreen_target_prepare_overlay_view(
    JceOffscreenTarget *bridge, uint16_t view_id,
    const float *view16, const float *proj16, const char *view_name)
{
    if (!bridge || !view16 || !proj16 ||
        !BGFX_HANDLE_IS_VALID(bridge->target_fbo) ||
        bridge->target_w == 0 || bridge->target_h == 0)
        return false;

    bgfx_set_view_name(view_id,
        view_name && view_name[0] ? view_name : "OffscreenOverlay",
        INT32_MAX);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)bridge->target_w,
                       (uint16_t)bridge->target_h);
    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_transform(view_id, view16, proj16);
    bgfx_set_view_frame_buffer(view_id, bridge->target_fbo);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);
    return true;
}

uint16_t jce_offscreen_target_get_color_texture(
    const JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return UINT16_MAX;
    return bridge->target_color.idx;
}

uint16_t jce_offscreen_target_get_depth_texture(
    const JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return UINT16_MAX;
    bgfx_texture_handle_t depth = bgfx_get_texture(bridge->target_fbo, 1);
    return BGFX_HANDLE_IS_VALID(depth) ? depth.idx : (uint16_t)UINT16_MAX;
}

uint16_t jce_offscreen_target_get_view_id(const JceOffscreenTarget *bridge)
{
    if (!bridge)
        return (uint16_t)JCE_VIEW_EDITOR_SCENE;
    return bridge->view_id;
}

uint16_t jce_offscreen_target_get_frame_buffer(const JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return UINT16_MAX;
    return bridge->target_fbo.idx;
}

bool jce_offscreen_target_is_hdr(const JceOffscreenTarget *bridge)
{
    return bridge && bridge->color_format == BGFX_TEXTURE_FORMAT_RGBA16F;
}

void jce_offscreen_target_composite_texture(
    JceOffscreenTarget *bridge, uint16_t view_id, uint16_t texture_idx,
    uint16_t width, uint16_t height, bool flip_v)
{
    if (!bridge || !bridge->renderer || texture_idx == UINT16_MAX ||
        !BGFX_HANDLE_IS_VALID(bridge->target_fbo) ||
        width == 0 || height == 0) {
        return;
    }
    const bgfx_caps_t *caps = bgfx_get_caps();
    bgfx_set_view_frame_buffer(view_id, bridge->target_fbo);
    bgfx_set_view_rect(view_id, 0, 0, width, height);
    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    jce_mat4 view = jce_m4_identity();
    jce_mat4 proj = jce_m4_ortho(0.0f, (float)width, (float)height, 0.0f,
                                 0.0f, 100.0f, caps->homogeneousDepth);
    bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
    JceTexture tex; tex.idx = texture_idx;
    const float uv_flip[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    jce_draw_textured_rect_view_opaque(bridge->renderer, view_id,
                                       0.0f, 0.0f,
                                       (float)width, (float)height,
                                       tex, 0xFFFFFFFFu,
                                       flip_v ? uv_flip : NULL);
}
