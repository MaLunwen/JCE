/*
 * miniaudio_impl.c  Compilation unit for miniaudio.
 *
 * miniaudio is a single-file audio library by David Reid.
 * License: Unlicense / MIT-0
 * https://github.com/mackron/miniaudio
 *
 * stb_vorbis (header-only portion) must be included BEFORE miniaudio.h
 * so that STB_VORBIS_INCLUDE_STB_VORBIS_H is defined, enabling
 * miniaudio's built-in Vorbis decoder (MA_HAS_VORBIS).
 * The implementation portion is compiled after miniaudio.
 */

#ifndef JCE_NO_AUDIO

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4245 4456 4457 4701 4100 4189 4242 4267 4996)
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

/* Phase 1: stb_vorbis header declarations only.
   This defines STB_VORBIS_INCLUDE_STB_VORBIS_H which miniaudio checks
   to enable MA_HAS_VORBIS. */
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

/* Phase 2: miniaudio implementation (sees STB_VORBIS_INCLUDE_STB_VORBIS_H). */
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

/* Phase 3: stb_vorbis implementation. */
#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* JCE_NO_AUDIO */
