#include "jce_water_gradient_gpu.h"
#include "jce_water_gradient.h"
#include "os/core/jce_memory.h"
#include "os/core/jce_half_internal.h"

struct JceWaterGradientGpu {
    bgfx_texture_handle_t texture;
    float *scratch;
    int resolution;
    const JceWaterField *field;
    uint64_t revision;
    bool uploaded;
};

void jce_water_gradient_gpu_destroy(JceWaterGradientGpu *cache)
{
    if (!cache) return;
    if (BGFX_HANDLE_IS_VALID(cache->texture)) bgfx_destroy_texture(cache->texture);
    JCE_FREE(cache->scratch);
    JCE_FREE(cache);
}

bgfx_texture_handle_t jce_water_gradient_gpu_texture(const JceWaterGradientGpu *cache)
{
    bgfx_texture_handle_t result = { UINT16_MAX };
    if (cache && cache->uploaded) result = cache->texture;
    return result;
}

bool jce_water_gradient_gpu_prepare(JceWaterGradientGpu **owner,
                                    const JceWaterField *field)
{
    if (!owner || !field) return false;
    const JceWaterFft *fft = jce_water_field_fft(field);
    if (!fft) return false;
    const int n = jce_water_fft_resolution(fft);
    const JceWaterFft *second = jce_water_field_fft2(field);
    if (n < 2 || n > 1024 || (n & (n - 1)) ||
        (second && jce_water_fft_resolution(second) != n)) return false;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_2D) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, BGFX_TEXTURE_NONE))
        return false;
    const size_t count = (size_t)n * (size_t)n * 4u;
    JceWaterGradientGpu *cache = *owner;
    const bool replace = !cache || cache->resolution != n;
    if (replace) {
        cache = (JceWaterGradientGpu *)JCE_CALLOC(1, sizeof(*cache));
        if (!cache) return false;
        cache->texture.idx = UINT16_MAX;
        cache->resolution = n;
        cache->scratch = (float *)JCE_MALLOC(count * sizeof(float));
        if (!cache->scratch) { jce_water_gradient_gpu_destroy(cache); return false; }
        cache->texture = bgfx_create_texture_2d((uint16_t)n, (uint16_t)n, true,
            1, BGFX_TEXTURE_FORMAT_RGBA16F, BGFX_TEXTURE_NONE, NULL, 0);
        if (!BGFX_HANDLE_IS_VALID(cache->texture)) {
            jce_water_gradient_gpu_destroy(cache); return false;
        }
    }
    const uint64_t revision = jce_water_field_revision(field);
    if (!replace && cache->uploaded && cache->field == field && cache->revision == revision)
        return true;
    if (!jce_water_gradient_pack(cache->scratch, count, n,
        jce_water_fft_slope_x_data(fft), jce_water_fft_slope_z_data(fft),
        second ? jce_water_fft_slope_x_data(second) : NULL,
        second ? jce_water_fft_slope_z_data(second) : NULL)) {
        if (replace) jce_water_gradient_gpu_destroy(cache);
        return false;
    }
    uint8_t level = 0;
    for (int side = n; side > 0; side /= 2, ++level) {
        const uint32_t words = (uint32_t)side * (uint32_t)side * 4u;
        const bgfx_memory_t *memory = bgfx_alloc(words * (uint32_t)sizeof(uint16_t));
        if (!memory) {
            cache->uploaded = false;
            if (replace) jce_water_gradient_gpu_destroy(cache);
            return false;
        }
        uint16_t *pixels = (uint16_t *)memory->data;
        for (uint32_t i = 0; i < words; ++i) pixels[i] = jce_float_to_half(cache->scratch[i]);
        bgfx_update_texture_2d(cache->texture, 0, level, 0, 0,
            (uint16_t)side, (uint16_t)side, memory, (uint16_t)(side * 8));
        if (side > 1) (void)jce_water_gradient_reduce(cache->scratch, count, side);
    }
    cache->field = field;
    cache->revision = revision;
    cache->uploaded = true;
    if (replace) { jce_water_gradient_gpu_destroy(*owner); *owner = cache; }
    return true;
}
