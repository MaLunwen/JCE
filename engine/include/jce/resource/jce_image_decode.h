/*
 * jce_image_decode.h  Decode common image formats (PNG/JPG/BMP/TGA/HDR).
 *
 * Returns RGBA8 pixel buffers with no dependency on SDL_image or any
 * other vendor library on the client side.  Internally backed by stb_image.
 *
 * Layer: OS / Resource (Layer 2).
 */

#ifndef JCE_IMAGE_DECODE_H
#define JCE_IMAGE_DECODE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceImage {
    uint8_t *pixels;   /* RGBA8, width * height * 4 bytes; may be NULL on blank */
    uint32_t width;
    uint32_t height;
} JceImage;

/* Decode an image from an in-memory buffer (PNG/JPG/BMP/TGA/HDR/...).
   On success fills `out` and returns true; the caller owns out->pixels
   and must release it with jce_image_free.  Always RGBA8. */
JCE_API bool JCE_CALL jce_image_decode(const void *data, size_t size,
                                       JceImage *out);

/* Decode an image from a host-path file.  Equivalent to read-then-decode. */
JCE_API bool JCE_CALL jce_image_decode_file(const char *path, JceImage *out);

/* Allocate a blank RGBA8 image of the given size, zero-filled.
   Returns false on OOM or invalid dimensions. */
JCE_API bool JCE_CALL jce_image_create_blank(uint32_t width, uint32_t height,
                                             JceImage *out);

/* Release an image returned by any of the above.  Safe on a zeroed JceImage. */
JCE_API void JCE_CALL jce_image_free(JceImage *img);

JCE_EXTERN_C_END

#endif /* JCE_IMAGE_DECODE_H */
