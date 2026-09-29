/*
 * jce_ktx2_writer.h  Private C bridge to bgfx's bundled `bimg` image
 * writer. Used by the reflection probe bake worker to persist baked
 * cubemaps without exposing C++ to the rest of the engine.
 *
 * NOTE: bimg only ships a KTX1 writer (see `bimg::imageWriteKtx`); the
 * on-disk container is therefore the legacy KTX1 format. `path` should
 * use the `.ktx` extension — KTX1 is parseable by bgfx and the vast
 * majority of texture tooling (RenderDoc, KTXSoftware, NVIDIA Texture
 * Tools, etc.). A future upgrade to KTX2 / supercompression would land
 * here without touching callers.
 *
 * NOT in the public API (`engine/include/jce/...`). Renderer-internal.
 */
#ifndef JCE_KTX2_WRITER_H
#define JCE_KTX2_WRITER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Write a 6-face cubemap to `path`.
 *
 *   path             output file (parent directory is created on demand)
 *   face_size        edge length in pixels (square faces)
 *   mip_count        mip levels packed in `faces` (>= 1, top first)
 *   faces            contiguous source bytes — cubemap face order
 *                    +X,-X,+Y,-Y,+Z,-Z. With mip_count > 1 the per-face
 *                    mip chain follows immediately after each face.
 *   bytes_per_pixel  4 → RGBA8, 8 → RGBA16F.  The second exists because an
 *                    8-bit container clamps every value above 1.0, and the
 *                    values above 1.0 are what a reflection probe is for.
 *
 * Returns true on success. */
bool jce__ktx2_write_cubemap(const char    *path,
                              uint32_t       face_size,
                              uint32_t       mip_count,
                              const uint8_t *faces,
                              uint32_t       bytes_per_pixel);

/* Load a cubemap from a host-path KTX/DDS file into a bgfx cube texture.
 *
 *   path  host-path file (read via jce_fs_host_read_all)
 *
 * Returns the bgfx texture handle index (0..UINT16_MAX-1) on success, or
 * UINT16_MAX (== bgfx invalid) on any failure (missing file, parse error,
 * not a cubemap, GPU upload failure). The returned texture is owned by the
 * caller and must be destroyed via the renderer's texture path. Used by the
 * scene renderer to consume baked reflection-probe cubemaps. */
/* `out_mips`, when non-NULL, receives the mip count the container declares --
 * the number a sampler needs to pick a roughness LOD.  Pass NULL when only
 * the handle matters (the single-mip irradiance sidecar). */
uint16_t jce__ktx_load_cubemap(const char *path, uint32_t *out_mips);

/* Parse a KTX1 RGBA8 cubemap from memory into the layout bgfx wants.
 *
 * KTX1 stores MIP-MAJOR (each mip: an imageSize, then its 6 faces);
 * bgfx_create_texture_cube wants SIDE-MAJOR (each face followed by that
 * face's whole mip chain).  The two are transposes and confusing them
 * produces a cube that parses, uploads and renders -- with the rough mips
 * showing other faces' pixels.
 *
 * On success *out_data is a jce_malloc'd side-major buffer the caller frees,
 * and *out_size is its length.  Returns false and touches nothing else on a
 * malformed or unsupported container. */
/* `out_bpp` receives 4 (RGBA8) or 8 (RGBA16F), read from the container's own
 * glType/glInternalFormat rather than assumed. */
bool jce__ktx_parse_cubemap(const void *bytes, size_t len,
                            uint32_t *out_face_size, uint32_t *out_mips,
                            uint8_t **out_data, size_t *out_size,
                            uint32_t *out_bpp);

JCE_EXTERN_C_END

#endif /* JCE_KTX2_WRITER_H */
