/*
 * jce_vp8_decode.c  Decode VP8 (IVF or raw packets) to YUV420 using libvpx.
 *
 * Royalty-free codec from Google / Alliance for Open Media.
 * IVF stream layout matches AV1's (see jce_av1_decode.c) with fourcc 'VP80'.
 */

#include <jce/middleware/video/jce_vp8_decode.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_image.h>
#include <string.h>

#define LOG_TAG "jce_vp8"

struct JceVp8Decoder {
    vpx_codec_ctx_t  codec;
    bool             codec_init;

    /* IVF mode (optional) */
    const uint8_t   *data;
    size_t           size;
    size_t           pos;

    /* Iterator for vpx_codec_get_frame after a vpx_codec_decode call. */
    const vpx_image_t *last_img;
};

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool jce_vp8_is_ivf(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || size < 32u) return false;
    if (p[0] != 'D' || p[1] != 'K' || p[2] != 'I' || p[3] != 'F') return false;
    return p[8] == 'V' && p[9] == 'P' && p[10] == '8' && p[11] == '0';
}

static bool init_codec(JceVp8Decoder *dec)
{
    vpx_codec_dec_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.threads = 1;
    if (vpx_codec_dec_init(&dec->codec, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
        LOG_ERROR(LOG_TAG, "vpx_codec_dec_init(VP8) failed: %s",
                  vpx_codec_error(&dec->codec));
        return false;
    }
    dec->codec_init = true;
    return true;
}

JceVp8Decoder *jce_vp8_decoder_open(void)
{
    JceVp8Decoder *dec = (JceVp8Decoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) return NULL;
    if (!init_codec(dec)) { JCE_FREE(dec); return NULL; }
    return dec;
}

JceVp8Decoder *jce_vp8_open_ivf_memory(const void *data, size_t size,
                                       JceVp8FrameInfo *out_info)
{
    if (!jce_vp8_is_ivf(data, size)) {
        LOG_WARN(LOG_TAG, "input is not an IVF/VP8 stream");
        return NULL;
    }
    const uint8_t *p = (const uint8_t *)data;
    JceVp8Decoder *dec = jce_vp8_decoder_open();
    if (!dec) return NULL;

    dec->data = p;
    dec->size = size;
    dec->pos  = 32;

    if (out_info) {
        out_info->width   = rd_u16_le(p + 12);
        out_info->height  = rd_u16_le(p + 14);
        out_info->fps_num = rd_u32_le(p + 16);
        out_info->fps_den = rd_u32_le(p + 20);
    }
    return dec;
}

static bool emit_image(JceVp8Decoder *dec,
                       const uint8_t **out_y, ptrdiff_t *out_y_stride,
                       const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                       const uint8_t **out_v,
                       uint32_t *out_width, uint32_t *out_height)
{
    const vpx_image_t *img = dec->last_img;
    if (!img) return false;
    if (img->fmt != VPX_IMG_FMT_I420 && img->fmt != VPX_IMG_FMT_YV12) {
        LOG_WARN(LOG_TAG, "unsupported pixel format: 0x%x", img->fmt);
        return false;
    }
    if (out_y)         *out_y         = img->planes[VPX_PLANE_Y];
    if (out_u)         *out_u         = (img->fmt == VPX_IMG_FMT_YV12)
                                            ? img->planes[VPX_PLANE_V]
                                            : img->planes[VPX_PLANE_U];
    if (out_v)         *out_v         = (img->fmt == VPX_IMG_FMT_YV12)
                                            ? img->planes[VPX_PLANE_U]
                                            : img->planes[VPX_PLANE_V];
    if (out_y_stride)  *out_y_stride  = img->stride[VPX_PLANE_Y];
    if (out_uv_stride) *out_uv_stride = img->stride[VPX_PLANE_U];
    if (out_width)     *out_width     = img->d_w;
    if (out_height)    *out_height    = img->d_h;
    return true;
}

bool jce_vp8_decode_packet(JceVp8Decoder *dec,
                           const void *packet, size_t packet_size,
                           const uint8_t **out_y, ptrdiff_t *out_y_stride,
                           const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                           const uint8_t **out_v,
                           uint32_t *out_width, uint32_t *out_height)
{
    if (!dec || !dec->codec_init || !packet || packet_size == 0) return false;

    if (vpx_codec_decode(&dec->codec, (const uint8_t *)packet,
                         (unsigned int)packet_size, NULL, 0) != VPX_CODEC_OK) {
        LOG_WARN(LOG_TAG, "vpx_codec_decode failed: %s",
                 vpx_codec_error(&dec->codec));
        return false;
    }

    vpx_codec_iter_t iter = NULL;
    dec->last_img = vpx_codec_get_frame(&dec->codec, &iter);
    return emit_image(dec, out_y, out_y_stride,
                      out_u, out_uv_stride, out_v, out_width, out_height);
}

bool jce_vp8_decode_next(JceVp8Decoder *dec,
                         const uint8_t **out_y, ptrdiff_t *out_y_stride,
                         const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                         const uint8_t **out_v,
                         uint32_t *out_width, uint32_t *out_height)
{
    if (!dec || !dec->data) return false;
    if (dec->pos + 12u > dec->size) return false; /* clean EOF */

    uint32_t fsize = rd_u32_le(dec->data + dec->pos);
    dec->pos += 12u;
    if (dec->pos + fsize > dec->size) {
        LOG_WARN(LOG_TAG, "truncated IVF frame");
        return false;
    }
    const uint8_t *frame = dec->data + dec->pos;
    dec->pos += fsize;

    return jce_vp8_decode_packet(dec, frame, fsize,
                                 out_y, out_y_stride,
                                 out_u, out_uv_stride, out_v,
                                 out_width, out_height);
}

void jce_vp8_close(JceVp8Decoder *dec)
{
    if (!dec) return;
    if (dec->codec_init) vpx_codec_destroy(&dec->codec);
    JCE_FREE(dec);
}
