/*
 * jce_stb_image_impl.c  stb_image implementation unit.
 *
 * This file exists solely to compile the stb_image header-only library
 * into the engine.  Only HDR (Radiance .hdr) loading is needed; the
 * remaining formats are handled by SDL3_image.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR            /* compile only the HDR codec */
#define STBI_NO_STDIO            /* we load from memory, not FILE* */
#include "internal/stb_image.h"
