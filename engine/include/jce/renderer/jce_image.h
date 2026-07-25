/*
 * jce_image.h  CPU-side image decoding service.
 *
 * The ONE place the engine decodes images.  Callers name the pixel format
 * they want out (RGBA8 / RGBA32F / gray16), never a codec: which library
 * runs behind each entry point is an implementation detail that may change
 * without touching a single call site.  Provided so that downstream code
 * (e.g. the editor's asset preview) does not have to reach for a
 * third-party header — keeping the public ABI dependency-free.
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

/* Decode a single-channel 16-bit grayscale image (e.g. a terrain heightmap
 * PNG) from a memory buffer.  8-bit sources are promoted to the full
 * 0..65535 range.  Returns a newly-allocated uint16 buffer of
 * `width * height` samples, or NULL on failure (out_w/out_h unchanged).
 * The caller MUST free it via jce_image_free_gray16().  This keeps
 * specialized numeric (non-RGBA) decodes behind the JCE image facade so
 * callers never reach for stb_image directly. */
JCE_API uint16_t *JCE_CALL jce_image_load_gray16_from_memory(const void *data, uint64_t size,
                                                             int *out_w, int *out_h);

/* Free a buffer returned by jce_image_load_gray16_from_memory(). */
JCE_API void JCE_CALL jce_image_free_gray16(uint16_t *pixels);

/* Decode an LDR image (PNG/JPG/TGA/BMP/WEBP/... — every format the engine's
 * image codec set understands) from a memory buffer.
 *
 * Returns a newly-allocated, TIGHTLY PACKED buffer of `width * height * 4`
 * bytes (RGBA8, sRGB-encoded exactly as authored — no colour conversion is
 * applied), or NULL on failure (out_w/out_h unchanged).  The buffer belongs
 * to the ENGINE allocator: release it with jce_image_free_rgba8() and never
 * with a codec's own free — mismatched allocators across a decode boundary
 * have already cost this codebase one P0.
 */
JCE_API uint8_t *JCE_CALL jce_image_load_rgba8_from_memory(const void *data, uint64_t size,
                                                            int *out_w, int *out_h);

/* Free a buffer returned by jce_image_load_rgba8_from_memory(). */
JCE_API void JCE_CALL jce_image_free_rgba8(uint8_t *pixels);

JCE_EXTERN_C_END

#endif /* JCE_IMAGE_H */
