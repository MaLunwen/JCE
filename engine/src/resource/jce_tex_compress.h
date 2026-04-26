/*
 * jce_tex_compress.h  Texture mipmap generation API.
 *
 * C API for generating texture mipmaps (box filter downscaling).
 * Used by the asset cooker (jce_cook) at build time.
 */

#ifndef JCE_TEX_COMPRESS_H
#define JCE_TEX_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Mipmap generation                                                   */
/* ================================================================== */

/*
 * Calculate total mip count for a texture (including base level).
 * Returns at least 1.
 */
uint32_t jce_tex_mip_count(uint32_t width, uint32_t height);

/*
 * Calculate dimensions of a specific mip level.
 *
 * @param base_w      Base level width
 * @param base_h      Base level height
 * @param mip_level   Mip level (0 = base)
 * @param out_w       Output width for this mip
 * @param out_h       Output height for this mip
 */
void jce_tex_mip_dimensions(uint32_t base_w, uint32_t base_h,
                            uint32_t mip_level,
                            uint32_t *out_w, uint32_t *out_h);

/*
 * Generate next mip level using box filter (2x2 average).
 *
 * @param src      Source RGBA8 pixels (src_w * src_h * 4 bytes)
 * @param src_w    Source width
 * @param src_h    Source height
 * @param dst      Destination buffer (must be (src_w/2) * (src_h/2) * 4 bytes)
 * @param dst_w    Output: destination width
 * @param dst_h    Output: destination height
 *
 * Note: If src_w or src_h is 1, that dimension stays at 1.
 */
void jce_tex_generate_mip(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                          uint8_t *dst, uint32_t *dst_w, uint32_t *dst_h);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TEX_COMPRESS_H */
