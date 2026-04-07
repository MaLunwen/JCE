/*
 * miniaudio_impl_cook.c  miniaudio implementation for the jce_cook CLI tool.
 *
 * This translation unit provides the miniaudio implementation so that
 * jce_cook can decode audio files (WAV, OGG, FLAC, MP3) during the
 * asset cooking phase.
 *
 * Separated from the engine's miniaudio_impl.c to avoid pulling in
 * engine-specific defines and platform audio backends.
 */

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

/* Phase 1: stb_vorbis header declarations only. */
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

/* Phase 2: miniaudio implementation.
   We only need decoding, not playback. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
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
