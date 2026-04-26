/*
 * jce_image.h  CPU-side image decoding helpers.
 *
 * Thin public wrappers around image codecs that the engine's renderer
 * uses internally (currently HDR / Radiance .hdr).  Provided so that
 * downstream code (e.g. the editor's asset preview) does not have to
 * reach for a third-party header — keeping the public ABI dependency-free.
 */

#ifndef JCE_IMAGE_H
#define JCE_IMAGE_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Decode a Radiance HDR (.hdr) image from a memory buffer.
 *
 * Returns a newly-allocated float buffer of `width * height * 4` floats
 * (RGBA, linear).  The caller MUST free the buffer via
 * jce_image_free_hdr().  Returns NULL on failure (and out_w/out_h
 * remain unchanged).
 *
 * `data`/`size` describe the on-disk file bytes.  `out_w` / `out_h`
 * receive the decoded dimensions.
 */
JCE_API float *JCE_CALL jce_image_load_hdr_from_memory(const void *data, uint64_t size, int *out_w,
                                                       int *out_h);

/* Free a buffer returned by jce_image_load_hdr_from_memory(). */
JCE_API void JCE_CALL jce_image_free_hdr(float *pixels);

JCE_EXTERN_C_END

#endif /* JCE_IMAGE_H */
