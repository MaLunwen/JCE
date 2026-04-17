/*
 * jce_h264_decode.c  OpenH264-backed H.264 decoder.
 *
 * - Parses the avcC box to extract SPS/PPS NAL units.
 * - Converts AVCC-formatted samples to Annex B on the fly.
 * - Feeds Annex B NALs to OpenH264 ISVCDecoder.
 * - Converts I420 output to packed RGBA8.
 */

#include "jce_h264_decode.h"
#include "core/jce_memory.h"
#include <jce/core/jce_log.h>

#include <wels/codec_api.h>
#include <wels/codec_def.h>
#include <string.h>

#define LOG_TAG "jce_h264"

/* ── YUV420P → RGBA8 ─────────────────────────────────────────────── */

static void yuv420_to_rgba(const uint8_t *y_plane, int y_stride,
                           const uint8_t *u_plane, int u_stride,
                           const uint8_t *v_plane, int v_stride,
                           uint8_t *rgba, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *yp = y_plane + row * y_stride;
        const uint8_t *up = u_plane + (row / 2) * u_stride;
        const uint8_t *vp = v_plane + (row / 2) * v_stride;
        uint8_t *dst = rgba + row * width * 4;

        for (uint32_t col = 0; col < width; ++col) {
            int y = (int)yp[col] - 16;
            int u = (int)up[col / 2] - 128;
            int v = (int)vp[col / 2] - 128;

            /* BT.601 fixed-point: Y'=Y*298, Cb/Cr scaled accordingly. */
            int c = y * 298;
            int r = (c + 409 * v + 128) >> 8;
            int g = (c - 100 * u - 208 * v + 128) >> 8;
            int b = (c + 516 * u + 128) >> 8;

            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;

            dst[col * 4 + 0] = (uint8_t)r;
            dst[col * 4 + 1] = (uint8_t)g;
            dst[col * 4 + 2] = (uint8_t)b;
            dst[col * 4 + 3] = 255;
        }
    }
}

/* ── avcC → Annex B helper ────────────────────────────────────────── */

static const uint8_t k_start_code[4] = {0, 0, 0, 1};

/* Read a big-endian length of |len_size| bytes from |p|. */
static uint32_t read_be(const uint8_t *p, uint8_t len_size)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < len_size; ++i)
        v = (v << 8) | p[i];
    return v;
}

/* Convert AVCC-formatted sample data to Annex B (start-code prefixed).
 * Writes into |dst| which must be at least |src_bytes + overhead| bytes.
 * Returns total bytes written, or 0 on failure. */
static uint32_t avcc_to_annexb(const uint8_t *src, uint32_t src_bytes,
                               uint8_t nal_length_size,
                               uint8_t *dst, uint32_t dst_cap)
{
    uint32_t pos = 0;
    uint32_t out = 0;

    if (!src || !dst || nal_length_size < 1 || nal_length_size > 4)
        return 0;

    while (pos + nal_length_size <= src_bytes) {
        uint32_t nal_len = read_be(src + pos, nal_length_size);
        pos += nal_length_size;

        if (nal_len == 0 || pos + nal_len > src_bytes)
            break;

        if (out + 4 + nal_len > dst_cap)
            return 0; /* overflow */

        memcpy(dst + out, k_start_code, 4);
        out += 4;
        memcpy(dst + out, src + pos, nal_len);
        out += nal_len;
        pos += nal_len;
    }
    return out;
}

/* ── Decoder struct ───────────────────────────────────────────────── */

struct JceH264Decoder {
    ISVCDecoder *svc;

    uint8_t  *annexb_buf;     /* reusable conversion buffer */
    uint32_t  annexb_cap;

    uint8_t  *rgba_buf;       /* output RGBA frame */
    uint32_t  rgba_cap;
    uint32_t  last_w;
    uint32_t  last_h;

    /* SPS/PPS in Annex B form (fed to decoder at init and after flush). */
    uint8_t  *param_sets;
    uint32_t  param_sets_len;

    uint32_t  decode_calls;   /* diagnostic counter */

    /* Pending frame from two-step DecodeFrame2 DPB drain.
     * When both feed and drain steps produce output, the second
     * frame is buffered here for the caller to collect. */
    uint8_t  *pending_rgba;
    uint32_t  pending_cap;
    uint32_t  pending_w;
    uint32_t  pending_h;
    bool      has_pending;
};

/* Parse avcC blob, extract SPS/PPS NAL units as Annex B.
 * Returns malloc'd buffer + length, or NULL on failure. */
static uint8_t *parse_avcc_params(const uint8_t *avcc, uint32_t avcc_bytes,
                                  uint32_t *out_len)
{
    uint32_t cap, pos, out;
    uint8_t *buf;
    uint8_t num_sps, num_pps;
    int i;

    *out_len = 0;
    if (!avcc || avcc_bytes < 7) return NULL;

    /* avcc[0] = configurationVersion (must be 1)
     * avcc[5] bits[4:0] = numSPS
     * then SPS entries, then numPPS, then PPS entries. */
    if (avcc[0] != 1) return NULL;

    num_sps = avcc[5] & 0x1Fu;
    pos = 6;

    /* Estimate upper bound for output buffer. */
    cap = avcc_bytes + (uint32_t)(num_sps + 32) * 4;
    buf = (uint8_t *)JCE_MALLOC(cap);
    if (!buf) return NULL;
    out = 0;

    /* SPS entries. */
    for (i = 0; i < (int)num_sps; ++i) {
        uint16_t nal_len;
        if (pos + 2 > avcc_bytes) { JCE_FREE(buf); return NULL; }
        nal_len = (uint16_t)((avcc[pos] << 8) | avcc[pos + 1]);
        pos += 2;
        if (pos + nal_len > avcc_bytes) { JCE_FREE(buf); return NULL; }
        if (out + 4 + nal_len > cap) {
            cap = (out + 4 + nal_len) * 2;
            uint8_t *tmp = (uint8_t *)JCE_REALLOC(buf, cap);
            if (!tmp) { JCE_FREE(buf); return NULL; }
            buf = tmp;
        }
        memcpy(buf + out, k_start_code, 4); out += 4;
        memcpy(buf + out, avcc + pos, nal_len); out += nal_len;
        pos += nal_len;
    }

    /* PPS count. */
    if (pos >= avcc_bytes) { JCE_FREE(buf); return NULL; }
    num_pps = avcc[pos]; pos++;

    for (i = 0; i < (int)num_pps; ++i) {
        uint16_t nal_len;
        if (pos + 2 > avcc_bytes) { JCE_FREE(buf); return NULL; }
        nal_len = (uint16_t)((avcc[pos] << 8) | avcc[pos + 1]);
        pos += 2;
        if (pos + nal_len > avcc_bytes) { JCE_FREE(buf); return NULL; }
        if (out + 4 + nal_len > cap) {
            cap = (out + 4 + nal_len) * 2;
            uint8_t *tmp = (uint8_t *)JCE_REALLOC(buf, cap);
            if (!tmp) { JCE_FREE(buf); return NULL; }
            buf = tmp;
        }
        memcpy(buf + out, k_start_code, 4); out += 4;
        memcpy(buf + out, avcc + pos, nal_len); out += nal_len;
        pos += nal_len;
    }

    *out_len = out;
    return buf;
}

/* ── Public API ───────────────────────────────────────────────────── */

JceH264Decoder *jce_h264_decoder_open(const void *avcc,
                                      uint32_t avcc_bytes)
{
    JceH264Decoder *dec;
    SDecodingParam param;

    if (!avcc || avcc_bytes < 7) return NULL;

    dec = (JceH264Decoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) return NULL;

    if (WelsCreateDecoder(&dec->svc) != 0 || !dec->svc) {
        JCE_FREE(dec);
        return NULL;
    }

    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_FRAME_COPY;

    if ((*dec->svc)->Initialize(dec->svc, &param) != 0) {
        WelsDestroyDecoder(dec->svc);
        JCE_FREE(dec);
        return NULL;
    }

    /* Suppress verbose B-frame warnings from OpenH264's internal logging.
     * B-Slice reference-loss warnings are expected for streams with B-frames
     * and do not prevent successful decode of I/P frames. */
    {
        int log_level = WELS_LOG_QUIET;
        (*dec->svc)->SetOption(dec->svc, DECODER_OPTION_TRACE_LEVEL, &log_level);
    }

    /* Parse avcC → Annex B parameter sets and feed to decoder.
     * Use DecodeFrameNoDelay for SPS/PPS so they are synchronously
     * processed (DecodeFrame2's pipeline delay would leave them
     * "pending" and cause dsNoParamSets on the next frame). */
    dec->param_sets = parse_avcc_params((const uint8_t *)avcc,
                                        avcc_bytes, &dec->param_sets_len);
    if (dec->param_sets && dec->param_sets_len > 0) {
        SBufferInfo info;
        uint8_t *yuv[3] = {0};
        DECODING_STATE ds;
        memset(&info, 0, sizeof(info));
        ds = (*dec->svc)->DecodeFrameNoDelay(dec->svc,
                                             dec->param_sets,
                                             (int)dec->param_sets_len,
                                             yuv, &info);
        LOG_INFO(LOG_TAG, "SPS/PPS feed: %u bytes, ds=0x%X, bufStatus=%d",
                 dec->param_sets_len, (unsigned)ds, info.iBufferStatus);
    } else {
        LOG_WARN(LOG_TAG, "parse_avcc_params failed: avcc_bytes=%u", avcc_bytes);
    }

    return dec;
}

bool jce_h264_decode_frame(JceH264Decoder *dec,
                           const void *avcc_sample,
                           uint32_t sample_bytes,
                           uint8_t nal_length_size,
                           const uint8_t **out_rgba,
                           uint32_t *out_width,
                           uint32_t *out_height)
{
    uint32_t annexb_need;
    uint32_t annexb_len;
    SBufferInfo buf_info;
    uint8_t *yuv[3] = {0};
    DECODING_STATE ds;

    if (!dec || !dec->svc || !avcc_sample || sample_bytes == 0)
        return false;

    dec->decode_calls++;

    /* Ensure Annex B buffer is large enough.
     * Worst case: each NAL adds 4 start-code bytes. */
    annexb_need = sample_bytes + (sample_bytes / nal_length_size + 1) * 4;
    if (annexb_need > dec->annexb_cap) {
        JCE_FREE(dec->annexb_buf);
        dec->annexb_cap = annexb_need;
        dec->annexb_buf = (uint8_t *)JCE_MALLOC(dec->annexb_cap);
        if (!dec->annexb_buf) { dec->annexb_cap = 0; return false; }
    }

    annexb_len = avcc_to_annexb((const uint8_t *)avcc_sample, sample_bytes,
                                nal_length_size,
                                dec->annexb_buf, dec->annexb_cap);
    if (annexb_len == 0) {
        LOG_WARN(LOG_TAG, "avcc_to_annexb failed: sample_bytes=%u nal_len_size=%u",
                 sample_bytes, (unsigned)nal_length_size);
        return false;
    }

    /* Log first few decode calls for diagnostics. */
    if (dec->decode_calls <= 5) {
        const uint8_t *p = dec->annexb_buf;
        LOG_INFO(LOG_TAG, "decode[%u]: avcc=%u annexb=%u first8=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                 dec->decode_calls, sample_bytes, annexb_len,
                 annexb_len > 0 ? p[0] : 0, annexb_len > 1 ? p[1] : 0,
                 annexb_len > 2 ? p[2] : 0, annexb_len > 3 ? p[3] : 0,
                 annexb_len > 4 ? p[4] : 0, annexb_len > 5 ? p[5] : 0,
                 annexb_len > 6 ? p[6] : 0, annexb_len > 7 ? p[7] : 0);
    }

    /* Two-step DecodeFrame2 for correct B-frame DPB management.
     *
     * DecodeFrameNoDelay internally calls DecodeFrame2(data) followed by
     * DecodeFrame2(NULL,0), but SKIPS the drain step when the feed step
     * already produces output.  For B-frame streams this causes frames
     * to accumulate in the DPB, leading to reference corruption.
     *
     * Fix: always execute both steps and buffer the extra frame. */

    /* Step 1: Feed bitstream into decoder pipeline. */
    memset(&buf_info, 0, sizeof(buf_info));
    ds = (*dec->svc)->DecodeFrame2(dec->svc,
                                    dec->annexb_buf,
                                    (int)annexb_len,
                                    yuv, &buf_info);

    if (dec->decode_calls <= 5) {
        LOG_INFO(LOG_TAG, "decode[%u]: feed ds=0x%X bufStatus=%d",
                 dec->decode_calls, (unsigned)ds, buf_info.iBufferStatus);
    }

    if (ds & dsNoParamSets) {
        LOG_WARN(LOG_TAG, "DecodeFrame2: no SPS/PPS (ds=0x%X call=%u)",
                 (unsigned)ds, dec->decode_calls);
        return false;
    }

    /* Capture feed-step frame immediately (drain may invalidate YUV ptrs). */
    bool have_frame = false;
    if (buf_info.iBufferStatus == 1 && yuv[0] && yuv[1] && yuv[2]) {
        uint32_t fw = (uint32_t)buf_info.UsrData.sSystemBuffer.iWidth;
        uint32_t fh = (uint32_t)buf_info.UsrData.sSystemBuffer.iHeight;
        if (fw > 0 && fh > 0) {
            uint32_t need = fw * fh * 4;
            if (need > dec->rgba_cap) {
                JCE_FREE(dec->rgba_buf);
                dec->rgba_cap = need;
                dec->rgba_buf = (uint8_t *)JCE_MALLOC(need);
                if (!dec->rgba_buf) { dec->rgba_cap = 0; return false; }
            }
            yuv420_to_rgba(yuv[0], buf_info.UsrData.sSystemBuffer.iStride[0],
                           yuv[1], buf_info.UsrData.sSystemBuffer.iStride[1],
                           yuv[2], buf_info.UsrData.sSystemBuffer.iStride[1],
                           dec->rgba_buf, fw, fh);
            dec->last_w = fw;
            dec->last_h = fh;
            have_frame = true;
        }
    }

    /* Step 2: Always drain DPB to prevent frame accumulation. */
    {
        SBufferInfo drain_info;
        uint8_t *drain_yuv[3] = {0};
        memset(&drain_info, 0, sizeof(drain_info));
        (*dec->svc)->DecodeFrame2(dec->svc, NULL, 0, drain_yuv, &drain_info);

        if (drain_info.iBufferStatus == 1
            && drain_yuv[0] && drain_yuv[1] && drain_yuv[2]) {
            uint32_t dw = (uint32_t)drain_info.UsrData.sSystemBuffer.iWidth;
            uint32_t dh = (uint32_t)drain_info.UsrData.sSystemBuffer.iHeight;
            if (dw > 0 && dh > 0) {
                if (have_frame) {
                    /* Both steps produced output — buffer drain as pending. */
                    uint32_t pn = dw * dh * 4;
                    if (pn > dec->pending_cap) {
                        JCE_FREE(dec->pending_rgba);
                        dec->pending_cap = pn;
                        dec->pending_rgba = (uint8_t *)JCE_MALLOC(pn);
                    }
                    if (dec->pending_rgba) {
                        yuv420_to_rgba(
                            drain_yuv[0], drain_info.UsrData.sSystemBuffer.iStride[0],
                            drain_yuv[1], drain_info.UsrData.sSystemBuffer.iStride[1],
                            drain_yuv[2], drain_info.UsrData.sSystemBuffer.iStride[1],
                            dec->pending_rgba, dw, dh);
                        dec->pending_w = dw;
                        dec->pending_h = dh;
                        dec->has_pending = true;
                    }
                } else {
                    /* Only drain produced output — use as primary result. */
                    uint32_t need = dw * dh * 4;
                    if (need > dec->rgba_cap) {
                        JCE_FREE(dec->rgba_buf);
                        dec->rgba_cap = need;
                        dec->rgba_buf = (uint8_t *)JCE_MALLOC(need);
                        if (!dec->rgba_buf) { dec->rgba_cap = 0; return false; }
                    }
                    yuv420_to_rgba(
                        drain_yuv[0], drain_info.UsrData.sSystemBuffer.iStride[0],
                        drain_yuv[1], drain_info.UsrData.sSystemBuffer.iStride[1],
                        drain_yuv[2], drain_info.UsrData.sSystemBuffer.iStride[1],
                        dec->rgba_buf, dw, dh);
                    dec->last_w = dw;
                    dec->last_h = dh;
                    have_frame = true;
                }
            }
        }

        if (dec->decode_calls <= 5) {
            LOG_INFO(LOG_TAG, "decode[%u]: drain bufStatus=%d pending=%d",
                     dec->decode_calls, drain_info.iBufferStatus,
                     (int)dec->has_pending);
        }
    }

    if (!have_frame) return false;

    if (out_rgba)   *out_rgba   = dec->rgba_buf;
    if (out_width)  *out_width  = dec->last_w;
    if (out_height) *out_height = dec->last_h;
    return true;
}

bool jce_h264_decoder_drain_pending(JceH264Decoder *dec,
                                     const uint8_t **out_rgba,
                                     uint32_t *out_width,
                                     uint32_t *out_height)
{
    if (!dec || !dec->has_pending) return false;

    dec->has_pending = false;
    if (out_rgba)   *out_rgba   = dec->pending_rgba;
    if (out_width)  *out_width  = dec->pending_w;
    if (out_height) *out_height = dec->pending_h;
    return true;
}

void jce_h264_decoder_flush(JceH264Decoder *dec)
{
    if (!dec || !dec->svc) return;

    dec->has_pending = false;

    /* Uninit and re-init to flush all internal state. */
    (*dec->svc)->Uninitialize(dec->svc);

    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_FRAME_COPY;
    (*dec->svc)->Initialize(dec->svc, &param);

    /* Suppress verbose B-frame warnings after re-init. */
    {
        int log_level = WELS_LOG_QUIET;
        (*dec->svc)->SetOption(dec->svc, DECODER_OPTION_TRACE_LEVEL, &log_level);
    }

    /* Re-feed SPS/PPS (synchronous via DecodeFrameNoDelay). */
    if (dec->param_sets && dec->param_sets_len > 0) {
        SBufferInfo info;
        uint8_t *yuv[3] = {0};
        memset(&info, 0, sizeof(info));
        (*dec->svc)->DecodeFrameNoDelay(dec->svc,
                                        dec->param_sets,
                                        (int)dec->param_sets_len,
                                        yuv, &info);
    }
}

void jce_h264_decoder_close(JceH264Decoder *dec)
{
    if (!dec) return;
    if (dec->svc) {
        (*dec->svc)->Uninitialize(dec->svc);
        WelsDestroyDecoder(dec->svc);
    }
    JCE_FREE(dec->param_sets);
    JCE_FREE(dec->annexb_buf);
    JCE_FREE(dec->rgba_buf);
    JCE_FREE(dec->pending_rgba);
    JCE_FREE(dec);
}
