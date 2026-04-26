/*
 * jce_av1_decode.c  Decode AV1 (IVF container) to YUV420 frames using dav1d.
 *
 * Royalty-free codec from Alliance for Open Media.
 *
 * IVF stream layout:
 *   File header (32 bytes):
 *     0  : 'DKIF'
 *     4  : u16  version (0)
 *     6  : u16  header length (32)
 *     8  : u32  fourcc (e.g. 'AV01')
 *     12 : u16  width
 *     14 : u16  height
 *     16 : u32  framerate numerator
 *     20 : u32  framerate denominator
 *     24 : u32  frame count
 *     28 : u32  reserved
 *   Per frame:
 *     0  : u32  size
 *     4  : u64  pts
 *     12 : payload
 */

#include <jce/middleware/video/jce_av1_decode.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <dav1d/dav1d.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_av1"

/* ---- helpers (declared early so jce_av1_open* can reference them) ---- */

/* Silent dav1d logger: suppress "Error parsing OBU data" and similar
 * stderr spam that dav1d emits via its default printf logger. We re-emit
 * via our throttled av1_should_log_send_err() path on real send failures. */
static void av1_silent_logger(void *cookie, const char *fmt, va_list ap)
{
    (void)cookie; (void)fmt; (void)ap;
}

/* Shared throttle counter for dav1d_send_data() warnings. Corrupt streams
 * can hit this every frame; we log first 4, then 1 per 256, with running
 * total so the user still knows the error is recurring. */
static unsigned long s_send_data_err_count = 0;
static bool av1_should_log_send_err(void)
{
    unsigned long n = ++s_send_data_err_count;
    return (n <= 4u) || ((n & 0xFFu) == 0u);
}

static void av1_packet_noop_free(const uint8_t *buf, void *cookie)
{
    (void)buf;
    (void)cookie;
}

struct JceAv1Decoder {
    Dav1dContext  *ctx;
    const uint8_t *data;
    size_t         size;
    size_t         pos;            /* read cursor (after IVF header) */
    Dav1dPicture   pic;            /* last decoded picture (must be unref'd) */
    bool           pic_valid;
    bool           ivf_eof;        /* all IVF packets sent; drain buffered frames */
};

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool jce_av1_is_ivf(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || size < 32) return false;
    if (p[0] != 'D' || p[1] != 'K' || p[2] != 'I' || p[3] != 'F') return false;
    /* fourcc at offset 8: 'AV01' */
    return p[8] == 'A' && p[9] == 'V' && p[10] == '0' && p[11] == '1';
}

JceAv1Decoder *jce_av1_open_memory(const void *data, size_t size,
                                   JceAv1FrameInfo *out_info)
{
    if (!jce_av1_is_ivf(data, size)) {
        LOG_WARN(LOG_TAG, "input is not an IVF/AV1 stream");
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)data;
    JceAv1Decoder *dec = (JceAv1Decoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) return NULL;

    dec->data = p;
    dec->size = size;
    dec->pos  = 32;

    Dav1dSettings s;
    dav1d_default_settings(&s);
    /* S2: n_threads=0 (auto = logical cores, already the default).
     * max_frame_delay=0 (auto = ceil(sqrt(n_threads))) enables frame-level
     * parallelism inside dav1d, critical for 4K throughput.  Low-latency
     * single-frame mode (=1) is only needed for packet-driven paths where
     * the caller feeds one packet at a time and expects one frame back. */
    s.max_frame_delay = 0;
    s.logger.cookie = NULL;
    s.logger.callback = av1_silent_logger;
    if (dav1d_open(&dec->ctx, &s) < 0) {
        LOG_ERROR(LOG_TAG, "dav1d_open failed");
        JCE_FREE(dec);
        return NULL;
    }
    {
        int actual_delay = dav1d_get_frame_delay(&s);
        LOG_INFO(LOG_TAG, "dav1d IVF: n_threads=%d max_frame_delay=%d (actual=%d)",
                 s.n_threads, s.max_frame_delay, actual_delay);
    }
    if (out_info) {
        out_info->width   = rd_u16_le(p + 12);
        out_info->height  = rd_u16_le(p + 14);
        out_info->fps_num = rd_u32_le(p + 16);
        out_info->fps_den = rd_u32_le(p + 20);
    }
    return dec;
}

static void unref_pic(JceAv1Decoder *dec) {
    if (dec->pic_valid) {
        dav1d_picture_unref(&dec->pic);
        dec->pic_valid = false;
    }
}

bool jce_av1_decode_next(JceAv1Decoder *dec,
                         const uint8_t **out_y, ptrdiff_t *out_y_stride,
                         const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                         const uint8_t **out_v,
                         uint32_t *out_width, uint32_t *out_height)
{
    if (!dec) return false;
    unref_pic(dec);

    for (;;) {
        /* Try to pull a decoded picture first. */
        int gp = dav1d_get_picture(dec->ctx, &dec->pic);
        if (gp == 0) {
            dec->pic_valid = true;
            if (out_y)         *out_y         = (const uint8_t *)dec->pic.data[0];
            if (out_u)         *out_u         = (const uint8_t *)dec->pic.data[1];
            if (out_v)         *out_v         = (const uint8_t *)dec->pic.data[2];
            if (out_y_stride)  *out_y_stride  = dec->pic.stride[0];
            if (out_uv_stride) *out_uv_stride = dec->pic.stride[1];
            if (out_width)     *out_width     = (uint32_t)dec->pic.p.w;
            if (out_height)    *out_height    = (uint32_t)dec->pic.p.h;
            return true;
        }
        if (gp != DAV1D_ERR(EAGAIN)) {
            if (gp == DAV1D_ERR(ENOENT)) return false; /* EOF */
            LOG_WARN(LOG_TAG, "dav1d_get_picture: %d", gp);
            return false;
        }

        /* EAGAIN: dav1d needs more input.  If we've already sent all IVF
         * packets (ivf_eof), dav1d is done — no more frames will come.
         * With max_frame_delay > 1 the remaining buffered frames would have
         * been output before EAGAIN was returned, so this is true EOF. */
        if (dec->ivf_eof) return false;

        /* Feed next IVF frame. */
        if (dec->pos + 12 > dec->size) {
            dec->ivf_eof = true;
            continue; /* let dav1d drain buffered frames via get_picture */
        }
        uint32_t fsize = rd_u32_le(dec->data + dec->pos);
        dec->pos += 12;
        if (dec->pos + fsize > dec->size) {
            LOG_WARN(LOG_TAG, "truncated IVF frame");
            return false;
        }

        Dav1dData d;
        if (dav1d_data_wrap(&d, dec->data + dec->pos, fsize, NULL, NULL) < 0) {
            LOG_ERROR(LOG_TAG, "dav1d_data_wrap failed");
            return false;
        }
        dec->pos += fsize;

        int sd = dav1d_send_data(dec->ctx, &d);
        if (sd < 0 && sd != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&d);
            if (av1_should_log_send_err()) {
                LOG_WARN(LOG_TAG, "dav1d_send_data: %d (err #%lu)",
                         sd, s_send_data_err_count);
            }
            return false;
        }
        /* If EAGAIN, dav1d kept a reference; loop and try get_picture. */
    }
}

void jce_av1_close(JceAv1Decoder *dec)
{
    if (!dec) return;
    unref_pic(dec);
    if (dec->ctx) dav1d_close(&dec->ctx);
    JCE_FREE(dec);
}

JceAv1Decoder *jce_av1_open_packet(void)
{
    JceAv1Decoder *dec = (JceAv1Decoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) return NULL;
    Dav1dSettings s;
    dav1d_default_settings(&s);
    /* Packet-driven path (WebM/AV1): keep max_frame_delay=1 (low-latency).
     * Caller sends one packet and expects at most one frame back immediately.
     * Frame-level threading (S2) is not useful here since the pipeline is
     * bounded by network/demux, not decode throughput. */
    s.max_frame_delay = 1;
    s.logger.cookie = NULL;
    s.logger.callback = av1_silent_logger;
    if (dav1d_open(&dec->ctx, &s) < 0) {
        LOG_ERROR(LOG_TAG, "dav1d_open failed");
        JCE_FREE(dec);
        return NULL;
    }
    /* No IVF buffer — caller drives via jce_av1_decode_packet(). */
    return dec;
}

bool jce_av1_decode_packet(JceAv1Decoder *dec,
                           const void *packet, size_t packet_size,
                           const uint8_t **out_y, ptrdiff_t *out_y_stride,
                           const uint8_t **out_u, ptrdiff_t *out_uv_stride,
                           const uint8_t **out_v,
                           uint32_t *out_width, uint32_t *out_height)
{
    if (!dec) return false;
    unref_pic(dec);

    if (packet && packet_size > 0) {
        Dav1dData d;
        if (dav1d_data_wrap(&d, (const uint8_t *)packet, packet_size,
                            av1_packet_noop_free, NULL) < 0) {
            LOG_ERROR(LOG_TAG, "dav1d_data_wrap failed");
            return false;
        }
        int sd = dav1d_send_data(dec->ctx, &d);
        if (sd < 0 && sd != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&d);
            if (av1_should_log_send_err()) {
                LOG_WARN(LOG_TAG, "dav1d_send_data: %d (err #%lu)",
                         sd, s_send_data_err_count);
            }
            return false;
        }
    }

    int gp = dav1d_get_picture(dec->ctx, &dec->pic);
    if (gp != 0) {
        if (gp != DAV1D_ERR(EAGAIN) && gp != DAV1D_ERR(ENOENT)) {
            LOG_WARN(LOG_TAG, "dav1d_get_picture: %d", gp);
        }
        return false;
    }
    dec->pic_valid = true;
    if (out_y)         *out_y         = (const uint8_t *)dec->pic.data[0];
    if (out_u)         *out_u         = (const uint8_t *)dec->pic.data[1];
    if (out_v)         *out_v         = (const uint8_t *)dec->pic.data[2];
    if (out_y_stride)  *out_y_stride  = dec->pic.stride[0];
    if (out_uv_stride) *out_uv_stride = dec->pic.stride[1];
    if (out_width)     *out_width     = (uint32_t)dec->pic.p.w;
    if (out_height)    *out_height    = (uint32_t)dec->pic.p.h;
    return true;
}
