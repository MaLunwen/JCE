#include "jce_sr_internal.h"
#include "jce_water_field_internal.h"
#include "os/core/jce_memory.h"
#include <string.h>

bool sr_water_fft_upload(const JceWaterFft *fft, bgfx_texture_handle_t texture, int resolution)
{
    if (!fft || !BGFX_HANDLE_IS_VALID(texture) || resolution < 2 || resolution > 1024 ||
        resolution != jce_water_fft_resolution(fft)) return false;

    const float *height = jce_water_fft_height_data(fft);
    const float *dx = jce_water_fft_disp_x_data(fft);
    const float *dz = jce_water_fft_disp_z_data(fft);
    const float *foam = jce_water_fft_foam_data(fft);
    const uint32_t cells = (uint32_t)resolution * (uint32_t)resolution;
    const uint32_t bytes_per_texel = 4u * (uint32_t)sizeof(float);
    const bgfx_memory_t *memory = bgfx_alloc(cells * bytes_per_texel);
    if (!memory) return false;
    float *rgba = (float *)memory->data;
    for (uint32_t i = 0; i < cells; ++i) {
        rgba[i * 4 + 0] = height ? height[i] : 0.0f;
        rgba[i * 4 + 1] = dx ? dx[i] : 0.0f;
        rgba[i * 4 + 2] = dz ? dz[i] : 0.0f;
        rgba[i * 4 + 3] = foam ? foam[i] : 1.0f;
    }
    bgfx_update_texture_2d(texture, 0, 0, 0, 0, (uint16_t)resolution, (uint16_t)resolution,
                           memory, (uint16_t)((uint32_t)resolution * bytes_per_texel));
    return true;
}

SrWaterCausticSource sr_water_fine_caustic_source(JceSceneRenderer *sr, int slot,
    JceScene *scene, JceWaterField *field, float period_ratio, uint32_t frame)
{
    const SrWaterCausticSource invalid = {NULL, {UINT16_MAX}, 0};
    bgfx_texture_handle_t *texture = &sr->water_cache[slot].caustic_fft_tex;
    const bool has_texture = BGFX_HANDLE_IS_VALID(*texture);
    const bool in_flight = has_texture && sr->water_cache[slot].caustic_fft_revision != 0 &&
        sr->water_cache[slot].caustic_fft_frame == frame;
    if (period_ratio == 0.0f) {
        if (in_flight) return invalid;
        if (has_texture) bgfx_destroy_texture(*texture);
        texture->idx = UINT16_MAX;
        sr->water_cache[slot].caustic_fft_revision = 0;
        (void)jce_water_field_caustic_fft(field, 0.0f, NULL);
        return invalid;
    }
    if (in_flight && sr->water_cache[slot].caustic_fft_scene != scene) return invalid;

    uint64_t revision = 0;
    const JceWaterFft *fft = jce_water_field_caustic_fft(field, period_ratio, &revision);
    if (!fft) return invalid;
    const uint16_t resolution = (uint16_t)jce_water_fft_resolution(fft);
    if (in_flight) {
        if (revision != sr->water_cache[slot].caustic_fft_revision ||
            resolution != sr->water_cache[slot].caustic_fft_resolution) return invalid;
        return (SrWaterCausticSource){fft, *texture, revision};
    }
    if (!has_texture || resolution != sr->water_cache[slot].caustic_fft_resolution) {
        if (has_texture) bgfx_destroy_texture(*texture);
        *texture = bgfx_create_texture_2d(resolution, resolution, false, 1,
                                          BGFX_TEXTURE_FORMAT_RGBA32F, BGFX_TEXTURE_NONE, NULL, 0);
        sr->water_cache[slot].caustic_fft_resolution = resolution;
    }
    if (!sr_water_fft_upload(fft, *texture, resolution)) return invalid;
    sr->water_cache[slot].caustic_fft_scene = scene;
    sr->water_cache[slot].caustic_fft_revision = revision;
    sr->water_cache[slot].caustic_fft_frame = frame;
    return (SrWaterCausticSource){fft, *texture, revision};
}

/* Release the water mesh, retained source vertices and displacement textures
 * on eviction and renderer destruction. The simulation belongs to the scene. */
void sr_water_slot_free(JceSceneRenderer *sr, int slot)
{
    if (sr->water_cache[slot].mesh)
        jce_mesh_destroy(sr->water_cache[slot].mesh);
    JCE_FREE(sr->water_cache[slot].fft_source_vertices);
    jce_water_gradient_gpu_destroy(sr->water_cache[slot].gradient);
    jce_water_foam_gpu_destroy(sr->water_cache[slot].foam);
    sr_water_shore_gpu_destroy(sr->water_cache[slot].shore);
    /* No FFT to free here: the simulation belongs to the scene's water field,
     * and this cache holds only GPU resources.  It used to own a second,
     * private Tessendorf patch -- which is exactly the duplication that let the
     * drawn ocean and the simulated ocean drift apart. */
    if (BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft_tex))
        bgfx_destroy_texture(sr->water_cache[slot].fft_tex);
    if (BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft2_tex))
        bgfx_destroy_texture(sr->water_cache[slot].fft2_tex);
    if (BGFX_HANDLE_IS_VALID(sr->water_cache[slot].caustic_fft_tex))
        bgfx_destroy_texture(sr->water_cache[slot].caustic_fft_tex);
    memset(&sr->water_cache[slot], 0, sizeof sr->water_cache[slot]);
    /* memset leaves both texture handles at idx 0, which bgfx reads as a VALID
     * handle -- the same trap jce_scene_renderer_create already documents for
     * the initial slots.  Re-invalidate, or the next tenant of this slot both
     * destroys someone else's texture 0 and (for the cascade, which is now
     * created only when the field really has one) never creates its own. */
    sr->water_cache[slot].fft_tex.idx  = UINT16_MAX;
    sr->water_cache[slot].fft2_tex.idx = UINT16_MAX;
    sr->water_cache[slot].caustic_fft_tex.idx = UINT16_MAX;
}

void sr_water_destroy(JceSceneRenderer *sr)
{
    sr_water_optics_destroy(sr->water_optics);
    for (int i = 0; i < SR_WATER_CAUSTIC_VIEWPORTS; ++i)
        sr_water_caustic_atlas_destroy(sr->water_caustic_atlas[i]);
    /* Water cache (grid meshes) + program + uniforms. */
    for (int i = 0; i < SR_WATER_SLOT_MAX; i++)
        sr_water_slot_free(sr, i);   /* mesh, source vertices and water textures */
    if (BGFX_HANDLE_IS_VALID(sr->prog_water))           bgfx_destroy_program(sr->prog_water);
    if (BGFX_HANDLE_IS_VALID(sr->prog_water_physical))  bgfx_destroy_program(sr->prog_water_physical);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_wave_a))        bgfx_destroy_uniform(sr->u_water_wave_a);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_wave_b))        bgfx_destroy_uniform(sr->u_water_wave_b);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_params))        bgfx_destroy_uniform(sr->u_water_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_time))          bgfx_destroy_uniform(sr->u_water_time);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_color_shallow)) bgfx_destroy_uniform(sr->u_water_color_shallow);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_color_deep))    bgfx_destroy_uniform(sr->u_water_color_deep);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_shading))       bgfx_destroy_uniform(sr->u_water_shading);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_mode))          bgfx_destroy_uniform(sr->u_water_mode);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_disp))          bgfx_destroy_uniform(sr->s_water_disp);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_disp2))
        bgfx_destroy_uniform(sr->s_water_disp2);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_ripple))
        bgfx_destroy_uniform(sr->s_water_ripple);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_ripple))
        bgfx_destroy_uniform(sr->u_water_ripple);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_nearshore))
        bgfx_destroy_uniform(sr->u_water_nearshore);
    if (BGFX_HANDLE_IS_VALID(sr->water_ripple_tex))
        bgfx_destroy_texture(sr->water_ripple_tex);

    if (BGFX_HANDLE_IS_VALID(sr->s_water_data))
        bgfx_destroy_uniform(sr->s_water_data);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_planar))
        bgfx_destroy_uniform(sr->s_water_planar);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_planar))
        bgfx_destroy_uniform(sr->u_water_planar);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_planar_vp))
        bgfx_destroy_uniform(sr->u_water_planar_vp);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_depth))
        bgfx_destroy_uniform(sr->s_water_depth);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_absorb))
        bgfx_destroy_uniform(sr->u_water_absorb);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_absorb_tint))
        bgfx_destroy_uniform(sr->u_water_absorb_tint);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_depth_params))
        bgfx_destroy_uniform(sr->u_water_depth_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_caustics))
        bgfx_destroy_uniform(sr->u_water_caustics);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_caustic_secondary))
        bgfx_destroy_uniform(sr->u_water_caustic_secondary);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_caustic_detail))
        bgfx_destroy_uniform(sr->u_water_caustic_detail);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_caustic_fine))
        bgfx_destroy_uniform(sr->u_water_caustic_fine);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_caustic_fine_detail))
        bgfx_destroy_uniform(sr->u_water_caustic_fine_detail);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_caustic_disp))
        bgfx_destroy_uniform(sr->s_water_caustic_disp);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_shore))
        bgfx_destroy_uniform(sr->u_water_shore);

    if (BGFX_HANDLE_IS_VALID(sr->s_water_gradient))
        bgfx_destroy_uniform(sr->s_water_gradient);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_normal_detail))
        bgfx_destroy_uniform(sr->u_water_normal_detail);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_foam))
        bgfx_destroy_uniform(sr->u_water_foam);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_crest))
        bgfx_destroy_uniform(sr->u_water_crest);
}
