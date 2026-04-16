/*
 * pl_mpeg_impl.c  Compilation unit for pl_mpeg.
 *
 * pl_mpeg is a single-file MPEG-1 video / MP2 audio / MPEG-PS demuxer
 * by Dominic Szablewski.  License: MIT
 * https://github.com/phoboslab/pl_mpeg
 *
 * This translation unit provides the implementation; every other
 * consumer in the jce_video layer includes pl_mpeg.h header-only.
 */

#ifndef JCE_NO_VIDEO

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4245 4456 4457 4701 4100 4189 4242 4267 4996)
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif

/* Skip the stdio helpers — we always supply data from memory. */
#define PLM_NO_STDIO

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* JCE_NO_VIDEO */
