/*
 * jce_offscreen_target.c  Engine-owned bridge for editor scene viewport.
 */

#include <jce/renderer/jce_offscreen_target.h>

#include <jce/renderer/jce_views.h>
#include <jce/os/core/jce_log.h>

#include <bgfx/c99/bgfx.h>

#include "os/core/jce_memory.h"

#include <limits.h>

#define LOG_TAG "editor_bridge"

struct JceOffscreenTarget {
    JceRenderer *renderer;
    uint16_t view_id;

    bgfx_frame_buffer_handle_t target_fbo;
    bgfx_texture_handle_t target_color;
    uint32_t target_w;
    uint32_t target_h;
};

static void bridge_destroy_target(JceOffscreenTarget *bridge)
{
    if (!bridge)
        return;

    if (BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        bgfx_destroy_frame_buffer(bridge->target_fbo);

    bridge->target_fbo.idx = UINT16_MAX;
    bridge->target_color.idx = UINT16_MAX;
    bridge->target_w = 0;
    bridge->target_h = 0;
}

static bool bridge_ensure_target(JceOffscreenTarget *bridge,
                                 uint32_t width,
                                 uint32_t height)
{
    if (!bridge || width == 0 || height == 0)
        return false;

    if (bridge->target_w == width
        && bridge->target_h == height
        && BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return true;

    bridge_destroy_target(bridge);

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
                         0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
    bgfx_attachment_init(&attachments[1], textures[1], BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);

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
        bridge_destroy_target(bridge);
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
    return bridge;
}

void jce_offscreen_target_destroy(JceOffscreenTarget *bridge)
{
    if (!bridge)
        return;

    bridge_destroy_target(bridge);
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
    bgfx_set_view_rect(bridge->view_id, 0, 0, (uint16_t)width, (uint16_t)height);
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

uint16_t jce_offscreen_target_get_view_id(const JceOffscreenTarget *bridge)
{
    if (!bridge)
        return (uint16_t)JCE_VIEW_EDITOR_SCENE;
    return bridge->view_id;
}
