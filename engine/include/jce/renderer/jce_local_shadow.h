/*
 * jce_local_shadow.h  Pure helpers for local (spot/point) light shadows.
 *
 * Both spot and point shadow casters share ONE mechanism: a perspective
 * depth tile packed into a shadow atlas. This module holds the pure math
 * (no bgfx): the per-light view-proj and the atlas tile rect.
 *
 * Layer: Renderer (Layer 3) — pure math.
 */

#ifndef JCE_LOCAL_SHADOW_H
#define JCE_LOCAL_SHADOW_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Perspective light-view-proj for a local shadow caster.
 *
 * pos:       light world position.
 * dir:       aim direction (need not be normalized). For a spot this is the
 *            cone axis; for a point it is the chosen hemisphere aim (e.g. down).
 * fov_rad:   vertical FOV, clamped to [0.01, 3.10] rad. Spot = 2*acos(outerCos);
 *            point = a wide angle (~2.4 rad).
 * near_z/far_z: depth range (caller picks, e.g. 0.05*radius .. radius).
 * homogeneous_depth: bgfx caps flag (GL = true [-1,1], D3D/Metal = false [0,1]).
 *
 * Returns proj*view. Uses a safe up-vector when dir is near-vertical so the
 * look-at never degenerates to NaN. */
JCE_API jce_mat4 jce_local_shadow_vp(jce_vec3 pos, jce_vec3 dir,
                             float fov_rad, float near_z, float far_z,
                             bool homogeneous_depth);

/* Pixel rect of shadow tile `slot` in a `tiles_per_side` x `tiles_per_side`
 * grid packed into a square atlas of `atlas_size` px (row-major: slot 0 =
 * top-left, increasing left-to-right then top-to-bottom).
 *
 * Writes the tile origin (out_x,out_y) and edge length (out_size).
 * Returns false (and writes nothing) if slot is out of range or inputs are
 * degenerate. */
JCE_API bool jce_local_shadow_atlas_tile(uint32_t slot, uint32_t atlas_size,
                                 uint32_t tiles_per_side,
                                 uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_size);

JCE_EXTERN_C_END

#endif /* JCE_LOCAL_SHADOW_H */
