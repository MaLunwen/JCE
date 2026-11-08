/* Scene-owned nearshore preparation and shared water-height query. */
#include "jce_terrain_cache.h"
#include "jce_terrain_surface_internal.h"
#include "jce_water_field_internal.h"
#include "jce_water_nearshore_config.h"
#include <jce/middleware/scene/jce_component_registry.h>

#include <math.h>

static bool shore_region(const JceWaterNearshoreComponent *config, const JceWaterFieldDesc *water,
    JceWaterShoreBinding *binding)
{
    const float half = 0.5f * config->extent;
    if (!isfinite(water->base_height) ||
        !(fabsf(config->offset_x) + half <= 0.5f * water->size_x) ||
        !(fabsf(config->offset_z) + half <= 0.5f * water->size_z)) return false;
    binding->still_y = water->base_height;
    binding->min_x = water->center_x + config->offset_x - half;
    binding->min_z = water->center_z + config->offset_z - half;
    binding->extent = config->extent;
    binding->drag = config->drag;
    binding->absorption = config->boundary_absorption;
    return isfinite(binding->min_x) && isfinite(binding->min_z);
}

void jce_scene_prepare_water_nearshore(JceScene *scene, JceEntity water,
    const JceWaterFieldDesc *desc, const JceWaterField *field)
{
    JceWaterFieldSet *set = jce_scene_water_fields(scene);
    const JceWaterNearshoreComponent *config = jce_scene_get_water_nearshore(scene, water);
    JceWaterShoreBinding binding = {0};
    if (field && jce_water_field_fft(field) && jce_water_nearshore_valid(config) && config->terrain &&
        jce_scene_comp_enabled(scene, water, jce_component_find("WaterNearshore")) &&
        jce_scene_entity_alive(scene, config->terrain) && shore_region(config, desc, &binding)) {
        const JceTerrainComponent *terrain = jce_scene_get_terrain(scene, config->terrain);
        if (terrain && terrain->terrain_path[0] &&
            jce_scene_comp_enabled(scene, config->terrain, jce_component_find("Terrain"))) {
            JceTerrainCache *cache = jce_scene_terrain_cache(scene);
            JceTerrain *resident = jce_terrain_cache_peek(cache, terrain->terrain_path);
            binding.terrain = config->terrain;
            binding.terrain_revision = jce_terrain_cache_revision(cache, terrain->terrain_path);
            binding.terrain_world = jce_scene_get_world_matrix(scene, config->terrain);
            if (resident && binding.terrain_revision) {
                if (jce_water_field_set_shore_matches(set, water, &binding)) return;
                const int side = jce_water_nearshore_grid_side(config->extent);
                const JceWaterShoreStateDesc grid = {
                    .side = side, .cell_size = config->extent / side,
                    .min_x = binding.min_x, .min_z = binding.min_z, .still_y = binding.still_y,
                    .max_incoming_height = 1.5f, .max_incoming_speed = 3.0f,
                    .water = {config->drag, 0.02f, 0.8f, 0.03f, 8.0f, 0.15f, config->boundary_absorption},
                    .foam = {1.5f, 0.7f, 0.35f, 3.0f, 0.5f},
                };
                float bed[JCE_WATER_SHORE_MAX_SIDE * JCE_WATER_SHORE_MAX_SIDE];
                if (jce_terrain_surface_sample_bed(resident, &binding.terrain_world,
                        grid.side, grid.cell_size, grid.min_x, grid.min_z, binding.still_y, bed)) {
                    jce_water_field_set_configure_shore(set, water, &binding, &grid, bed);
                    return;
                }
            }
        }
    }
    /* A removed, unready or reauthored terrain cannot publish the old bed. */
    jce_water_field_set_configure_shore(set, water, NULL, NULL, NULL);
}

bool JCE_CALL jce_scene_water_nearshore_surface(const JceScene *scene, JceEntity water,
    float world_x, float world_z, float ambient_y, float *surface_y, float *coverage)
{
    if (!scene || !surface_y || !coverage || !isfinite(ambient_y)) return false;
    JceScene *s = (JceScene *)scene; /* getters borrow this scene's component and field set */
    if (!jce_scene_has_water_nearshore(s, water) || !jce_scene_has_water(s, water) ||
        !jce_scene_comp_enabled(s, water, jce_component_find("WaterNearshore"))) return false;
    const JceWaterFieldSet *set = jce_scene_water_fields(s);
    JceWaterShoreStateSample sample;
    if (!jce_water_shore_state_sample(jce_water_field_set_shore(set, water),
            world_x, world_z, &sample)) return false;
    const float t = fminf(1.0f, fmaxf(0.0f, (sample.depth - 0.001f) / 0.009f));
    const float wet = t * t * (3.0f - 2.0f * t);
    *surface_y = ambient_y + (sample.surface_y - ambient_y) * sample.weight;
    *coverage = 1.0f + (wet - 1.0f) * sample.weight;
    return true;
}
