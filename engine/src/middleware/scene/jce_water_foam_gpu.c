#include "jce_water_foam_gpu.h"
#include "jce_water_gradient.h" /* shared four-channel box reduction */
#include "os/core/jce_memory.h"
#include "os/core/jce_half_internal.h"
struct JceWaterFoamGpu {
    bgfx_texture_handle_t texture;
    float *scratch;
    int resolution;
    uint64_t identity, revision;
    bool uploaded;
};
void jce_water_foam_gpu_destroy(JceWaterFoamGpu *cache)
{
    if (!cache) return;
    if (BGFX_HANDLE_IS_VALID(cache->texture)) bgfx_destroy_texture(cache->texture);
    JCE_FREE(cache->scratch);
    JCE_FREE(cache);
}
bgfx_texture_handle_t jce_water_foam_gpu_texture(const JceWaterFoamGpu *cache)
{
    bgfx_texture_handle_t result = { UINT16_MAX };
    if (cache && cache->uploaded) result = cache->texture;
    return result;
}
static float foam_detail(uint32_t i)
{
    i ^= 0x9e3779b9u;
    i ^= i >> 16; i *= 0x7feb352du;
    i ^= i >> 15; i *= 0x846ca68bu;
    i ^= i >> 16;
    return (float)(i & 0x00ffffffu) / 16777215.0f;
}
bool jce_water_foam_gpu_prepare(JceWaterFoamGpu **owner, const JceWaterFoamState *state)
{
    if (!owner || !state || !state->density[0]) return false;
    const int n = state->resolution;
    if (n < 2 || n > 1024 || (n & (n-1))) return false;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_2D) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, BGFX_TEXTURE_NONE)) return false;
    const size_t cells = (size_t)n*(size_t)n, count = cells*4u;
    for (size_t i = 0; i < cells; ++i)
        if (!isfinite(state->density[0][i]) || state->density[0][i] < 0.0f || state->density[0][i] > 1.0f ||
            (state->density[1] && (!isfinite(state->density[1][i]) ||
            state->density[1][i] < 0.0f || state->density[1][i] > 1.0f))) return false;
    JceWaterFoamGpu *cache = *owner;
    const bool replace = !cache || cache->resolution != n;
    if (replace) {
        cache = (JceWaterFoamGpu *)JCE_CALLOC(1, sizeof(*cache));
        if (!cache) return false;
        cache->texture.idx = UINT16_MAX;
        cache->resolution = n;
        cache->scratch = (float *)JCE_MALLOC(count*sizeof(float));
        if (!cache->scratch) { jce_water_foam_gpu_destroy(cache); return false; }
        cache->texture = bgfx_create_texture_2d((uint16_t)n, (uint16_t)n, true,
            1, BGFX_TEXTURE_FORMAT_RGBA16F, BGFX_TEXTURE_NONE, NULL, 0);
        if (!BGFX_HANDLE_IS_VALID(cache->texture)) {
            jce_water_foam_gpu_destroy(cache); return false;
        }
    }
    if (!replace && cache->uploaded && cache->identity == state->identity &&
        cache->revision == state->revision) return true;
    for (size_t i = 0; i < cells; ++i) {
        cache->scratch[4*i] = state->density[0][i];
        cache->scratch[4*i+1] = state->density[1] ? state->density[1][i] : 0.0f;
        cache->scratch[4*i+2] = foam_detail((uint32_t)i);
        cache->scratch[4*i+3] = 0.0f;
    }
    uint8_t level = 0;
    for (int side = n; side > 0; side /= 2, ++level) {
        const uint32_t words = (uint32_t)side*(uint32_t)side*4u;
        const bgfx_memory_t *memory = bgfx_alloc(words*(uint32_t)sizeof(uint16_t));
        if (!memory) {
            cache->uploaded = false;
            if (replace) jce_water_foam_gpu_destroy(cache);
            return false;
        }
        uint16_t *pixels = (uint16_t *)memory->data;
        for (uint32_t i = 0; i < words; ++i) pixels[i] = jce_float_to_half(cache->scratch[i]);
        bgfx_update_texture_2d(cache->texture, 0, level, 0, 0,
            (uint16_t)side, (uint16_t)side, memory, (uint16_t)(side*8));
        if (side > 1) (void)jce_water_gradient_reduce(cache->scratch, count, side);
    }
    cache->identity = state->identity;
    cache->revision = state->revision;
    cache->uploaded = true;
    if (replace) { jce_water_foam_gpu_destroy(*owner); *owner = cache; }
    return true;
}
