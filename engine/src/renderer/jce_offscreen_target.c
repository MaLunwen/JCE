/*
 * jce_offscreen_target.c  Engine-owned bridge for editor scene viewport.
 */

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_offscreen_target.h>
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
#define BRIDGE_RETIRE_DELAY 3
#define BRIDGE_RETIRE_SLOTS 4

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

    bgfx_texture_handle_t textures[2];
    textures[0] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL);

    textures[1] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8,
        BGFX_TEXTURE_RT,
        NULL);

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
