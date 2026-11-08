/* Grass colour ranges publish complete final state after instance admission. */
#include "jce_sr_grass_colour.h"
#include "jce_sr_colour_output.h"
#include "renderer/jce_lighting_system_internal.h"
#include "renderer/jce_render_encoder.h"
#include <string.h>

void sr_grass_bind_geometry_uniforms(JceSceneRenderer *sr,
    const JceGrassFieldComponent *field, const JceCamera *camera)
{
    const jce_vec3 position = camera ? jce_camera_get_position(camera) : jce_v3(0.0f, 0.0f, 0.0f);
    const float time[4] = {sr->grass_time, 0.0f, 0.0f, 0.0f};
    const float wind[4] = {field->wind_dir[0], field->wind_dir[1],
        field->wind_speed * jce_environment_wind_speed_now(sr->env), field->wind_amplitude};
    const float fade[4] = {field->fade_start, field->fade_end, field->hue_jitter, 0.0f};
    const float camera_pos[4] = {position.x, position.y, position.z, 0.0f};
    bgfx_set_uniform(sr->u_grass_time, time, 1u);
    bgfx_set_uniform(sr->u_grass_wind, wind, 1u);
    bgfx_set_uniform(sr->u_grass_fade, fade, 1u);
    bgfx_set_uniform(sr->u_grass_camera, camera_pos, 1u);
}

/* Grass owns the final shader inputs once per admitted ALL-discard submit.
 * Geometry owns u_cameraPos; the directional light path deliberately omits it.
 * No default PBR material writes are needed by this standalone grass shader. */
static void sr_grass_bind_colour_uniforms(JceSceneRenderer *sr,
    const JceGrassFieldComponent *field, const JceCamera *camera,
    uint32_t receiver_layer)
{
    sr_grass_bind_geometry_uniforms(sr, field, camera);
    const float colors[8] = {
        field->root_color[0], field->root_color[1], field->root_color[2], 1.0f,
        field->tip_color[0], field->tip_color[1], field->tip_color[2], 1.0f
    };
    bgfx_set_uniform(sr->u_grass_color, colors, 2);
    jce_light_env_apply_directional_for_layer(sr->light_env, sr->renderer,
        receiver_layer);
    sr_bind_frame_shadow_state(sr);
    sr_bind_cloud_shadow(sr);
    float output[4];
    sr_pack_colour_output(jce_texture_colour_space(),
        sr->postfx_tonemap_active, output);
    jce_enc_set_uniform(sr->u_ibl_params, output, 1);
    sr_apply_fog_uniforms(sr);
    jce_pbr_material_bind_view_mode();
}

/* Submit a contiguous [start, start+n) blade slice of the cell-ordered packed
 * cache with the partial-fill instance loop (mirrors the model instancer). */
typedef struct SrGrassColourRange {
    JceSceneRenderer *sr;
    const JceGrassFieldComponent *field;
    const JceCamera *camera;
    uint32_t receiver_layer;
    int slot;
    bgfx_program_handle_t program;
    uint64_t state;
    uint16_t view, stride;
    bgfx_vertex_buffer_handle_t vertex;
    bgfx_index_buffer_handle_t index;
    uint32_t vertices, indices, start;
    bool resident, expanded;
} SrGrassColourRange;
static uint32_t sr_grass_colour_available(uint32_t wanted, uint16_t stride, void *user)
{
    const SrGrassColourRange *range = (const SrGrassColourRange *)user;
    return range->resident || range->expanded ? wanted
        : bgfx_get_avail_instance_data_buffer(wanted, stride);
}
static bool sr_grass_colour_emit(uint32_t offset, uint32_t count, void *user)
{
    SrGrassColourRange *range = (SrGrassColourRange *)user;
    JceSceneRenderer *sr = range->sr;
    if (range->expanded) {
        if (!sr_grass_expanded_bind(sr, range->slot, range->start + offset, count))
            return false;
    } else if (range->resident) {
        bgfx_set_instance_data_from_dynamic_vertex_buffer(
            sr->grass_cache[range->slot].inst_vb, range->start + offset, count);
    } else {
        bgfx_instance_data_buffer_t data;
        bgfx_alloc_instance_data_buffer(&data, count, range->stride);
        if (!data.data || data.num < count) return false;
        const uint8_t *packed = sr->grass_cache[range->slot].packed;
        if (packed) {
            memcpy(data.data, packed + (size_t)(range->start + offset) * range->stride,
                   (size_t)count * range->stride);
        } else {
            const JceFoliageInstance *instances = sr->grass_cache[range->slot].insts;
            if (!instances) return false;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t item = range->start + offset + i;
                jce_mat4 matrix = sr_foliage_instance_matrix(&instances[item], 0.0f);
                float t = (float)(item & 7u) / 7.0f;
                float hue = (t - 0.5f) * 2.0f * range->field->hue_jitter;
                jce_vec4 tint = {1.0f - hue, 1.0f, 1.0f + hue, 1.0f};
                uint8_t *dst = (uint8_t *)data.data + (size_t)i * range->stride;
                memcpy(dst, JCE_M4_PTR(matrix), sizeof(matrix));
                memcpy(dst + sizeof(matrix), &tint, sizeof(tint));
            }
        }
        bgfx_set_instance_data_buffer(&data, 0, count);
    }
    if (!range->expanded) {
        bgfx_set_vertex_buffer(0, range->vertex, 0, range->vertices);
        bgfx_set_index_buffer(range->index, 0, range->indices);
    }
    sr_grass_bind_colour_uniforms(sr, range->field, range->camera,
        range->receiver_layer);
    bgfx_set_state(range->state, 0);
    bgfx_submit(range->view, range->program, 0, BGFX_DISCARD_ALL);
    if (range->expanded && !sr->grass_expanded_submitted) {
        sr->grass_expanded_submitted = true;
        LOG_INFO(LOG_TAG, "grass GL2.1 expanded blades submitted count=%u", count);
    }
    return true;
}
JceGrassColourReceipt sr_grass_submit_range(JceSceneRenderer *sr, int slot,
    const JceGrassFieldComponent *field, const JceCamera *camera,
    uint32_t receiver_layer, bgfx_program_handle_t program, uint64_t state,
    uint16_t view, uint16_t stride, bgfx_vertex_buffer_handle_t vertex,
    bgfx_index_buffer_handle_t index, uint32_t vertices, uint32_t indices,
    uint32_t start, uint32_t count)
{
    const uint32_t total = sr->grass_cache[slot].inst_count;
    SrGrassColourRange range = {sr, field, camera, receiver_layer, slot, program, state, view, stride,
        vertex, index, vertices, indices, start, false, sr_grass_expanded_mode()};
    range.resident = !range.expanded && sr_grass_persist_enabled() &&
        sr_grass_roots_ensure(sr, slot) &&
        sr->grass_cache[slot].inst_vb_count == total && start <= total && count <= total - start;
    const JceGrassColourProvider provider = {
        sr_grass_colour_available, sr_grass_colour_emit, &range
    };
    return sr_grass_colour_dispatch(count, stride, &provider);
}
