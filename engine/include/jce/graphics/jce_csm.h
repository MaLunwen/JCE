/*
 * jce_csm.h  Cascaded Shadow Map computation.
 *
 * Computes per-cascade light-space VP matrices and split distances
 * for use by the PBR shader's multi-cascade shadow lookup.
 */

#ifndef JCE_CSM_H
#define JCE_CSM_H

#include <jce/core/jce_math.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_CSM_MAX_CASCADES 4

typedef struct {
    /* View-space split distances: [0]=near, [cascade_count]=far. */
    float    splits[JCE_CSM_MAX_CASCADES + 1];
    /* Light-space VP matrix per cascade. */
    jce_mat4 vp[JCE_CSM_MAX_CASCADES];
    uint32_t cascade_count;
} JceCsmData;

/*
 * Compute cascade split distances and per-cascade VP matrices.
 *
 * @param out            Output CSM data.
 * @param cascade_count  Number of cascades (1..4).
 * @param near_plane     Camera near plane.
 * @param far_plane      Camera far plane (or shadow distance).
 * @param fov_deg        Camera vertical FOV in degrees.
 * @param aspect         Camera aspect ratio (width/height).
 * @param camera_view    Camera view matrix.
 * @param light_dir      Normalized light direction (world space).
 * @param homogeneous_depth  bgfx homogeneous depth flag.
 * @param shadow_map_size Shadow map resolution used by cascades.
 */
void jce_csm_compute(JceCsmData *out,
                     uint32_t cascade_count,
                     float near_plane,
                     float far_plane,
                     float fov_deg,
                     float aspect,
                     const jce_mat4 *camera_view,
                     const jce_vec3 *light_dir,
                     bool homogeneous_depth,
                     uint16_t shadow_map_size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_CSM_H */
