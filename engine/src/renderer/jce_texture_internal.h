/*
 * jce_texture_internal.h  Texture-module internals (deliberately bgfx-free).
 *
 * Pure CPU helpers owned by jce_texture.c that something outside that one
 * translation unit must bind to the PRODUCTION definition of — today the
 * headless LUT unit test.  Kept free of bgfx/SDL includes so a test can
 * pick the declaration up without dragging the graphics backend in.
 *
 * Only for use within renderer/ and tests/; external modules use the
 * public API in <jce/renderer/jce_texture.h>.
 */

#ifndef JCE_TEXTURE_INTERNAL_H
#define JCE_TEXTURE_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reorder a horizontal LUT strip (N tiles of NxN laid left-to-right) into a
 * z-major N*N*N volume.  Tile z holds the slice where blue index == z, the
 * strip stride is N*N texels per row, and the volume layout is
 * out[(z*N + y)*N + x].  `strip` and `out` both hold N*N*N*4 RGBA8 bytes. */
void jce_lut_strip_to_volume(const uint8_t *strip, int N, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TEXTURE_INTERNAL_H */
