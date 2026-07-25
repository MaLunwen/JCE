/*
 * jce_gi_probes.h  GI L1 — dynamic irradiance probe grid (Lumen-lite lvl 1).
 *
 * A camera-following, world-anchored (toroidal) grid of SH9 irradiance
 * probes, refreshed every frame by a compute gather over the PREVIOUS
 * frame's lit color + depth (single-bounce + sky, temporal hysteresis) and
 * read back to the CPU on a rolling latency so the scene renderer can feed
 * the EXISTING u_sh9 / u_giParams baked-GI path per submit — fs_pbr needs
 * no new sampler (all 16 stages are occupied) and no shader change.
 *
 * Layer: Renderer (Layer 1).
 */

#ifndef JCE_GI_PROBES_H
#define JCE_GI_PROBES_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceGiProbes  JceGiProbes;
typedef struct JcePakArchive JcePakArchive;

/* Create the probe system (loads cs_gi_gather from `pak`, falling back to
 * the embedded engine shader pak).  Returns NULL when compute/image caps or
 * the program are unavailable — callers just skip GI. */
JCE_API JceGiProbes *jce_gi_probes_create(const JcePakArchive *pak);
JCE_API void         jce_gi_probes_destroy(JceGiProbes *gi);

/* One update per color pass: dispatches the gather on `compute_view`
 * (reading prev-frame `color_tex` + `depth_tex` with `prev_vp`), then kicks
 * the rolling atlas readback via a blit on `blit_view` (must be ordered
 * AFTER compute_view).  `gl_ndc` = homogeneous [-1,1] depth backend (GL). */
JCE_API void jce_gi_probes_update(JceGiProbes *gi,
                                  uint16_t compute_view, uint16_t blit_view,
                                  uint16_t color_tex, uint16_t depth_tex,
                                  const float prev_vp[16], jce_vec3 cam_pos,
                                  uint32_t vp_w, uint32_t vp_h, bool gl_ndc,
                                  /* GI L2: scene ambient for the per-probe
                                   * SKY-VISIBILITY floor (each probe tracks
                                   * how much sky its screen samples see and
                                   * scales this ambient by that openness —
                                   * covered probes go darker than open
                                   * ones).  amount <= 0 disables. */
                                  jce_vec3 sky_color, float sky_amount,
                                  /* GI L3: sun-bounce injection.  Each probe
                                   * samples this CSM cascade at its own
                                   * position; SUNLIT probes gain an upward
                                   * ground-bounce lobe (sun x ground albedo),
                                   * shadowed probes none — warm fill that
                                   * follows real sun occlusion.  csm_tex ==
                                   * UINT16_MAX or amount <= 0 disables. */
                                  uint16_t sun_csm_tex,
                                  /* The cascade's REAL square size + bgfx depth
                                   * format — the private copy must match them
                                   * exactly (bgfx_blit neither clamps nor
                                   * converts; a mismatched copy removes the
                                   * D3D12 device with INVALID_CALL).  0 size
                                   * disables the term. */
                                  uint16_t sun_csm_size,
                                  uint32_t sun_csm_fmt,
                                  const float *sun_csm_vp /* 16 or NULL */,
                                  jce_vec3 sun_dir, jce_vec3 sun_color,
                                  float sun_amount);

/* Trilinear-sample the CPU-side probe grid at a world position into 9 RGB
 * SH coefficients (lightmapper convention — feed u_sh9 directly).  Returns
 * the blend weight [0..1]; < ~0.05 means "no data yet, don't apply". */
JCE_API float jce_gi_probes_sample_sh9(const JceGiProbes *gi, jce_vec3 pos,
                                       float out_sh9[9][3]);

JCE_EXTERN_C_END

#endif /* JCE_GI_PROBES_H */
