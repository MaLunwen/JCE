#include "jce_sr_internal.h"
#include "jce_sr_rain_surface.h"
#include "jce_water_field_internal.h"
#include "jce_water_shore_state.h"
#include "jce_water_shore_atlas.h"
#include "os/core/jce_half_internal.h"
#include "os/core/jce_memory.h"
#include "renderer/jce_renderer_caps_internal.h"
#include <jce/middleware/scene/jce_scene_water_nearshore.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_water_ripple.h>
#include <math.h>
#include <string.h>

/* Stage 13 packs two independent square grids, each with a one-texel
 * replicated border. Shore R/G/B stores surface offset, depth and foam.
 * Shore velocity RG is a second padded tile below the first, from the SAME
 * snapshot/revision. Wake R keeps its original independent grid order. */
struct SrWaterShoreGpu {
    bgfx_texture_handle_t texture;
    uint16_t width, height;
    uint32_t frame;
    uintptr_t scene_id;
    uint64_t identity, revision;
    int shore_side, wake_side;
};

static const uint64_t disturbance_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

void sr_water_shore_gpu_destroy(SrWaterShoreGpu *cache)
{
    if (!cache) return;
    if (BGFX_HANDLE_IS_VALID(cache->texture)) bgfx_destroy_texture(cache->texture);
    JCE_FREE(cache);
}


static bool prepare_atlas(SrWaterShoreGpu **owner, const JceWaterShoreSnapshot *shore,
                          const float *wake, int wake_side, uintptr_t scene_id, uint32_t frame)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    const int side = shore->side;
    if (side < 4 || side > 128 || !shore->cells || !shore->bed || !shore->foam ||
        wake_side < 0 || wake_side > 1024 || (wake_side > 0 && !wake)) return false;
    const int width = side + 2 + (wake_side > 0 ? wake_side + 2 : 0);
    const int shore_height = 2 * (side + 2);
    const int height = wake_side + 2 > shore_height ? wake_side + 2 : shore_height;
    if (!caps || (uint32_t)width > caps->limits.maxTextureSize ||
        (uint32_t)height > caps->limits.maxTextureSize ||
        !(caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_2D) ||
        !jce_gpu_vertex_fetch_usable(JCE_GPU_VFETCH_FMT_RGBA16F) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, disturbance_flags))
        return false;

    SrWaterShoreGpu *cache = *owner;
    const bool same_frame = cache && cache->frame == frame;
    if (same_frame && cache->scene_id == scene_id && cache->identity == shore->identity &&
        cache->revision == shore->revision && cache->shore_side == side &&
        cache->wake_side == wake_side) return true;
    /* Earlier views in this frame must retain the texture they submitted. */
    const bool replace = !cache || same_frame || cache->width != width || cache->height != height;
    if (replace) {
        cache = (SrWaterShoreGpu *)JCE_CALLOC(1, sizeof(*cache));
        if (!cache) return false;
        cache->width = (uint16_t)width;
        cache->height = (uint16_t)height;
        cache->texture = bgfx_create_texture_2d(cache->width, cache->height, false,
            1, BGFX_TEXTURE_FORMAT_RGBA16F, disturbance_flags, NULL, 0);
        if (!BGFX_HANDLE_IS_VALID(cache->texture)) {
            sr_water_shore_gpu_destroy(cache);
            return false;
        }
    }
    const bgfx_memory_t *memory = bgfx_alloc((uint32_t)width * (uint32_t)height * 8u);
    if (!memory) {
        if (replace) sr_water_shore_gpu_destroy(cache);
        return false;
    }
    jce_water_shore_pack_atlas((uint16_t *)memory->data, width, height, shore, wake, wake_side);
    bgfx_update_texture_2d(cache->texture, 0, 0, 0, 0, cache->width, cache->height,
                           memory, (uint16_t)(width * 8));
    cache->scene_id = scene_id;
    cache->identity = shore->identity;
    cache->revision = shore->revision;
    cache->frame = frame;
    cache->shore_side = side;
    cache->wake_side = wake_side;
    if (replace) {
        sr_water_shore_gpu_destroy(*owner);
        *owner = cache;
    }
    return true;
}

bool sr_water_shore_gpu_rain_surface(JceSceneRenderer *sr, JceScene *scene,
    jce_vec3 origin, jce_vec3 spawn_box, JceParticleRainSurface *out)
{
    if (!isfinite(origin.x) || !isfinite(origin.z) ||
        !isfinite(spawn_box.x) || !isfinite(spawn_box.z)) return false;
    const uint32_t frame = jce_renderer_get_frame_index(sr->renderer);
    const float half_x = fmaxf(0.05f, fabsf(spawn_box.x) * 0.5f);
    const float half_z = fmaxf(0.05f, fabsf(spawn_box.z) * 0.5f);
    JceWaterFieldSet *fields = NULL;
    const int water_type = jce_component_find("Water");
    float best_area = 0.0f;
    for (size_t i = 0; i < sizeof sr->water_cache / sizeof sr->water_cache[0]; ++i) {
        const SrWaterShoreGpu *cache = sr->water_cache[i].shore;
        if (!sr->water_cache[i].used || !cache || cache->frame != frame ||
            cache->scene_id != (uintptr_t)scene || !BGFX_HANDLE_IS_VALID(cache->texture))
            continue;
        const JceEntity water = sr->water_cache[i].entity;
        const JceWaterComponent *wc = jce_scene_get_water(scene, water);
        if (!wc || !wc->visible ||
            (water_type >= 0 && !jce_scene_comp_enabled(scene, water, water_type))) continue;
        if (!fields) fields = jce_scene_water_fields(scene);
        JceWaterShoreSnapshot shore;
        if (!jce_water_shore_state_snapshot(jce_water_field_set_shore(fields, water), &shore) ||
            cache->identity != shore.identity || cache->revision != shore.revision ||
            cache->shore_side != shore.side) continue;
        const float margin = fmaxf(1.0f, (float)shore.side * 0.15f) * shore.cell_size;
        const float extent = (float)shore.side * shore.cell_size - margin;
        const float x = fminf(shore.min_x + extent, origin.x + half_x) -
                        fmaxf(shore.min_x + margin, origin.x - half_x);
        const float z = fminf(shore.min_z + extent, origin.z + half_z) -
                        fmaxf(shore.min_z + margin, origin.z - half_z);
        if (!(x > 0.0f && z > 0.0f && x * z > best_area)) continue;
        JceWaterFieldDesc field;
        if (!jce_scene_water_field_desc(scene, water, &field) || !isfinite(field.base_height))
            continue;
        best_area = x * z;
        *out = (JceParticleRainSurface){
            {cache->texture.idx},
            {{shore.min_x, shore.min_z, 1.0f / shore.cell_size, (float)shore.side},
             {1.0f / cache->width, 1.0f / cache->height, field.base_height, 1.0f}},
        };
    }
    return best_area > 0.0f;
}

/* The old R32F wake path remains shared by every body without a ready atlas. */
static bool prepare_wake(JceSceneRenderer *sr, const float *heights, int side, uint32_t frame)
{
    if (!heights || side <= 0 || !jce_gpu_vertex_fetch_usable(JCE_GPU_VFETCH_FMT_R32F))
        return false;
    if (BGFX_HANDLE_IS_VALID(sr->water_ripple_tex) && sr->water_ripple_res != side) {
        bgfx_destroy_texture(sr->water_ripple_tex);
        sr->water_ripple_tex.idx = UINT16_MAX;
    }
    if (!BGFX_HANDLE_IS_VALID(sr->water_ripple_tex)) {
        sr->water_ripple_tex = bgfx_create_texture_2d((uint16_t)side, (uint16_t)side, false,
            1, BGFX_TEXTURE_FORMAT_R32F, disturbance_flags, NULL, 0);
        sr->water_ripple_res = side;
        sr->water_ripple_frame_valid = false;
    }
    if (!BGFX_HANDLE_IS_VALID(sr->water_ripple_tex)) return false;
    if (!sr->water_ripple_frame_valid || sr->water_ripple_frame != frame) {
        const uint32_t bytes = (uint32_t)side * (uint32_t)side * sizeof(float);
        const bgfx_memory_t *memory = bgfx_copy(heights, bytes);
        if (!memory) return false;
        bgfx_update_texture_2d(sr->water_ripple_tex, 0, 0, 0, 0,
            (uint16_t)side, (uint16_t)side, memory, (uint16_t)(side * sizeof(float)));
        sr->water_ripple_frame = frame;
        sr->water_ripple_frame_valid = true;
    }
    return true;
}

bool sr_bind_water_disturbance(JceSceneRenderer *sr, JceScene *scene,
                              JceEntity water, int slot, bool allow_shore)
{
    if (!BGFX_HANDLE_IS_VALID(sr->s_water_ripple))
        sr->s_water_ripple = bgfx_create_uniform("s_water_ripple", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_ripple))
        sr->u_water_ripple = bgfx_create_uniform("u_water_ripple", BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_nearshore))
        sr->u_water_nearshore = bgfx_create_uniform("u_water_nearshore", BGFX_UNIFORM_TYPE_VEC4, 2);

    const JceWaterRipple *ripple = jce_scene_water_ripple(scene, NULL);
    const float *wake = ripple ? jce_water_ripple_height_data(ripple) : NULL;
    const int wake_side = wake ? jce_water_ripple_resolution(ripple) : 0;
    const uint32_t frame = jce_renderer_get_frame_index(sr->renderer);
    const JceWaterShoreState *state = allow_shore ?
        jce_water_field_set_shore(jce_scene_water_fields(scene), (uint64_t)water) : NULL;
    const JceWaterNearshoreComponent *settings = state ?
        jce_scene_get_water_nearshore(scene, water) : NULL;
    SrWaterShoreGpu **owner = &sr->water_cache[slot].shore;
    if (!settings && *owner) {
        sr_water_shore_gpu_destroy(*owner);
        *owner = NULL;
    }

    JceWaterShoreSnapshot shore = {0};
    const bool atlas_ready = settings && BGFX_HANDLE_IS_VALID(sr->u_water_nearshore) &&
        BGFX_HANDLE_IS_VALID(sr->s_water_ripple) &&
        (!wake_side || BGFX_HANDLE_IS_VALID(sr->u_water_ripple)) &&
        jce_water_shore_state_snapshot(state, &shore) &&
        prepare_atlas(owner, &shore, wake, wake_side, (uintptr_t)scene, frame);
    float params[8] = {0};
    float rp[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    bgfx_texture_handle_t texture = sr->white_tex;
    bool wake_ready = false;
    if (atlas_ready) {
        const SrWaterShoreGpu *cache = *owner;
        params[0] = shore.min_x;
        params[1] = shore.min_z;
        params[2] = 1.0f / shore.cell_size;
        params[3] = (float)shore.side;
        params[4] = 1.0f / cache->width;
        params[5] = 1.0f / cache->height;
        params[6] = (float)wake_side;
        params[7] = settings->foam_opacity;
        texture = cache->texture;
        wake_ready = wake_side > 0;
    } else if (prepare_wake(sr, wake, wake_side, frame)) {
        texture = sr->water_ripple_tex;
        wake_ready = true;
    }
    if (wake_ready) {
        jce_water_ripple_world_rect(ripple, &rp[0], &rp[1], &rp[2]);
        rp[3] = 1.0f;
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_water_nearshore))
        bgfx_set_uniform(sr->u_water_nearshore, params, 2);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_ripple))
        bgfx_set_uniform(sr->u_water_ripple, rp, 1);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_ripple))
        bgfx_set_texture(13, sr->s_water_ripple, texture, UINT32_MAX);
    return atlas_ready;
}
