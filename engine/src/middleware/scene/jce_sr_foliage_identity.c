/* Authored foliage identities shared by preparation and shadow caching. */
#include "jce_sr_foliage_identity.h"
#include <jce/os/core/jce_hash.h>
#include <string.h>

uint32_t sr_foliage_param_hash(const JceVegetationScatterComponent *scatter,
                               const jce_vec3 *origin,
                               uint32_t surface_hash)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, &scatter->seed,
                                    sizeof scatter->seed);
    h = jce_fnv1a32_append(h, &scatter->density, sizeof scatter->density);
    h = jce_fnv1a32_append(h, &scatter->area_x, sizeof scatter->area_x);
    h = jce_fnv1a32_append(h, &scatter->area_z, sizeof scatter->area_z);
    h = jce_fnv1a32_append(h, &scatter->max_slope_deg,
                           sizeof scatter->max_slope_deg);
    h = jce_fnv1a32_append(h, &scatter->scale_min,
                           sizeof scatter->scale_min);
    h = jce_fnv1a32_append(h, &scatter->scale_max,
                           sizeof scatter->scale_max);
    h = jce_fnv1a32_append(h, &scatter->align_to_normal,
                           sizeof scatter->align_to_normal);
    h = jce_fnv1a32_append(h, origin, sizeof *origin);
    h = jce_fnv1a32_append(h, &surface_hash, sizeof surface_hash);
    h = jce_fnv1a32_append(h, scatter->baked_placement_path,
        (uint32_t)strlen(scatter->baked_placement_path));
    h = jce_fnv1a32_append(h, scatter->density_mask_path,
        (uint32_t)strlen(scatter->density_mask_path));
    h = jce_fnv1a32_append(h, &scatter->density_paint_active,
                           sizeof scatter->density_paint_active);
    if (scatter->density_paint_active) {
        h = jce_fnv1a32_append(h, scatter->density_paint,
                               (uint32_t)sizeof scatter->density_paint);
    }
    return h;
}

uint32_t sr_foliage_geometry_hash(const JceVegetationScatterComponent *scatter)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, scatter->mesh_path,
        strlen(scatter->mesh_path));
    return jce_fnv1a32_append(h, &scatter->mesh_shape, sizeof scatter->mesh_shape);
}
