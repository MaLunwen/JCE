/* Current static foliage content is prepared before CSM cache admission.
 * Authored fields stream through the existing bounded cache during each pass. */
#include "jce_sr_internal.h"
#include "jce_sr_foliage_identity.h"
#include "jce_sr_foliage_shadow_key.h"
#include "renderer/jce_model_instancing.h"
#include <jce/os/core/jce_hash.h>

typedef struct {
    JceSceneRenderer *sr;
    JceScene *scene;
    EntityList *list;
} SrFoliageShadowContext;

static bool sr_foliage_shadow_read(uint32_t index,
    JceSrFoliageShadowItem *item, void *user)
{
    SrFoliageShadowContext *ctx = user;
    JceEntity entity = ctx->list->entities[index];
    JceVegetationScatterComponent *vs =
        jce_scene_get_vegetation_scatter(ctx->scene, entity);
    if (!vs || !jce_scene_has_transform(ctx->scene, entity)) return false;
    static int component_id = -2;
    if (component_id == -2) component_id = jce_component_find("VegetationScatter");
    item->entity = entity;
    item->visible = vs->visible && (component_id < 0 ||
        jce_scene_comp_enabled(ctx->scene, entity, component_id));
    item->cast_shadow = vs->cast_shadow &&
        (vs->mesh_path[0] || sr_foliage_roots_should_persist(ctx->sr));
    item->geometry_hash = sr_foliage_geometry_hash(vs);
    return true;
}

static int sr_foliage_shadow_prepare_slot(SrFoliageShadowContext *ctx,
    JceSrFoliageShadowItem *item)
{
    JceSceneRenderer *sr = ctx->sr;
    const int slot = sr_foliage_prepare_roots(sr, ctx->scene, ctx->list,
        item->entity, NULL, &item->placement_hash);
    if (slot < 0) return -1;
    item->count = sr->foliage_cache[slot].inst_count;
    if (item->count == 0 || sr->foliage_cache[slot].tiles) return slot;
    JceVegetationScatterComponent *vs =
        jce_scene_get_vegetation_scatter(ctx->scene, item->entity);
    /* Model depth uses all node-local transforms through the CPU-root model
     * traversal. A raw root VB is correct only for the shared primitive mesh. */
    if (vs->mesh_path[0]) return slot;
    if (!sr_foliage_roots_ensure(sr, slot, item->count, sr->foliage_cull_enabled))
        return -1;
    return slot;
}

static bool sr_foliage_shadow_prepare(JceSrFoliageShadowItem *item, void *user)
{
    SrFoliageShadowContext *ctx = user;
    const int slot = sr_foliage_shadow_prepare_slot(ctx, item);
    if (slot < 0) return false;
    /* Tiled colour continues its existing visible-neighbourhood streaming.
     * Key the resident subset so later tile arrivals cannot retain empty depth. */
    if (ctx->sr->foliage_cache[slot].tiles) {
        const int count = ctx->sr->foliage_cache[slot].tiles_x *
                          ctx->sr->foliage_cache[slot].tiles_z;
        uint64_t tiles = JCE_FNV1A64_INIT;
        for (int i = 0; i < count; ++i) {
            const struct SrFoliageTile *tile = &ctx->sr->foliage_cache[slot].tiles[i];
            if (!tile->resident) continue;
            tiles = jce_hash_det64_u32(tiles, (uint32_t)i);
            tiles = jce_hash_det64_u32(tiles, tile->count);
        }
        item->geometry_hash = jce_fnv1a32_append(item->geometry_hash,
            &tiles, sizeof tiles);
    }
    return true;
}

uint64_t sr_prepare_scatter_shadows(JceSceneRenderer *sr, JceScene *scene,
    EntityList *list)
{
    SrFoliageShadowContext context = {sr, scene, list};
    const JceSrFoliageShadowProvider provider = {
        sr_foliage_shadow_read, sr_foliage_shadow_prepare, &context
    };
    uint64_t key;
    (void)sr_foliage_shadow_key_walk((uint32_t)list->count, &provider, &key);
    return key;
}

bool sr_submit_scatter_shadows(JceSceneRenderer *sr, JceScene *scene,
    EntityList *list, uint16_t cv, uint16_t shadow_inst_idx)
{
    if (!scene || !list) return false;
    static int shadow_lod = -2;
    if (shadow_lod == -2) {
        const char *value = getenv("JCE_FOLIAGE_SHADOW_LOD");
        shadow_lod = value && value[0] ? atoi(value) : 0x7fffffff;
    }
    const uint32_t level = shadow_lod < 0 ? 0u : (uint32_t)shadow_lod;
    SrFoliageShadowContext context = {sr, scene, list};
    bool complete = true;
    for (int i = 0; i < list->count; ++i) {
        JceSrFoliageShadowItem item;
        memset(&item, 0, sizeof item);
        if (!sr_foliage_shadow_read((uint32_t)i, &item, &context) ||
            !item.visible || !item.cast_shadow) continue;
        const int slot = sr_foliage_shadow_prepare_slot(&context, &item);
        if (slot < 0) { complete = false; continue; }
        const uint32_t n = sr->foliage_cache[slot].inst_count;
        if (n == 0) continue;
        if (shadow_inst_idx == UINT16_MAX) { complete = false; continue; }
        JceVegetationScatterComponent *vs =
            jce_scene_get_vegetation_scatter(scene, item.entity);
        if (sr->foliage_cache[slot].tiles && vs->mesh_path[0]) {
            SrModelCache *mc = sr_get_model(sr, vs->mesh_path, (uint32_t)item.entity);
            if (!mc || !mc->model) { complete = false; continue; }
            const int count = sr->foliage_cache[slot].tiles_x *
                              sr->foliage_cache[slot].tiles_z;
            for (int t = 0; t < count; ++t) {
                const struct SrFoliageTile *tile = &sr->foliage_cache[slot].tiles[t];
                if (!tile->resident || tile->count == 0) continue;
                jce_model_draw_shadow_instanced_lod(mc->model, sr->renderer, cv,
                    shadow_inst_idx, tile->vb.idx, tile->count, level);
            }
            continue;
        }
        if (vs->mesh_path[0]) {
            SrModelCache *mc = sr_get_model(sr, vs->mesh_path, (uint32_t)item.entity);
            if (!mc || !mc->model ||
                !jce_model_draw_shadow_instanced_cpu_lod(mc->model, sr->renderer,
                    cv, sr->foliage_cache[slot].roots, n, level)) complete = false;
            continue;
        }
        if (sr->foliage_cache[slot].inst_vb_count != n ||
            sr->foliage_cache[slot].inst_vb_hash != item.placement_hash ||
            sr->foliage_cache[slot].inst_vb.idx == UINT16_MAX) {
            complete = false;
            continue;
        }
        JceMesh *mesh;
        switch (vs->mesh_shape) {
        default:
        case 0: mesh = sr->cube_mesh; break;
        case 1: mesh = sr->sphere_mesh; break;
        case 2: mesh = sr->plane_mesh; break;
        case 3: mesh = sr->capsule_mesh; break;
        case 4: mesh = sr->cylinder_mesh; break;
        }
        if (!mesh) { complete = false; continue; }
        bgfx_vertex_buffer_handle_t vb = {(uint16_t)jce_mesh_get_vbh(mesh)};
        bgfx_index_buffer_handle_t ib = {(uint16_t)jce_mesh_get_ibh(mesh)};
        bgfx_program_handle_t program = {shadow_inst_idx};
        bgfx_set_vertex_buffer(0, vb, 0, UINT32_MAX);
        if (ib.idx != UINT16_MAX)
            bgfx_set_index_buffer(ib, 0, jce_mesh_index_count(mesh));
        bgfx_set_instance_data_from_dynamic_vertex_buffer(
            sr->foliage_cache[slot].inst_vb, 0, n);
        bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
            BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);
        bgfx_submit(cv, program, 0, BGFX_DISCARD_ALL);
    }
    return complete;
}
