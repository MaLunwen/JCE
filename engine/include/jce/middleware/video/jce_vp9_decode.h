/*
 * jce_vp9_decode.h  Decode VP9 video to YUV420 frames using libvpx.
 *
 * VP9 is a royalty-free successor to VP8 (Google / Alliance for Open
 * Media). Containers: IVF (fourcc 'VP90'), WebM/Matroska, and ISOBMFF
 * (vp09 sample entry / vpcC config box). This module mirrors the VP8
 * surface (jce_vp8_decode.h):
 *   - jce_vp9_open_ivf_memory()  : self-contained IVF reader
 *   - jce_vp9_decoder_open() + jce_vp9_decode_packet() : per-packet
 *     feed for callers that demux WebM/MKV/MP4 themselves.
 */

#ifndef JCE_VP9_DECODE_H
#define JCE_VP9_DECODE_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceVp9Decoder JceVp9Decoder;

typedef struct JceVp9FrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;       /* IVF timebase numerator (0 if unknown)  */
    uint32_t fps_den;       /* IVF timebase denominator (0 if unknown) */
} JceVp9FrameInfo;

/* Quick magic-byte check for an IVF/VP9 stream ('DKIF' + codec='VP90'). */
JCE_API bool jce_vp9_is_ivf(const void *data, size_t size);

/* Open an in-memory IVF/VP9 stream. The buffer must outlive the decoder. */
JceVp9Decoder *jce_vp9_open_ivf_memory(const void *data, size_t size,
                                       JceVp9FrameInfo *out_info);

/* Open a raw decoder (no container) — caller feeds VP9 packets via
 * jce_vp9_decode_packet(). Use this when demuxing WebM / MKV / MP4. */
JCE_API JceVp9Decoder *jce_vp9_decoder_open(void);

/* Decode the next IVF frame. Plane pointers point into libvpx-owned memory
 * and are valid until the next call. Returns false at EOF or on error.
 * Only valid when opened via jce_vp9_open_ivf_memory(). */
bool jce_vp9_decode_next(JceVp9Decoder *dec,
                         const uint8_t **out_y, ptrdiff_t *out_y_stride,
                         const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                         const uint8_t **out_v,
                         uint32_t *out_width, uint32_t *out_height);

/* Decode one VP9 packet. Same output semantics as jce_vp9_decode_next()
 * but driven by the caller (e.g. a WebM/MP4 demuxer). VP9 in MP4 stores
 * a single VP9 frame per sample as a raw bitstream (no NAL framing).
 * Returns false on error. */
bool jce_vp9_decode_packet(JceVp9Decoder *dec,
                           const void *packet, size_t packet_size,
                           const uint8_t **out_y, ptrdiff_t *out_y_stride,
                           const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                           const uint8_t **out_v,
                           uint32_t *out_width, uint32_t *out_height);

JCE_API void jce_vp9_close(JceVp9Decoder *dec);

JCE_EXTERN_C_END

#endif /* JCE_VP9_DECODE_H */
