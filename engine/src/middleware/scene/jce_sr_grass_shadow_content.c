#include "jce_sr_grass_shadow_content.h"
#include <jce/os/core/jce_hash.h>
#include <math.h>
#include <string.h>

bool sr_grass_shadow_content_walk(uint32_t count,
    const JceSrGrassShadowProvider *provider, uint64_t *key)
{
    if (!provider || !provider->read || !provider->prepare || !key) return false;
    uint64_t hash = JCE_FNV1A64_INIT;
    bool complete = true;
    for (uint32_t index = 0u; index < count; ++index) {
        JceSrGrassShadowItem item = {0};
        const JceSrGrassShadowRead status = provider->read(index, &item, provider->user);
        if (status == JCE_GRASS_SHADOW_ABSENT) continue;
        if (status != JCE_GRASS_SHADOW_CASTER) {
            complete = false;
            hash = jce_hash_det64_u32(hash, UINT32_MAX);
            hash = jce_hash_det64_u32(hash, index);
            continue;
        }
        item.ready = provider->prepare(&item, provider->user);
        if (!item.ready) complete = false;
        bool accepted = item.ready;
        if (accepted && provider->submit)
            accepted = provider->submit(&item, provider->user);
        if (!accepted) complete = false;
        hash = jce_hash_det64_u64(hash, item.entity);
        hash = jce_hash_det64_u32(hash, item.placement_hash);
        hash = jce_hash_det64_u32(hash, item.geometry_hash);
        hash = jce_hash_det64_u32(hash, item.count);
        hash = jce_hash_det64_u32(hash, item.ready ? 1u : 0u);
        hash = jce_hash_det64_u32(hash, accepted ? 1u : 0u);
    }
    *key = hash;
    return complete;
}

bool sr_grass_inputs_finite(const JceGrassFieldComponent *g, const jce_mat4 *world)
{
    if (!g || !world || !memchr(g->density_mask_path, '\0', sizeof g->density_mask_path))
        return false;
    const float fields[] = {g->density, g->area_x, g->area_z, g->max_slope_deg,
        g->scale_min, g->scale_max, g->blade_height, g->blade_width,
        g->wind_dir[0], g->wind_dir[1], g->wind_speed, g->wind_amplitude,
        g->fade_start, g->fade_end, g->hue_jitter, g->density_threshold,
        g->mask_world_size, g->root_color[0], g->root_color[1], g->root_color[2],
        g->tip_color[0], g->tip_color[1], g->tip_color[2]};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; ++i)
        if (!isfinite(fields[i])) return false;
    for (unsigned col = 0u; col < 4u; ++col)
        for (unsigned row = 0u; row < 4u; ++row)
            if (!isfinite(world->raw[col][row])) return false;
    return true;
}

uint32_t sr_grass_placement_input_key(const JceGrassFieldComponent *g,
    const jce_mat4 *world, uint32_t surface_hash, float density_scale)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, &g->seed, sizeof g->seed);
    h = jce_fnv1a32_append(h, &g->density,       sizeof g->density);
    h = jce_fnv1a32_append(h, &g->area_x,        sizeof g->area_x);
    h = jce_fnv1a32_append(h, &g->area_z,        sizeof g->area_z);
    h = jce_fnv1a32_append(h, &g->max_slope_deg, sizeof g->max_slope_deg);
    h = jce_fnv1a32_append(h, &g->scale_min,     sizeof g->scale_min);
    h = jce_fnv1a32_append(h, &g->scale_max,     sizeof g->scale_max);
    /* Look fields that affect placement geometry — include so cache is invalidated
     * when blade_height/width/cards change (Task 6 uses this hash for change detection). */
    h = jce_fnv1a32_append(h, &g->blade_height,  sizeof g->blade_height);
    h = jce_fnv1a32_append(h, &g->blade_width,   sizeof g->blade_width);
    h = jce_fnv1a32_append(h, &g->cards,         sizeof g->cards);
    /* hue_jitter feeds the per-blade tint now baked into the packed instance
     * cache, so a change must invalidate (re-pack) it. */
    h = jce_fnv1a32_append(h, &g->hue_jitter,    sizeof g->hue_jitter);
    /* Full world identity invalidates placement prepared from this entity. */
    h = jce_fnv1a32_append(h, world, sizeof *world);
    h = jce_fnv1a32_append(h, &density_scale, sizeof density_scale);
    /* Density-mask trio is consumed at scatter time too (world-UV sampling). */
    h = jce_fnv1a32_append(h, g->density_mask_path,
                           strlen(g->density_mask_path));
    h = jce_fnv1a32_append(h, &g->density_threshold, sizeof g->density_threshold);
    h = jce_fnv1a32_append(h, &g->mask_world_size,   sizeof g->mask_world_size);
    h = jce_fnv1a32_append(h, &surface_hash, sizeof surface_hash);
    return h;
}


uint32_t sr_grass_complete_placement_key(uint32_t input_key, uint32_t mask_content_hash)
{
    return jce_fnv1a32_append(input_key, &mask_content_hash, sizeof mask_content_hash);
}

uint32_t sr_grass_shadow_geometry_key(const JceGrassFieldComponent *field,
    float blade_height, float blade_width, int cards, jce_vec3 camera,
    float environment_wind, float time)
{
    uint32_t hash = jce_fnv1a32_append(JCE_FNV1A32_INIT, &blade_height, sizeof blade_height);
    hash = jce_fnv1a32_append(hash, &blade_width, sizeof blade_width);
    hash = jce_fnv1a32_append(hash, &cards, sizeof cards);
    hash = jce_fnv1a32_append(hash, &camera, sizeof camera);
    const float wind[4] = {field->wind_dir[0], field->wind_dir[1],
        field->wind_speed * environment_wind, field->wind_amplitude};
    hash = jce_fnv1a32_append(hash, wind, sizeof wind);
    hash = jce_fnv1a32_append(hash, &field->fade_start, sizeof field->fade_start);
    hash = jce_fnv1a32_append(hash, &field->fade_end, sizeof field->fade_end);
    if (wind[2] != 0.0f && wind[3] != 0.0f)
        hash = jce_fnv1a32_append(hash, &time, sizeof time);
    return hash;
}
