/*
 * jce_stb_image_impl.c  stb_image implementation unit.
 *
 * Compiles stb_image into the engine with all built-in codecs enabled
 * (PNG / JPEG / BMP / TGA / GIF / PSD / HDR / PIC / PNM) so that no
 * runtime image-decode dependency (e.g. SDL3_image) is required.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO            /* loads come from memory buffers */
#include "internal/stb_image.h"
