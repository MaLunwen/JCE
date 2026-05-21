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
 *   bytes_per_pixel  4 → RGBA8 (only format supported by v2).
 *
 * Returns true on success. */
bool jce__ktx2_write_cubemap(const char    *path,
                              uint32_t       face_size,
                              uint32_t       mip_count,
                              const uint8_t *faces,
                              uint32_t       bytes_per_pixel);

JCE_EXTERN_C_END

#endif /* JCE_KTX2_WRITER_H */
