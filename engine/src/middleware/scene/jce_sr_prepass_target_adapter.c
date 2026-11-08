/* Backend adaptation and borrowed current-render aliases for the prepass owner. */
#include "jce_sr_internal.h"

static uint16_t texture_create(uint16_t width, uint16_t height,
                                uint16_t format, void *user)
{
    (void)user;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || width > caps->limits.maxTextureSize ||
        height > caps->limits.maxTextureSize)
        return UINT16_MAX;
    return bgfx_create_texture_2d(width, height, false, 1,
        (bgfx_texture_format_t)format, BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT, NULL, 0).idx;
}

static uint16_t frame_buffer_create(const uint16_t *textures,
                                     uint8_t count, void *user)
{
    (void)user;
    bgfx_attachment_t attachments[4];
    memset(attachments, 0, sizeof(attachments));
    for (uint8_t i = 0; i < count; ++i) {
        bgfx_texture_handle_t texture = {textures[i]};
        bgfx_attachment_init(&attachments[i], texture, BGFX_ACCESS_WRITE,
                              0, 1, 0, BGFX_RESOLVE_NONE);
    }
    /* Validate the new layout before submitting its creation. A provider may
     * still refuse creation; the transactional owner keeps the legacy fallback. */
    if (count == 1 && !bgfx_is_frame_buffer_valid(count, attachments))
        return UINT16_MAX;
    return bgfx_create_frame_buffer_from_attachment(count, attachments, false).idx;
}

static void texture_destroy(uint16_t texture, void *user)
{
    (void)user;
    bgfx_texture_handle_t handle = {texture};
    bgfx_destroy_texture(handle);
}

static void frame_buffer_destroy(uint16_t frame_buffer, void *user)
{
    (void)user;
    bgfx_frame_buffer_handle_t handle = {frame_buffer};
    bgfx_destroy_frame_buffer(handle);
}

static const SrPrepassTargetProvider provider = {
    texture_create, frame_buffer_create, texture_destroy, frame_buffer_destroy, NULL
};

static void aliases_clear(JceSceneRenderer *sr)
{
    sr->ssao_depth_tex.idx = sr->ssao_normal_tex.idx = UINT16_MAX;
    sr->ssao_albedo_tex.idx = sr->ssao_velocity_tex.idx = UINT16_MAX;
    sr->ssao_depth_fbo.idx = UINT16_MAX;
    sr->ssao_w = sr->ssao_h = 0;
    sr->ssao_valid = sr->ssao_has_velocity = false;
    sr->ssao_target_generation = 0;
    sr->depth_prepass_frame = sr->velocity_valid_frame = false;
    sr->depth_history.valid = sr->depth_history_reused = false;
}

void sr_init_prepass_targets(JceSceneRenderer *sr)
{
    sr_prepass_targets_init(&sr->prepass_targets);
    aliases_clear(sr);
    sr->prepass_target_reported_reasons = 0;
}

void sr_ensure_ssao_target(JceSceneRenderer *sr, uint16_t view_base,
                            uint32_t width, uint32_t height, bool velocity,
                            bool normals_demand)
{
    if (!sr) return;
    SrPrepassTargetRequest request = {view_base, width, height,
        (uint16_t)sr->shadow_depth_fmt, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_FORMAT_RGBA16F, velocity, !normals_demand && !velocity};
    const SrPrepassTarget *target = sr_prepass_targets_acquire(&sr->prepass_targets,
        &request, &provider, sr);
    if (!target) {
        aliases_clear(sr);
        uint32_t reason = !width || !height || width > UINT16_MAX ||
            height > UINT16_MAX ? 1u : 2u;
        const bgfx_caps_t *caps = bgfx_get_caps();
        if (reason != 1u && caps && (width > caps->limits.maxTextureSize ||
                                     height > caps->limits.maxTextureSize))
            reason = 4u;
        if (!(sr->prepass_target_reported_reasons & reason)) {
            sr->prepass_target_reported_reasons |= reason;
            LOG_WARN(LOG_TAG,
                "prepass target unavailable: view=%u size=%ux%u reason=%s; render rejected",
                (unsigned)view_base, (unsigned)width, (unsigned)height,
                reason == 1u ? "extent" : reason == 4u ? "hardware-limit" : "allocation");
        }
        return;
    }
    if (sr->ssao_target_generation != target->generation)
        sr->depth_history.valid = false;
    sr->ssao_depth_tex.idx = target->depth;
    sr->ssao_normal_tex.idx = target->normal;
    sr->ssao_albedo_tex.idx = target->albedo;
    sr->ssao_velocity_tex.idx = target->velocity;
    sr->ssao_depth_fbo.idx = target->frame_buffer;
    sr->ssao_w = (uint16_t)width;
    sr->ssao_h = (uint16_t)height;
    sr->ssao_has_velocity = velocity;
    sr->ssao_valid = true;
    sr->ssao_target_generation = target->generation;
}

void sr_release_prepass_targets(JceSceneRenderer *sr,
                                 SrPrepassTargetReleaseReason reason)
{
    if (!sr) return;
    sr_prepass_targets_release(&sr->prepass_targets, &provider, sr, reason);
    aliases_clear(sr);
    sr->prepass_target_reported_reasons = 0;
    memset(sr->pre_cache, 0, sizeof(sr->pre_cache));
}
