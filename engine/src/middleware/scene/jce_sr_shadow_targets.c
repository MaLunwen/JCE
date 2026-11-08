/* Shadow target ownership, separate from shadow draw submission. */
#include "jce_sr_internal.h"
#include "jce_sr_shadow_targets.h"

static uint16_t depth_texture_create(uint16_t size, uint16_t format, void *user)
{
    (void)user;
    bgfx_texture_handle_t texture = bgfx_create_texture_2d(size, size, false, 1,
        (bgfx_texture_format_t)format,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT, NULL, 0);
    return texture.idx;
}

static uint16_t depth_frame_buffer_create(uint16_t texture, void *user)
{
    (void)user;
    bgfx_texture_handle_t handle = {texture};
    bgfx_attachment_t attachment;
    memset(&attachment, 0, sizeof(attachment));
    bgfx_attachment_init(&attachment, handle, BGFX_ACCESS_WRITE, 0, 1, 0,
                         BGFX_RESOLVE_NONE);
    return bgfx_create_frame_buffer_from_attachment(1, &attachment, false).idx;
}

static void depth_texture_destroy(uint16_t texture, void *user)
{
    (void)user;
    bgfx_texture_handle_t handle = {texture};
    bgfx_destroy_texture(handle);
}

static void depth_frame_buffer_destroy(uint16_t frame_buffer, void *user)
{
    (void)user;
    bgfx_frame_buffer_handle_t handle = {frame_buffer};
    bgfx_destroy_frame_buffer(handle);
}

static const JceSrDepthTargetProvider depth_provider = {
    depth_texture_create, depth_frame_buffer_create,
    depth_texture_destroy, depth_frame_buffer_destroy, NULL
};

/* Commit a complete pair only. A failed framebuffer must not orphan the
 * texture, and a partial pair from an earlier failure must not be overwritten. */
bool sr_shadow_depth_target_ensure(uint16_t *texture, uint16_t *frame_buffer,
    uint16_t size, uint16_t format, const JceSrDepthTargetProvider *provider)
{
    if (*frame_buffer != UINT16_MAX && *texture != UINT16_MAX) return true;
    sr_shadow_depth_target_destroy(texture, frame_buffer, provider);
    if (size == 0) return false;

    uint16_t new_texture = provider->create_texture(size, format, provider->user);
    if (new_texture == UINT16_MAX) return false;
    uint16_t new_frame_buffer = provider->create_frame_buffer(new_texture,
                                                             provider->user);
    if (new_frame_buffer == UINT16_MAX) {
        provider->destroy_texture(new_texture, provider->user);
        return false;
    }
    *texture = new_texture;
    *frame_buffer = new_frame_buffer;
    return true;
}

void sr_shadow_depth_target_destroy(uint16_t *texture, uint16_t *frame_buffer,
    const JceSrDepthTargetProvider *provider)
{
    if (*frame_buffer != UINT16_MAX) {
        provider->destroy_frame_buffer(*frame_buffer, provider->user);
        *frame_buffer = UINT16_MAX;
    }
    if (*texture != UINT16_MAX) {
        provider->destroy_texture(*texture, provider->user);
        *texture = UINT16_MAX;
    }
}

void sr_destroy_shadow_targets(JceSceneRenderer *sr)
{
    if (!sr) return;
    sr_shadow_depth_target_destroy(&sr->shadow_tex.idx, &sr->shadow_fbo.idx,
                                   &depth_provider);
    sr_shadow_depth_target_destroy(&sr->local_atlas_tex.idx,
                                   &sr->local_atlas_fbo.idx, &depth_provider);
    sr->local_atlas_valid = false;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; ++i) {
        sr_shadow_depth_target_destroy(&sr->csm_tex[i].idx, &sr->csm_fbo[i].idx,
                                       &depth_provider);
        sr->shadow_cache_valid[i] = false;
    }
    sr_shadow_depth_target_destroy(&sr->dyn_csm_atlas_tex.idx,
                                   &sr->dyn_csm_atlas_fbo.idx, &depth_provider);
    sr->dyn_csm_atlas_size = 0;
    sr->dyn_csm_valid = false;
    sr->shadow_valid = false;
    sr->csm_valid = false;
    sr->last_csm_valid = false;
}

static bool sr_make_depth_target(JceSceneRenderer *sr,
                                 bgfx_texture_handle_t *texture,
                                 bgfx_frame_buffer_handle_t *frame_buffer)
{
    if (!sr) return false;
    return sr_shadow_depth_target_ensure(&texture->idx, &frame_buffer->idx,
        sr->shadow_map_size, (uint16_t)sr->shadow_depth_fmt, &depth_provider);
}

/* Frame orchestration resolves cfg, live RP and scene precedence first. The
 * global RP count alone cannot describe an explicit per-view override.
 * Keep cascade zero available for the inactive local-shadow sampler fallback. */
static uint32_t sr_csm_wanted_count(const JceSceneRenderer *sr)
{
    uint32_t count = sr->csm_cascade_count;
    if (count < 1u) count = 1u;
    if (count > JCE_CSM_MAX_CASCADES) count = JCE_CSM_MAX_CASCADES;
    return count;
}

bool sr_ensure_csm_count(JceSceneRenderer *sr)
{
    if (!sr || sr->shadow_map_size == 0) return false;
    const uint32_t count = sr_csm_wanted_count(sr);
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i) {
        const bool complete = BGFX_HANDLE_IS_VALID(sr->csm_tex[i])
                           && BGFX_HANDLE_IS_VALID(sr->csm_fbo[i]);
        if (!sr_make_depth_target(sr, &sr->csm_tex[i], &sr->csm_fbo[i]))
            ok = false;
        else if (!complete)
            sr->shadow_cache_valid[i] = false;
    }
    sr->csm_valid = ok;
    return ok;
}

SrShadowTargetAdmission sr_shadow_dynamic_target_admit(bool dual_requested,
    bool percascade_active, uint32_t dynamic_count,
    uint16_t *texture, uint16_t *frame_buffer, uint16_t size, uint16_t format,
    const JceSrDepthTargetProvider *provider)
{
    if (!dual_requested || !percascade_active || dynamic_count == 0)
        return SR_SHADOW_TARGET_NOT_NEEDED;
    return sr_shadow_depth_target_ensure(texture, frame_buffer, size, format,
                                         provider)
        ? SR_SHADOW_TARGET_READY : SR_SHADOW_TARGET_FAILED;
}

bool sr_ensure_dyn_csm_atlas(JceSceneRenderer *sr, bool dual_requested,
                            bool percascade_active)
{
    if (!sr) return false;
    const SrShadowTargetAdmission result = sr_shadow_dynamic_target_admit(
        dual_requested, percascade_active, sr->shadow_dyn_count,
        &sr->dyn_csm_atlas_tex.idx, &sr->dyn_csm_atlas_fbo.idx,
        sr->shadow_map_size, (uint16_t)sr->shadow_depth_fmt, &depth_provider);
    if (result == SR_SHADOW_TARGET_NOT_NEEDED) return false;
    sr->dyn_csm_valid = result == SR_SHADOW_TARGET_READY;
    sr->dyn_csm_atlas_size = sr->dyn_csm_valid ? sr->shadow_map_size : 0;
    return sr->dyn_csm_valid;
}

bool sr_ensure_shadow_map(JceSceneRenderer *sr)
{
    if (!sr) return false;
    /* shadow_valid gates the entire pass, not this lazily allocated map. */
    return sr_make_depth_target(sr, &sr->shadow_tex, &sr->shadow_fbo);
}

bool sr_ensure_local_atlas(JceSceneRenderer *sr)
{
    if (!sr) return false;
    sr->local_atlas_valid = sr_make_depth_target(sr, &sr->local_atlas_tex,
                                                 &sr->local_atlas_fbo);
    return sr->local_atlas_valid;
}

void sr_ensure_shadow_map_size(JceSceneRenderer *sr, uint16_t size)
{
    if (!sr || size == 0) return;
    if (size < 512) size = 512;
    if (size > 4096) size = 4096;
    /* Resolution selection is not a request to draw. Existing same-size
     * pairs stay resident; a real directional/local pass repairs or grows
     * them on demand after its feature/light requirements are resolved. */
    if (sr->shadow_map_size == size) return;

    sr_destroy_shadow_targets(sr);
    sr->shadow_map_size = size;
    sr->shadow_valid = BGFX_HANDLE_IS_VALID(sr->u_shadowMap);
    sr->shadow_far_valid = false;
    LOG_INFO(LOG_TAG, "shadow map resized to %u", (unsigned)size);
}
