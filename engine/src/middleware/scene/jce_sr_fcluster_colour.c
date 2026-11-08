/* FoliageCluster colour emission: whole-cluster admission and complete state. */
#include "jce_sr_fcluster_colour.h"
#include "jce_sr_colour_output.h"
#include "jce_sr_shadow_frame.h"
#include "renderer/jce_lighting_system_internal.h"

static uint32_t sr_fcluster_instance_available(uint32_t wanted, uint16_t stride, void *user)
{
    (void)user;
    return bgfx_get_avail_instance_data_buffer(wanted, stride);
}
static JceFclusterAllocation sr_fcluster_instance_allocate(uint32_t wanted,
    uint16_t stride, void *user)
{
    bgfx_instance_data_buffer_t *buffer = (bgfx_instance_data_buffer_t *)user;
    bgfx_alloc_instance_data_buffer(buffer, wanted, stride);
    JceFclusterAllocation result = {buffer->data, buffer->num};
    return result;
}

static JceGrassColourReceipt sr_fcluster_submit_colour_geometry(JceSceneRenderer *sr,
    JceScene *scene, EntityList *list, const JceFoliageClusterComponent *field,
    const JceCamera *camera, uint16_t view, const JceMesh *mesh,
    const float *instances, uint32_t count, bool instanced)
{
    JceGrassColourReceipt receipt = {count, 0u, count, JCE_FCLUSTER_COMPLETE};
    if (!count) return receipt;

    bgfx_instance_data_buffer_t data = {0};
    if (instanced) {
        const uint16_t stride = 32u;
        const JceFclusterProvider provider = {
            sr_fcluster_instance_available, sr_fcluster_instance_allocate, &data
        };
        receipt = sr_fcluster_prepare_instances(count, stride, instances, &provider);
        if (receipt.reason) return receipt;
        receipt.accepted = 0u;
        receipt.rejected = count;
    }

    const bgfx_vertex_buffer_handle_t vertex = {(uint16_t)jce_mesh_get_vbh(mesh)};
    const bgfx_index_buffer_handle_t index = {(uint16_t)jce_mesh_get_ibh(mesh)};
    if (!BGFX_HANDLE_IS_VALID(vertex) || !BGFX_HANDLE_IS_VALID(index)) {
        receipt.reason = JCE_FCLUSTER_GEOMETRY_UNAVAILABLE;
        return receipt;
    }

    /* Publish only after geometry admission. No input is inherited
     * from a previous sorted draw or a pass-start uniform update range. */
    jce_vec3 to_light = jce_v3(0.3f, 0.8f, 0.2f);
    sr_resolve_primary_dir_light(sr, scene, list, false, &to_light, NULL, NULL);
    const float light[4] = {to_light.x, to_light.y, to_light.z, 0.0f};
    const float colors[16] = {
        field->shadow_color[0], field->shadow_color[1], field->shadow_color[2], 0.0f,
        field->mid_color[0], field->mid_color[1], field->mid_color[2], 0.0f,
        field->highlight_color[0], field->highlight_color[1], field->highlight_color[2], 0.0f,
        field->color_multiplier[0], field->color_multiplier[1], field->color_multiplier[2], 1.0f
    };
    const float time[4] = {sr->water_time, 0.0f, 0.0f, 0.0f};
    bgfx_set_uniform(sr->u_foliage_time, time, 1u);
    bgfx_set_uniform(sr->u_foliage_colors, colors, 4u);
    bgfx_set_uniform(sr->u_foliage_light, light, 1u);

    JceTexture alpha = JCE_TEXTURE_INVALID;
    if (field->alpha_tex[0]) alpha = sr_resolve_texture(sr, field->alpha_tex);
    const bgfx_texture_handle_t alpha_texture = {
        jce_texture_valid(alpha) ? alpha.idx : sr->white_tex.idx
    };
    bgfx_set_texture(0u, sr->s_foliage_alpha, alpha_texture, UINT32_MAX);
    const jce_vec3 position = camera ? jce_camera_get_position(camera) : jce_v3(0, 0, 0);
    jce_lighting_system_bind_camera(position);
    sr_bind_frame_shadow_splits(sr);
    sr_apply_fog_uniforms(sr);
    jce_pbr_material_bind_view_mode();
    sr_bind_cloud_shadow(sr);

    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bgfx_set_transform(identity, 1u);
    const uint32_t cards = instanced ? 1u : count;
    bgfx_set_vertex_buffer(0u, vertex, 0u, 4u * cards);
    bgfx_set_index_buffer(index, 0u, 6u * cards);
    if (instanced) bgfx_set_instance_data_buffer(&data, 0u, count);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z |
        BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA, 0u);
    bgfx_submit(view, sr->prog_fcluster, 0u, BGFX_DISCARD_ALL);
    receipt.accepted = count;
    receipt.rejected = 0u;
    return receipt;
}

JceGrassColourReceipt sr_fcluster_submit_colour(JceSceneRenderer *sr,
    JceScene *scene, EntityList *list, const JceFoliageClusterComponent *field,
    const JceCamera *camera, uint16_t view, const float *instances, uint32_t count)
{
    return sr_fcluster_submit_colour_geometry(sr, scene, list, field, camera,
        view, sr->fcluster_quad, instances, count, true);
}

JceGrassColourReceipt sr_fcluster_submit_colour_mesh(JceSceneRenderer *sr,
    JceScene *scene, EntityList *list, const JceFoliageClusterComponent *field,
    const JceCamera *camera, uint16_t view, const JceMesh *mesh, uint32_t count)
{
    return sr_fcluster_submit_colour_geometry(sr, scene, list, field, camera,
        view, mesh, NULL, count, false);
}
