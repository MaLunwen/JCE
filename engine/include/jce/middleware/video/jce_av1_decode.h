/*
 * jce_av1_decode.h  Decode AV1 video (IVF container) to YUV420 frames.
 *
 * AV1 is a royalty-free video codec from the Alliance for Open Media.
 * Container: IVF (one of the simplest video containers, ~32-byte header).
 * Backed by dav1d (VideoLAN, BSD-2).
 *
 * This is JCE 0.8's preferred video path (replaces H.264/H.265 for new assets).
 */

#ifndef JCE_AV1_DECODE_H
#define JCE_AV1_DECODE_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAv1Decoder JceAv1Decoder;

typedef struct JceAv1FrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;       /* timebase numerator from IVF header */
    uint32_t fps_den;       /* timebase denominator from IVF header */
} JceAv1FrameInfo;

/* Quick magic-byte check for an IVF/AV1 stream ('DKIF' + codec='AV01'). */
JCE_API bool jce_av1_is_ivf(const void *data, size_t size);

/* Open an in-memory IVF/AV1 stream. The buffer must outlive the decoder. */
JceAv1Decoder *jce_av1_open_memory(const void *data, size_t size,
                                   JceAv1FrameInfo *out_info);

/* Decode the next frame. Outputs Y/U/V plane pointers and strides
 * (these point into dav1d-owned memory and are valid until the next call).
 * Returns false at EOF or on error. */
bool jce_av1_decode_next(JceAv1Decoder *dec,
                         const uint8_t **out_y, ptrdiff_t *out_y_stride,
                         const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                         const uint8_t **out_v,
                         uint32_t *out_width, uint32_t *out_height);

/* Open a packet-driven AV1 decoder (no IVF reader). Use this when the
 * caller demuxes the AV1 bitstream itself, e.g. WebM/Matroska. */
JCE_API JceAv1Decoder *jce_av1_open_packet(void);

/* Decode one AV1 OBU/frame packet. Same output semantics as
 * jce_av1_decode_next(). Pass NULL/0 to drain. */
bool jce_av1_decode_packet(JceAv1Decoder *dec,
                           const void *packet, size_t packet_size,
                           const uint8_t **out_y, ptrdiff_t *out_y_stride,
                           const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                           const uint8_t **out_v,
                           uint32_t *out_width, uint32_t *out_height);

JCE_API void jce_av1_close(JceAv1Decoder *dec);

JCE_EXTERN_C_END

#endif /* JCE_AV1_DECODE_H */
