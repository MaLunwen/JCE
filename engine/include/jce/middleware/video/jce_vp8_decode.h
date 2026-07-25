/*
 * jce_vp8_decode.h  Decode VP8 video to YUV420 frames using libvpx.
 *
 * VP8 is a royalty-free video codec from Google / Alliance for Open Media.
 * Containers: IVF (one of the simplest — same layout as AV1's IVF, with
 * fourcc 'VP80') and WebM/Matroska. This module supports both:
 *   - jce_vp8_open_ivf_memory()  : self-contained IVF reader
 *   - jce_vp8_decoder_open() + jce_vp8_decode_packet() : per-packet feed
 *     for callers that demux WebM / MKV themselves.
 *
 * Sibling to jce_av1_decode.h. Either can serve as the runtime video path
 * without infringing AAC/H.26x patents.
 */

#ifndef JCE_VP8_DECODE_H
#define JCE_VP8_DECODE_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceVp8Decoder JceVp8Decoder;

typedef struct JceVp8FrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;       /* IVF timebase numerator (0 if unknown)  */
    uint32_t fps_den;       /* IVF timebase denominator (0 if unknown) */
} JceVp8FrameInfo;

/* Quick magic-byte check for an IVF/VP8 stream ('DKIF' + codec='VP80'). */
JCE_API bool jce_vp8_is_ivf(const void *data, size_t size);

/* Open an in-memory IVF/VP8 stream. The buffer must outlive the decoder. */
JCE_API JceVp8Decoder *jce_vp8_open_ivf_memory(const void *data, size_t size,
                                       JceVp8FrameInfo *out_info);

/* Open a raw decoder (no container) — caller feeds VP8 packets via
 * jce_vp8_decode_packet(). Use this when demuxing WebM / MKV. */
JCE_API JceVp8Decoder *jce_vp8_decoder_open(void);

/* Decode the next IVF frame. Plane pointers point into libvpx-owned memory
 * and are valid until the next call. Returns false at EOF or on error.
 * Only valid when opened via jce_vp8_open_ivf_memory(). */
JCE_API bool jce_vp8_decode_next(JceVp8Decoder *dec,
                         const uint8_t **out_y, ptrdiff_t *out_y_stride,
                         const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                         const uint8_t **out_v,
                         uint32_t *out_width, uint32_t *out_height);

/* Decode one VP8 packet. Same output semantics as jce_vp8_decode_next()
 * but driven by the caller (e.g. a WebM demuxer). Returns false on error. */
JCE_API bool jce_vp8_decode_packet(JceVp8Decoder *dec,
                           const void *packet, size_t packet_size,
                           const uint8_t **out_y, ptrdiff_t *out_y_stride,
                           const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                           const uint8_t **out_v,
                           uint32_t *out_width, uint32_t *out_height);

JCE_API void jce_vp8_close(JceVp8Decoder *dec);

JCE_EXTERN_C_END

#endif /* JCE_VP8_DECODE_H */
