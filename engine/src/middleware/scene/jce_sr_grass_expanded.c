/* Bounded desktop GL 2.1 grass geometry. Roots and blade shape retain the
 * normal placement/cache owners. These buffers replace only the instance port. */
#include "jce_sr_internal.h"
#include <math.h>
#include <string.h>

#define SR_GRASS_EXPANDED_VERTEX_LIMIT 16384u
#define SR_GRASS_PACKED_ROOT_FLOATS 20u

typedef struct SrGrassExpandedVertex {
    float position[3];
    float root[3];
    float scale_tint[4];
    float uv[2];
} SrGrassExpandedVertex;

bool sr_grass_expanded_mode(void)
{
    const JceRendererBackend backend = jce_renderer_get_active_backend();
    if (backend != JCE_BACKEND_OPENGL ||
        (jce_renderer_get_caps() & JCE_CAP_INSTANCING) != 0)
        return false;
    const JceRendererApiInfo api = jce_renderer_get_api_info();
    return jce_graphics_api_is_compatibility_gl_es(api.build_tier, backend);
}

uint32_t sr_grass_expanded_root_limit(int cards)
{
    if (cards < 1 || cards > 6) return 0u;
    return SR_GRASS_EXPANDED_VERTEX_LIMIT / ((uint32_t)cards * JCE_GRASS_BLADE_VERTS);
}

void sr_grass_expanded_clear(JceSceneRenderer *sr, int slot)
{
    if (sr->grass_cache[slot].expanded_count == 0u) return;
    bgfx_destroy_vertex_buffer(sr->grass_cache[slot].expanded_vb);
    bgfx_destroy_index_buffer(sr->grass_cache[slot].expanded_ib);
    sr->grass_cache[slot].expanded_count = 0u;
}

bool sr_grass_expanded_rebuild(JceSceneRenderer *sr, int slot, uint32_t count)
{
    if (count == 0u) {
        sr_grass_expanded_clear(sr, slot);
        return true;
    }

    const int cards = sr->grass_cache[slot].blade_cards;
    const uint8_t *packed = sr->grass_cache[slot].packed;
    if (!packed || count > sr_grass_expanded_root_limit(cards)) return false;
    const uint32_t vertices_per_root = (uint32_t)cards * JCE_GRASS_BLADE_VERTS;
    const uint32_t indices_per_root = (uint32_t)cards * JCE_GRASS_BLADE_INDICES;
    JceMeshVertex blade[6u * JCE_GRASS_BLADE_VERTS];
    uint32_t blade_indices[6u * JCE_GRASS_BLADE_INDICES];
    sr_grass_fill_blade(sr->grass_cache[slot].blade_h, sr->grass_cache[slot].blade_w,
                        cards, blade, blade_indices);

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_NORMAL, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TANGENT, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);

    const uint32_t total_vertices = count * vertices_per_root;
    const bgfx_memory_t *vmem = bgfx_alloc(total_vertices * (uint32_t)sizeof(SrGrassExpandedVertex));
    if (!vmem) return false;
    SrGrassExpandedVertex *out = (SrGrassExpandedVertex *)vmem->data;
    for (uint32_t root = 0; root < count; ++root) {
        float data[SR_GRASS_PACKED_ROOT_FLOATS];
        memcpy(data, packed + (size_t)root * sizeof data, sizeof data);
        const float scale = sqrtf(data[0] * data[0] + data[1] * data[1] + data[2] * data[2]);
        for (uint32_t k = 0; k < vertices_per_root; ++k, ++out) {
            memcpy(out->position, blade[k].pos, sizeof out->position);
            memcpy(out->root, data + 12u, sizeof out->root);
            out->scale_tint[0] = scale;
            memcpy(out->scale_tint + 1u, data + 16u, 3u * sizeof(float));
            memcpy(out->uv, blade[k].uv, sizeof out->uv);
        }
    }
    const bgfx_vertex_buffer_handle_t vb = bgfx_create_vertex_buffer(vmem, &layout, BGFX_BUFFER_NONE);
    if (!BGFX_HANDLE_IS_VALID(vb)) return false;
    const bgfx_memory_t *imem = bgfx_alloc(count * indices_per_root * (uint32_t)sizeof(uint16_t));
    if (!imem) {
        bgfx_destroy_vertex_buffer(vb);
        return false;
    }
    uint16_t *indices = (uint16_t *)imem->data;
    for (uint32_t root = 0; root < count; ++root) {
        for (uint32_t k = 0; k < indices_per_root; ++k)
            *indices++ = (uint16_t)(root * vertices_per_root + blade_indices[k]);
    }
    const bgfx_index_buffer_handle_t ib = bgfx_create_index_buffer(imem, BGFX_BUFFER_NONE);
    if (!BGFX_HANDLE_IS_VALID(ib)) {
        bgfx_destroy_vertex_buffer(vb);
        return false;
    }
    sr_grass_expanded_clear(sr, slot);
    sr->grass_cache[slot].expanded_vb = vb;
    sr->grass_cache[slot].expanded_ib = ib;
    sr->grass_cache[slot].expanded_count = count;
    sr->grass_cache[slot].expanded_vertices = total_vertices;
    sr->grass_cache[slot].expanded_indices_per_root = indices_per_root;
    return true;
}

bool sr_grass_expanded_bind(JceSceneRenderer *sr, int slot, uint32_t start, uint32_t count)
{
    const uint32_t total = sr->grass_cache[slot].expanded_count;
    if (!count || start >= total || count > total - start) return false;
    const uint32_t per_root = sr->grass_cache[slot].expanded_indices_per_root;
    bgfx_set_vertex_buffer(0u, sr->grass_cache[slot].expanded_vb, 0u,
                           sr->grass_cache[slot].expanded_vertices);
    bgfx_set_index_buffer(sr->grass_cache[slot].expanded_ib, start * per_root, count * per_root);
    return true;
}
