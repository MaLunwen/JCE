/*
 * jce_h265_decode.h  Cross-platform H.265 (HEVC) frame decoder.
 *
 * Thin adapter over AOSP libhevc (Apache-2.0).  Accepts raw NAL units
 * (HVCC length-prefixed) and outputs RGBA8 frames.
 */

#ifndef JCE_H265_DECODE_H
#define JCE_H265_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceH265Decoder JceH265Decoder;

/* Create a decoder, initializing it with VPS/SPS/PPS NAL units extracted
 * from the hvcC box.  |hvcc| points to the raw hvcC data (DSI blob
 * from the MP4 video track), |hvcc_bytes| is its length.
 * Returns NULL on failure. */
JceH265Decoder *jce_h265_decoder_open(const void *hvcc,
                                      uint32_t hvcc_bytes);

/* Decode one access unit.  |hvcc_sample| is the raw MP4 sample in HVCC
 * format (length-prefixed NAL units).  |nal_length_size| is 1, 2, or 4
 * (from hvcC lengthSizeMinusOne + 1).
 *
 * On success, returns true and sets *out_rgba to an internal buffer
 * holding width*height*4 RGBA8 bytes (top-left origin).
 * The pointer is valid until the next decode_frame or close call.
 * *out_width / *out_height receive the decoded dimensions. */
bool jce_h265_decode_frame(JceH265Decoder *dec,
                           const void *hvcc_sample,
                           uint32_t sample_bytes,
                           uint8_t nal_length_size,
                           const uint8_t **out_rgba,
                           uint32_t *out_width,
                           uint32_t *out_height);

/* Flush the decoder (e.g. after seeking) so stale reference frames
 * are discarded. */
void jce_h265_decoder_flush(JceH265Decoder *dec);

void jce_h265_decoder_close(JceH265Decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* JCE_H265_DECODE_H */
