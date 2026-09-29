/*
 * jce_h264_decode.h  Cross-platform H.264 (AVC) frame decoder.
 *
 * Thin adapter over OpenH264.  Accepts raw NAL units (Annex B or
 * AVCC via the avcC-to-Annex-B converter) and outputs RGBA8 frames.
 */

#ifndef JCE_H264_DECODE_H
#define JCE_H264_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceH264Decoder JceH264Decoder;

/* Create a decoder, initializing it with SPS/PPS NAL units extracted
 * from the avcC box.  |avcc| points to the raw avcC data (DSI blob
 * from the MP4 video track), |avcc_bytes| is its length.
 * Returns NULL on failure. */
JceH264Decoder *jce_h264_decoder_open(const void *avcc,
                                      uint32_t avcc_bytes);

/* Decode one access unit.  |avcc_sample| is the raw MP4 sample in AVCC
 * format (length-prefixed NAL units).  |nal_length_size| is 1, 2, or 4
 * (from avcC lengthSizeMinusOne + 1).
 *
 * On success, returns true and sets *out_rgba to an internal buffer
 * holding width*height*4 RGBA8 bytes (top-left origin).
 * The pointer is valid until the next decode_frame or close call.
 * *out_width / *out_height receive the decoded dimensions. */
bool jce_h264_decode_frame(JceH264Decoder *dec,
                           const void *avcc_sample,
                           uint32_t sample_bytes,
                           uint8_t nal_length_size,
                           const uint8_t **out_rgba,
                           uint32_t *out_width,
                           uint32_t *out_height);

/* Return a buffered frame from the previous decode call's DPB drain.
 * When B-frame reordering causes two frames to be produced from a
 * single decode, the second is stored internally.  Returns true if
 * a pending frame was available, false otherwise. */
bool jce_h264_decoder_drain_pending(JceH264Decoder *dec,
                                     const uint8_t **out_rgba,
                                     uint32_t *out_width,
                                     uint32_t *out_height);

/* Container PTS is carried through the codec's actual output reorder. */
bool jce_h264_decoder_drain(JceH264Decoder *dec, const uint8_t **out_rgba,
                             uint32_t *out_width, uint32_t *out_height);
void jce_h264_decoder_set_timestamp(JceH264Decoder *dec, uint64_t timestamp);
uint64_t jce_h264_decoder_frame_timestamp(const JceH264Decoder *dec);

/* Flush the decoder (e.g. after seeking) so stale reference frames
 * are discarded. */
void jce_h264_decoder_flush(JceH264Decoder *dec);

void jce_h264_decoder_close(JceH264Decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* JCE_H264_DECODE_H */
