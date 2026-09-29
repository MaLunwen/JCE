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
#include "jce_av1_packet.h"

#include <dav1d/dav1d.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_av1"

/* ---- helpers (declared early so jce_av1_open* can reference them) ---- */

/* Silent dav1d logger: suppress "Error parsing OBU data" and similar
 * stderr spam that dav1d emits via its default printf logger. We re-emit
 * via our throttled av1_should_log_send_err(dec) path on real send failures. */
static void av1_silent_logger(void *cookie, const char *fmt, va_list ap)
{
    (void)cookie; (void)fmt; (void)ap;
}

static void av1_packet_noop_free(const uint8_t *buf, void *cookie)
{
    (void)buf;
    (void)cookie;
}

struct JceAv1Decoder {
    Dav1dContext  *ctx;
    const uint8_t *data;
    JceReadSource *source;
    uint64_t       size;
    uint64_t       pos;            /* read cursor (after IVF header) */
    Dav1dData      pending;        /* retained until send_data consumes it */
    Dav1dPicture   pic;            /* last decoded picture (must be unref'd) */
    bool           pic_valid;
    bool           ivf_eof;        /* all IVF packets sent; drain buffered frames */
    unsigned long  send_data_errors;
};

/* Per-decoder throttling avoids races between independent preview workers. */
static bool av1_should_log_send_err(JceAv1Decoder *dec)
{
    unsigned long n = ++dec->send_data_errors;
    return n <= 4u || (n & 0xFFu) == 0u;
}

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
    /* dav1d selects a CPU-sized worker pool; do not serialize frame decoding. */
    s.n_threads = 0;
    s.frame_size_limit = 4096u * 4096u;
    /* Frame-level parallelism remains enabled with automatic worker selection.
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

JceAv1Decoder *jce_av1_open_source(JceReadSource *source,
                                    JceAv1FrameInfo *out_info)
{
    uint8_t header[32];
    JceAv1Decoder *dec;
    if (jce_read_source_read_at(source,0u,header,sizeof(header)) != sizeof(header))
        return NULL;
    dec = jce_av1_open_memory(header,sizeof(header),out_info);
    if (!dec) return NULL;
    dec->source = jce_read_source_acquire(source);
    dec->data = NULL;
    dec->size = jce_read_source_size(source);
    return dec;
}

static void unref_pic(JceAv1Decoder *dec) {
    if (dec->pic_valid) {
        dav1d_picture_unref(&dec->pic);
        dec->pic_valid = false;
    }
}

/* The downstream YUV->RGBA path is 8-bit 4:2:0 only (mirrors the VP8/VP9
 * decoders).  10-bit / 4:4:4 / 4:2:2 AV1 fed to it as if it were I420 renders
 * garbage, so reject it cleanly instead (audit F75). */
static bool av1_pic_supported(const JceAv1Decoder *dec)
{
    return dec->pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 && dec->pic.p.bpc == 8;
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
            if (!av1_pic_supported(dec)) {
                LOG_WARN(LOG_TAG, "unsupported AV1 pixel format "
                         "(layout=%d bpc=%d); only 8-bit 4:2:0 supported",
                         (int)dec->pic.p.layout, (int)dec->pic.p.bpc);
                unref_pic(dec);
                return false;
            }
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

        /* EAGAIN leaves ownership with us. Never advance to a new packet
         * while the previous packet still has unconsumed bytes. */
        if (!dec->pending.sz) {
            if (dec->size - dec->pos < 12u) {
                dec->ivf_eof = true;
                continue;
            }
            uint8_t header[12];
            uint32_t fsize;
            if (dec->source) {
                if (jce_read_source_read_at(dec->source,dec->pos,header,12u) != 12u)
                    return false;
            } else memcpy(header,dec->data+(size_t)dec->pos,12u);
            fsize = rd_u32_le(header);
            dec->pos += 12u;
            if (!fsize || fsize > 16u*1024u*1024u || fsize > dec->size-dec->pos) {
                dec->ivf_eof = true;
                LOG_WARN(LOG_TAG,"invalid or truncated IVF frame");
                return false;
            }
            if (dec->source) {
                uint8_t *owned = dav1d_data_create(&dec->pending,fsize);
                if (!owned) return false;
                if (jce_read_source_read_at(dec->source,dec->pos,owned,fsize) != fsize) {
                    dav1d_data_unref(&dec->pending);
                    dec->ivf_eof = true;
                    return false;
                }
            } else if (dav1d_data_wrap(&dec->pending,dec->data+(size_t)dec->pos,
                                       fsize,av1_packet_noop_free,NULL) < 0)
                return false;
            dec->pos += fsize;
        }
        int sd = dav1d_send_data(dec->ctx, &dec->pending);
        if (sd < 0 && sd != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&dec->pending);
            LOG_WARN(LOG_TAG, "dav1d_send_data: %d", sd);
            return false;
        }
    }
}

void jce_av1_close(JceAv1Decoder *dec)
{
    if (!dec) return;
    unref_pic(dec);
    dav1d_data_unref(&dec->pending);
    if (dec->ctx) dav1d_close(&dec->ctx);
    jce_read_source_close(dec->source);
    JCE_FREE(dec);
}

static JceAv1Decoder *av1_open_packet_with_delay(int delay)
{
    JceAv1Decoder *dec = (JceAv1Decoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) return NULL;
    Dav1dSettings s;
    dav1d_default_settings(&s);
    /* dav1d selects a CPU-sized worker pool; do not serialize frame decoding. */
    s.n_threads = 0;
    s.frame_size_limit = 4096u * 4096u;
    s.max_frame_delay = delay;
    s.logger.cookie = NULL;
    s.logger.callback = av1_silent_logger;
    if (dav1d_open(&dec->ctx, &s) < 0) {
        LOG_ERROR(LOG_TAG, "dav1d_open failed");
        JCE_FREE(dec);
        return NULL;
    }
    LOG_INFO(LOG_TAG, "dav1d %s packet: threads=%d delay=%d", dav1d_version(), s.n_threads,
             dav1d_get_frame_delay(&s));
    /* No IVF buffer — caller drives via jce_av1_decode_packet(). */
    return dec;
}

JceAv1Decoder *jce_av1_open_packet(void)
{
    return av1_open_packet_with_delay(1);
}

JceAv1Decoder *jce_av1_packet_open_parallel(void)
{
    return av1_open_packet_with_delay(0);
}

int jce_av1_packet_send(JceAv1Decoder *dec, const void *packet,
                       size_t size, int64_t timestamp)
{
    if (!dec || !packet || !size) return -1;
    if (!dec->pending.sz) {
        uint8_t *owned = dav1d_data_create(&dec->pending,size);
        if (!owned) return -1;
        memcpy(owned,packet,size);
        dec->pending.m.timestamp = timestamp;
    }
    int sd = dav1d_send_data(dec->ctx, &dec->pending);
    if (sd < 0 && sd != DAV1D_ERR(EAGAIN)) {
        dav1d_data_unref(&dec->pending);
        LOG_WARN(LOG_TAG, "dav1d packet send: %d", sd);
        return -1;
    }
    return dec->pending.sz ? 0 : 1;
}

int jce_av1_packet_receive(JceAv1Decoder *dec, JceAv1PacketFrame *frame)
{
    if (!dec || !frame) return -1;
    unref_pic(dec);
    int gp = dav1d_get_picture(dec->ctx, &dec->pic);
    if (gp == DAV1D_ERR(EAGAIN)) return 0;
    if (gp < 0) return -1;
    dec->pic_valid = true;
    if (!av1_pic_supported(dec)) {
        unref_pic(dec);
        return -1;
    }
    frame->y = (const uint8_t *)dec->pic.data[0];
    frame->u = (const uint8_t *)dec->pic.data[1];
    frame->v = (const uint8_t *)dec->pic.data[2];
    frame->y_stride = dec->pic.stride[0];
    frame->uv_stride = dec->pic.stride[1];
    frame->width = (uint32_t)dec->pic.p.w;
    frame->height = (uint32_t)dec->pic.p.h;
    frame->timestamp = dec->pic.m.timestamp;
    return 1;
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
        uint8_t *owned = dav1d_data_create(&d, packet_size);
        if (!owned) return false;
        memcpy(owned, packet, packet_size);
        int sd = dav1d_send_data(dec->ctx, &d);
        if (sd < 0) {
            dav1d_data_unref(&d);
            if (av1_should_log_send_err(dec)) {
                LOG_WARN(LOG_TAG, "dav1d_send_data: %d (err #%lu)",
                         sd, dec->send_data_errors);
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
    if (!av1_pic_supported(dec)) {
        LOG_WARN(LOG_TAG, "unsupported AV1 pixel format "
                 "(layout=%d bpc=%d); only 8-bit 4:2:0 supported",
                 (int)dec->pic.p.layout, (int)dec->pic.p.bpc);
        unref_pic(dec);
        return false;
    }
    if (out_y)         *out_y         = (const uint8_t *)dec->pic.data[0];
    if (out_u)         *out_u         = (const uint8_t *)dec->pic.data[1];
    if (out_v)         *out_v         = (const uint8_t *)dec->pic.data[2];
    if (out_y_stride)  *out_y_stride  = dec->pic.stride[0];
    if (out_uv_stride) *out_uv_stride = dec->pic.stride[1];
    if (out_width)     *out_width     = (uint32_t)dec->pic.p.w;
    if (out_height)    *out_height    = (uint32_t)dec->pic.p.h;
    return true;
}
