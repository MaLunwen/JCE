/* Decoder-owned color queries. No codec types escape these private adapters. */
#ifndef JCE_VIDEO_COLOR_H
#define JCE_VIDEO_COLOR_H
#include <jce/middleware/video/jce_av1_decode.h>
#include <jce/middleware/video/jce_vp8_decode.h>
#include <jce/middleware/video/jce_vp9_decode.h>
#include "jce_yuv_convert.h"
#ifdef __cplusplus
extern "C" {
#endif
JceYuvColor jce_av1_decoder_color(const JceAv1Decoder *decoder);
JceYuvColor jce_vp8_decoder_color(const JceVp8Decoder *decoder);
JceYuvColor jce_vp9_decoder_color(const JceVp9Decoder *decoder);
#ifdef __cplusplus
}
#endif
#endif
