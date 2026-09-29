/*
 * jce_ibl_convolve.h  Cube-source IBL convolutions, for the probe bake.
 *
 * jce_ibl.c has had a correct cosine-weighted irradiance convolution and a
 * correct GGX split-sum prefilter since IBL landed.  Both are pure functions
 * of a DIRECTION; only the sampler tied them to equirectangular input.  The
 * reflection probe bake, meanwhile, declared two convolution steps that did
 * no work whatsoever -- it advanced a progress bar and then wrote the
 * .irr.ktx sidecar from the same face bytes as the specular map, so a probe's
 * "irradiance" was a copy of the radiance it was supposed to integrate, and
 * its container declared one mip so every roughness read mip 0.
 *
 * Rather than a second copy of the same integrals, the sampler became a
 * parameter.  These are the cube-source entry points that fall out of that.
 *
 * INTERNAL to engine/src/renderer -- not public API.  The probe bake is the
 * only caller and the shape of these (RGBA8 in, RGBA8 out, KTX layout) is a
 * consequence of the container the bake writes, not a contract worth
 * exporting.
 */

#ifndef JCE_IBL_CONVOLVE_H
#define JCE_IBL_CONVOLVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The (face, u, v) -> direction mapping every cubemap in this engine uses.
 *
 * Exported so there is ONE authority for the convention: the probe capture
 * derives its six camera bases from it (asserted in
 * test_jce_probe_convolution), and a cubemap convention that is mirrored or
 * rotated renders a plausible image whose reflections point the wrong way
 * with nothing to report it.  u and v are [0,1] across the face, v running
 * DOWN, which is how the samplers index rows. */
void jce_ibl_cube_direction(int face, float u, float v,
                            float *dx, float *dy, float *dz);

/* Cosine-weighted irradiance of an RGBA8 cubemap.
 *
 * `src_faces` is 6 * src_face_size^2 RGBA8 texels in +X,-X,+Y,-Y,+Z,-Z order;
 * `out_faces` receives 6 * out_face_size^2 RGBA8 texels in the same order.
 * Irradiance is smooth by construction, so out_face_size is normally much
 * smaller than the source (32 is the size the IBL path uses) -- the cost is
 * ~1024 samples per OUTPUT texel, so this is the number that decides how long
 * a bake takes. */
/* THE SAME TWO CONVOLUTIONS, in half floats.
 *
 * An RGBA8 probe clamps every value above 1.0, so a sun, a lamp or a bright
 * window in the captured scene reflects at exactly the brightness of white
 * paper -- which is the one case a reflection exists for.  These take and
 * produce RGBA16F and run the SAME integrals as the RGBA8 pair below, with a
 * different sampler in and a different pack out.
 *
 * Buffer sizes are in uint16_t elements: 4 per texel, as the RGBA8 pair uses
 * 4 bytes per texel. */
void jce_ibl_convolve_cube_irradiance_rgba16f(const uint16_t *src_faces,
                                              uint32_t src_face_size,
                                              uint32_t out_face_size,
                                              uint16_t *out_faces);
void jce_ibl_convolve_cube_specular_rgba16f(const uint16_t *src_faces,
                                            uint32_t face_size,
                                            uint16_t *out_mipchain);

void jce_ibl_convolve_cube_irradiance_rgba8(const uint8_t *src_faces,
                                            uint32_t src_face_size,
                                            uint32_t out_face_size,
                                            uint8_t *out_faces);

/* Mip count and byte size of the specular chain for a given face size --
 * ask before allocating for jce_ibl_convolve_cube_specular_rgba8. */
uint32_t jce_ibl_cube_mip_count(uint32_t face_size);
size_t   jce_ibl_cube_mipchain_bytes(uint32_t face_size);

/* GGX split-sum prefilter of an RGBA8 cubemap into a roughness mip chain.
 *
 * `out_mipchain` must hold jce_ibl_cube_mipchain_bytes(face_size) bytes and
 * receives MIP-MAJOR data -- all six faces of mip 0, then all six of mip 1 --
 * which is what bimg's imageWriteKtx reads and what KTX1 stores.  (The
 * writer's own comment used to claim face-major; it was wrong, and believing
 * it would have produced a container whose rough mips are other faces'
 * pixels.)  Mip 0 is roughness 0, i.e. the source faces resampled through the
 * same integral, so a probe's mirror reflection is unchanged and only the
 * rough mips are new. */
void jce_ibl_convolve_cube_specular_rgba8(const uint8_t *src_faces,
                                          uint32_t face_size,
                                          uint8_t *out_mipchain);

#ifdef __cplusplus
}
#endif

#endif /* JCE_IBL_CONVOLVE_H */
