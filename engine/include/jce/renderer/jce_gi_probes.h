/*
 * jce_gi_probes.h  Light probe volume (SH3 grid) for indirect lighting.
 *
 * Stores a 3D grid of L2 (9-band) spherical harmonic coefficients per
 * RGB channel.  Each grid cell holds the irradiance environment of
 * its world-space sample point; runtime trilinear-samples the volume
 * at any world position to feed forward shaders' ambient term.
 *
 * Storage layout: tightly packed
 *   coeffs[grid_z][grid_y][grid_x][3 channels][9 bands]
 * With dimension defaults (8×8×8) this is 8³ × 27 floats = 13.5 KB.
 *
 * The bake pass (a future Batch 10 task) populates this volume by
 * sampling the scene's IBL + emissive + bounced GI at each grid cell
 * and projecting the resulting radiance onto SH.  This module just
 * owns the storage and the runtime sampling — bake-side code lives
 * elsewhere.
 *
 * Layer: renderer (Layer 5) — public.
 */

#ifndef JCE_GI_PROBES_H
#define JCE_GI_PROBES_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SH_BANDS 9   /* L2 — 9 coefficients per channel */

typedef struct JceLightProbeVolume JceLightProbeVolume;

/* Create a volume sized to cover an axis-aligned world-space box.
 * grid_x/y/z must each be at least 2.  Returns NULL on OOM. */
JCE_API JceLightProbeVolume *jce_gi_probes_create(jce_vec3 box_min,
                                                  jce_vec3 box_max,
                                                  uint32_t grid_x,
                                                  uint32_t grid_y,
                                                  uint32_t grid_z);
JCE_API void jce_gi_probes_destroy(JceLightProbeVolume *vol);

/* Volume metadata. */
JCE_API jce_vec3 jce_gi_probes_box_min(const JceLightProbeVolume *v);
JCE_API jce_vec3 jce_gi_probes_box_max(const JceLightProbeVolume *v);
JCE_API uint32_t jce_gi_probes_grid_x(const JceLightProbeVolume *v);
JCE_API uint32_t jce_gi_probes_grid_y(const JceLightProbeVolume *v);
JCE_API uint32_t jce_gi_probes_grid_z(const JceLightProbeVolume *v);

/* Direct cell access — `sh_rgb` must point to an array of
 * JCE_SH_BANDS × 3 floats laid out as [band][channel] (RGB channels
 * interleaved per band, the order most shaders expect for upload). */
JCE_API void jce_gi_probes_set_cell(JceLightProbeVolume *v,
                                    uint32_t x, uint32_t y, uint32_t z,
                                    const float *sh_rgb);
JCE_API void jce_gi_probes_get_cell(const JceLightProbeVolume *v,
                                    uint32_t x, uint32_t y, uint32_t z,
                                    float *out_sh_rgb);

/* Trilinear-sample at a world position.  Out-of-volume samples are
 * clamped to the nearest cell.  out_sh_rgb receives 27 floats.
 *
 * The shader-side counterpart evaluates these against the surface
 * normal to compute the irradiance:
 *   irradiance = c0 + c1*y + c2*z + c3*x + ...   (standard L2 form)
 */
JCE_API void jce_gi_probes_sample(const JceLightProbeVolume *v,
                                  jce_vec3 world_pos,
                                  float *out_sh_rgb);

/* ── Persistence ──────────────────────────────────────────────────── *
 *
 * Binary `.sh3` file format:
 *   magic         u32  'S' 'H' '3' '0'
 *   version       u32  1
 *   box_min       3*f32
 *   box_max       3*f32
 *   grid_xyz      3*u32
 *   coeffs        f32 × grid_x*grid_y*grid_z*JCE_SH_BANDS*3
 */
JCE_API bool jce_gi_probes_save(const JceLightProbeVolume *v, const char *path);
JCE_API JceLightProbeVolume *jce_gi_probes_load(const char *path);

JCE_EXTERN_C_END

#endif /* JCE_GI_PROBES_H */
