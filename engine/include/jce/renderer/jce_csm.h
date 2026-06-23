/*
 * jce_csm.h  Cascaded Shadow Map computation.
 *
 * Computes per-cascade light-space VP matrices and split distances
 * for use by the PBR shader's multi-cascade shadow lookup.
 */

#ifndef JCE_CSM_H
#define JCE_CSM_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_CSM_MAX_CASCADES 4

typedef struct {
    /* View-space split distances: [0]=near, [cascade_count]=far. */
    float    splits[JCE_CSM_MAX_CASCADES + 1];
    /* Light-space VP matrix per cascade. */
    jce_mat4 vp[JCE_CSM_MAX_CASCADES];
    /* World-space bounding sphere of each cascade's camera-frustum slice
     * (centre + radius).  Exposed so the shadow pass can cull casters that lie
     * outside a cascade's coverage (per-cascade caster culling), bounding the
     * per-frame shadow draws/uniforms.  Cull conservatively (add a margin toward
     * the light) so casters between the sphere and the sun are not dropped. */
    jce_vec3 center[JCE_CSM_MAX_CASCADES];
    float    radius[JCE_CSM_MAX_CASCADES];
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
 * @param split_lambda   Practical split blend (0=linear, 1=logarithmic).
 * @param caster_aabb_min World-space min corner of the shadow-caster bounds, or
 *                        NULL. When supplied, each cascade's light-space NEAR
 *                        plane is extended toward the light to enclose casters
 *                        that sit between the camera-frustum cascade sphere and
 *                        the sun (e.g. tall buildings).  Without this the near
 *                        plane is anchored to the camera sphere and tall casters
 *                        are clipped out of the shadow map, so their shadows
 *                        truncate and the truncation moves with the camera.
 * @param caster_aabb_max World-space max corner of the shadow-caster bounds, or
 *                        NULL (disables the near-plane extension; legacy fit).
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
                     uint16_t shadow_map_size,
                     float split_lambda,
                     const jce_vec3 *caster_aabb_min,
                     const jce_vec3 *caster_aabb_max);

JCE_EXTERN_C_END

#endif /* JCE_CSM_H */
