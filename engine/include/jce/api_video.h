/*
 * api_video.h  Video playback and capture.
 *
 * Container parsing, decode, and the encoder the in-engine recorder writes
 * through. Reachable from no umbrella until 2026-08-31.
 */

#ifndef JCE_API_VIDEO_H
#define JCE_API_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/video/jce_av1_decode.h>
#include <jce/middleware/video/jce_mp4_parser.h>
#include <jce/middleware/video/jce_video.h>
#include <jce/middleware/video/jce_vp8_decode.h>
#include <jce/middleware/video/jce_vp9_decode.h>
#include <jce/middleware/video/jce_webm_encoder.h>
#include <jce/middleware/video/jce_webm_parser.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_VIDEO_H */
