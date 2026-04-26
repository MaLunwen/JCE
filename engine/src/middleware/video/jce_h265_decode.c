/*
 * jce_h265_decode.c  AOSP libhevc-backed H.265/HEVC decoder.
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │ ⚠ PATENT NOTICE — H.265 / HEVC                              │
 * │ HEVC is covered by patents in three separate pools:         │
 * │   - MPEG-LA HEVC                                            │
 * │   - HEVC Advance / Access Advance                           │
 * │   - Velos Media                                             │
 * │ Distributing binaries that decode HEVC may require licenses │
 * │ from any/all of these pools depending on jurisdiction.      │
 * │                                                             │
 * │ The libhevc source is Apache-2.0 (no source-code royalty),  │
 * │ but the patent claims attach to the *resulting binary*,     │
 * │ not the source. Apache-2.0 explicitly does NOT grant patent │
 * │ licenses for third-party patents.                           │
 * │                                                             │
 * │ Prefer the royalty-free AV1 path (jce_av1_decode.h) for     │
 * │ NEW assets. This loader is retained ONLY to import legacy   │
 * │ HEVC content and SHOULD NOT be the default cooker output.   │
 * └─────────────────────────────────────────────────────────────┘
 *
 * - Parses the hvcC box to extract VPS/SPS/PPS NAL units.
 * - Converts HVCC-formatted samples to Annex B on the fly.
 * - Feeds Annex B NALs to the AOSP HEVC decoder (IVD API).
 * - Converts YUV420P output to packed RGBA8.
 */

#include "jce_h265_decode.h"

#ifdef JCE_ENABLE_PATENTED_CODECS

#include "jce_yuv_convert.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_log.h>

#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ihevcd_cxa.h"

#include <jce/os/core/jce_allocator.h>
#include <string.h>

#define LOG_TAG "jce_h265"

/* ── Aligned alloc callbacks for libhevc ──────────────────────────── */

static void *jce_hevc_aligned_alloc(void *pv_mem_ctxt,
                                    WORD32 alignment, WORD32 size)
{
    (void)pv_mem_ctxt;
    return jce_aligned_alloc((size_t)size, (size_t)alignment);
}

static void jce_hevc_aligned_free(void *pv_mem_ctxt, void *pv_buf)
{
    (void)pv_mem_ctxt;
    jce_aligned_free(pv_buf);
}

/* ── hvcC / HVCC → Annex B helpers ────────────────────────────────── */

static const uint8_t k_start_code[4] = {0, 0, 0, 1};

static uint32_t read_be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static uint32_t read_be(const uint8_t *p, uint8_t len_size)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < len_size; ++i)
        v = (v << 8) | p[i];
    return v;
}

/*
 * Parse hvcC box → extract VPS/SPS/PPS NAL units in Annex B form.
 *
 * hvcC layout (ISO 14496-15 §8.3.3.1.2):
 *   [0]   configurationVersion = 1
 *   [1]   general_profile_space/tier/idc...
 *   ...
 *   [21]  lengthSizeMinusOne (bits 1:0)
 *   [22]  numOfArrays
 *   For each array:
 *     [0]  NAL unit type (bits 5:0)
 *     [1..2] numNalus (big-endian 16)
 *     For each NAL:
 *       [0..1] nalUnitLength (big-endian 16)
 *       [2..N] NAL data
 */
static uint8_t *parse_hvcc_params(const uint8_t *hvcc, uint32_t hvcc_bytes,
                                  uint32_t *out_len,
                                  uint8_t *out_nal_length_size)
{
    *out_len = 0;
    if (!hvcc || hvcc_bytes < 23) return NULL;
    if (hvcc[0] != 1) return NULL;

    *out_nal_length_size = (hvcc[21] & 0x03) + 1;
    uint8_t num_arrays = hvcc[22];
    uint32_t pos = 23;

    /* Upper bound for output: hvcc_bytes + start codes overhead. */
    uint32_t cap = hvcc_bytes + (uint32_t)num_arrays * 64;
    uint8_t *buf = (uint8_t *)JCE_MALLOC(cap);
    if (!buf) return NULL;
    uint32_t out = 0;

    for (uint8_t arr = 0; arr < num_arrays; ++arr) {
        if (pos + 3 > hvcc_bytes) { JCE_FREE(buf); return NULL; }
        /* uint8_t nal_type = hvcc[pos] & 0x3F; */ /* unused */
        pos++;
        uint32_t num_nalus = read_be16(hvcc + pos); pos += 2;

        for (uint32_t n = 0; n < num_nalus; ++n) {
            if (pos + 2 > hvcc_bytes) { JCE_FREE(buf); return NULL; }
            uint32_t nal_len = read_be16(hvcc + pos); pos += 2;
            if (pos + nal_len > hvcc_bytes) { JCE_FREE(buf); return NULL; }

            /* Grow buffer if needed. */
            if (out + 4 + nal_len > cap) {
                cap = (out + 4 + nal_len) * 2;
                uint8_t *tmp = (uint8_t *)JCE_REALLOC(buf, cap);
                if (!tmp) { JCE_FREE(buf); return NULL; }
                buf = tmp;
            }

            memcpy(buf + out, k_start_code, 4); out += 4;
            memcpy(buf + out, hvcc + pos, nal_len); out += nal_len;
            pos += nal_len;
        }
    }

    *out_len = out;
    return buf;
}

/* Convert HVCC-formatted sample (length-prefixed NALs) to Annex B. */
static uint32_t hvcc_to_annexb(const uint8_t *src, uint32_t src_bytes,
                               uint8_t nal_length_size,
                               uint8_t *dst, uint32_t dst_cap)
{
    uint32_t pos = 0, out = 0;

    if (!src || !dst || nal_length_size < 1 || nal_length_size > 4)
        return 0;

    while (pos + nal_length_size <= src_bytes) {
        uint32_t nal_len = read_be(src + pos, nal_length_size);
        pos += nal_length_size;

        if (nal_len == 0 || pos + nal_len > src_bytes) break;
        if (out + 4 + nal_len > dst_cap) return 0;

        memcpy(dst + out, k_start_code, 4); out += 4;
        memcpy(dst + out, src + pos, nal_len); out += nal_len;
        pos += nal_len;
    }
    return out;
}

/* ── Decoder struct ───────────────────────────────────────────────── */

struct JceH265Decoder {
    iv_obj_t *codec;

    uint8_t  *annexb_buf;
    uint32_t  annexb_cap;

    uint8_t  *rgba_buf;
    uint32_t  rgba_cap;
    uint32_t  last_w;
    uint32_t  last_h;

    /* VPS/SPS/PPS in Annex B form. */
    uint8_t  *param_sets;
    uint32_t  param_sets_len;
    uint8_t   nal_length_size;

    /* Output buffers for the decoder. */
    uint8_t  *out_y;
    uint8_t  *out_u;
    uint8_t  *out_v;
    uint32_t  out_buf_size;

    bool      header_decoded;
};

/* ── Internal helpers ─────────────────────────────────────────────── */

static bool hevc_set_decode_mode(iv_obj_t *codec, IVD_VIDEO_DECODE_MODE_T mode)
{
    ihevcd_cxa_ctl_set_config_ip_t ip;
    ihevcd_cxa_ctl_set_config_op_t op;

    memset(&ip, 0, sizeof(ip));
    memset(&op, 0, sizeof(op));

    ip.s_ivd_ctl_set_config_ip_t.u4_size = sizeof(ip);
    ip.s_ivd_ctl_set_config_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.s_ivd_ctl_set_config_ip_t.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    ip.s_ivd_ctl_set_config_ip_t.e_vid_dec_mode = mode;
    ip.s_ivd_ctl_set_config_ip_t.e_frm_skip_mode = IVD_SKIP_NONE;
    ip.s_ivd_ctl_set_config_ip_t.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    ip.s_ivd_ctl_set_config_ip_t.u4_disp_wd = 0;

    op.s_ivd_ctl_set_config_op_t.u4_size = sizeof(op);

    IV_API_CALL_STATUS_T ret = ihevcd_cxa_api_function(
        codec, (void *)&ip, (void *)&op);
    return (ret == IV_SUCCESS);
}

static bool hevc_set_num_cores(iv_obj_t *codec, uint32_t num_cores)
{
    ihevcd_cxa_ctl_set_num_cores_ip_t ip;
    ihevcd_cxa_ctl_set_num_cores_op_t op;

    memset(&ip, 0, sizeof(ip));
    memset(&op, 0, sizeof(op));

    ip.u4_size = sizeof(ip);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IHEVCD_CXA_CMD_CTL_SET_NUM_CORES;
    ip.u4_num_cores = num_cores;
    op.u4_size = sizeof(op);

    IV_API_CALL_STATUS_T ret = ihevcd_cxa_api_function(
        codec, (void *)&ip, (void *)&op);
    return (ret == IV_SUCCESS);
}

static bool hevc_get_buf_info(iv_obj_t *codec,
                              uint32_t *out_min_out_bufs,
                              uint32_t out_sizes[3])
{
    ihevcd_cxa_ctl_getbufinfo_ip_t ip;
    ihevcd_cxa_ctl_getbufinfo_op_t op;

    memset(&ip, 0, sizeof(ip));
    memset(&op, 0, sizeof(op));

    ip.s_ivd_ctl_getbufinfo_ip_t.u4_size = sizeof(ip);
    ip.s_ivd_ctl_getbufinfo_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.s_ivd_ctl_getbufinfo_ip_t.e_sub_cmd = IVD_CMD_CTL_GETBUFINFO;
    op.s_ivd_ctl_getbufinfo_op_t.u4_size = sizeof(op);

    IV_API_CALL_STATUS_T ret = ihevcd_cxa_api_function(
        codec, (void *)&ip, (void *)&op);

    if (ret == IV_SUCCESS) {
        *out_min_out_bufs = op.s_ivd_ctl_getbufinfo_op_t.u4_min_num_out_bufs;
        for (uint32_t i = 0; i < 3; ++i)
            out_sizes[i] = op.s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[i];
    }
    return (ret == IV_SUCCESS);
}

static bool hevc_alloc_output_bufs(JceH265Decoder *dec)
{
    uint32_t num_bufs = 0;
    uint32_t sizes[3] = {0};

    if (!hevc_get_buf_info(dec->codec, &num_bufs, sizes))
        return false;

    /* For YUV420P: 3 planes. Allocate one contiguous block. */
    uint32_t total = sizes[0] + sizes[1] + sizes[2];
    if (total == 0) {
        /* Header not yet decoded; use a generous default (4K). */
        uint32_t w = 3840, h = 2160;
        sizes[0] = w * h;
        sizes[1] = sizes[2] = (w * h) / 4;
        total = sizes[0] + sizes[1] + sizes[2];
    }

    if (total > dec->out_buf_size) {
        uint8_t *buf = (uint8_t *)JCE_REALLOC(dec->out_y, total);
        if (!buf) return false;
        dec->out_y = buf;
        dec->out_u = buf + sizes[0];
        dec->out_v = buf + sizes[0] + sizes[1];
        dec->out_buf_size = total;
    }
    return true;
}

/* Feed data to the decoder and optionally produce output. */
static bool hevc_decode_raw(JceH265Decoder *dec,
                            const uint8_t *data, uint32_t data_len,
                            uint32_t *out_bytes_consumed,
                            uint32_t *out_width, uint32_t *out_height,
                            bool *out_has_output,
                            iv_yuv_buf_t *out_yuv)
{
    ihevcd_cxa_video_decode_ip_t ip;
    ihevcd_cxa_video_decode_op_t op;

    memset(&ip, 0, sizeof(ip));
    memset(&op, 0, sizeof(op));

    ivd_video_decode_ip_t *ps_ip = &ip.s_ivd_video_decode_ip_t;
    ivd_video_decode_op_t *ps_op = &op.s_ivd_video_decode_op_t;

    ps_ip->u4_size = sizeof(ip);
    ps_ip->e_cmd = IVD_CMD_VIDEO_DECODE;
    ps_ip->u4_ts = 0;
    ps_ip->pv_stream_buffer = (void *)data;
    ps_ip->u4_num_Bytes = data_len;

    /* Set output buffer pointers. */
    if (dec->out_y) {
        ps_ip->s_out_buffer.u4_num_bufs = 3;
        ps_ip->s_out_buffer.pu1_bufs[0] = dec->out_y;
        ps_ip->s_out_buffer.pu1_bufs[1] = dec->out_u;
        ps_ip->s_out_buffer.pu1_bufs[2] = dec->out_v;
        /* Compute sizes from what we allocated. */
        uint32_t y_size = (uint32_t)(dec->out_u - dec->out_y);
        uint32_t uv_size = (uint32_t)(dec->out_v - dec->out_u);
        ps_ip->s_out_buffer.u4_min_out_buf_size[0] = y_size;
        ps_ip->s_out_buffer.u4_min_out_buf_size[1] = uv_size;
        ps_ip->s_out_buffer.u4_min_out_buf_size[2] = uv_size;
    }

    ps_op->u4_size = sizeof(op);

    IV_API_CALL_STATUS_T ret = ihevcd_cxa_api_function(
        dec->codec, (void *)&ip, (void *)&op);

    *out_bytes_consumed = ps_op->u4_num_bytes_consumed;
    *out_width = ps_op->u4_pic_wd;
    *out_height = ps_op->u4_pic_ht;
    *out_has_output = (ps_op->u4_output_present != 0);
    if (out_yuv)
        *out_yuv = ps_op->s_disp_frm_buf;

    /* Non-fatal errors (corrupted data) are OK — we still try to decode. */
    if (ret != IV_SUCCESS && (ps_op->u4_error_code & 0xFF) != 0) {
        /* Only log fatal errors. */
        if (ps_op->u4_error_code & (1u << 14)) {  /* IVD_FATALERROR bit */
            LOG_WARN(LOG_TAG, "HEVC fatal decode error: 0x%08x",
                     ps_op->u4_error_code);
            return false;
        }
    }
    return true;
}

/* ── Public API ───────────────────────────────────────────────────── */

JceH265Decoder *jce_h265_decoder_open(const void *hvcc,
                                      uint32_t hvcc_bytes)
{
    if (!hvcc || hvcc_bytes < 23) {
        LOG_WARN(LOG_TAG, "Invalid hvcC data (too short: %u)", hvcc_bytes);
        return NULL;
    }

    JceH265Decoder *dec = (JceH265Decoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) return NULL;

    /* Parse hvcC → Annex B parameter sets. */
    dec->param_sets = parse_hvcc_params(
        (const uint8_t *)hvcc, hvcc_bytes,
        &dec->param_sets_len, &dec->nal_length_size);

    if (!dec->param_sets || dec->param_sets_len == 0) {
        LOG_WARN(LOG_TAG, "Failed to parse hvcC parameter sets");
        JCE_FREE(dec);
        return NULL;
    }

    /* Create the HEVC decoder instance. */
    {
        ihevcd_cxa_create_ip_t cip;
        ihevcd_cxa_create_op_t cop;

        memset(&cip, 0, sizeof(cip));
        memset(&cop, 0, sizeof(cop));

        cip.s_ivd_create_ip_t.u4_size = sizeof(cip);
        cip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
        cip.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
        cip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
        cip.s_ivd_create_ip_t.pf_aligned_alloc = jce_hevc_aligned_alloc;
        cip.s_ivd_create_ip_t.pf_aligned_free = jce_hevc_aligned_free;
        cip.s_ivd_create_ip_t.pv_mem_ctxt = NULL;
        cip.u4_enable_frame_info = 0;
        cip.u4_keep_threads_active = 0;
        cop.s_ivd_create_op_t.u4_size = sizeof(cop);

        IV_API_CALL_STATUS_T ret = ihevcd_cxa_api_function(
            NULL, (void *)&cip, (void *)&cop);

        if (ret != IV_SUCCESS) {
            LOG_WARN(LOG_TAG, "Failed to create HEVC decoder: 0x%08x",
                     cop.s_ivd_create_op_t.u4_error_code);
            JCE_FREE(dec->param_sets);
            JCE_FREE(dec);
            return NULL;
        }

        dec->codec = (iv_obj_t *)cop.s_ivd_create_op_t.pv_handle;
        dec->codec->pv_fxns = (void *)&ihevcd_cxa_api_function;
        dec->codec->u4_size = sizeof(iv_obj_t);
    }

    /* Configure: single core, decode frame mode. */
    hevc_set_num_cores(dec->codec, 1);
    hevc_set_decode_mode(dec->codec, IVD_DECODE_HEADER);

    /* Allocate initial output buffers. */
    hevc_alloc_output_bufs(dec);

    /* Feed VPS/SPS/PPS to decode headers. */
    {
        uint32_t consumed = 0, w = 0, h = 0;
        bool has_out = false;
        iv_yuv_buf_t yuv;

        hevc_decode_raw(dec, dec->param_sets, dec->param_sets_len,
                        &consumed, &w, &h, &has_out, &yuv);

        if (w > 0 && h > 0) {
            dec->header_decoded = true;
            /* Re-allocate output buffers now that we know dimensions. */
            hevc_alloc_output_bufs(dec);
        }
    }

    /* Switch to frame decode mode. */
    hevc_set_decode_mode(dec->codec, IVD_DECODE_FRAME);

    LOG_INFO(LOG_TAG, "HEVC decoder opened (nal_length_size=%u, params=%u bytes)",
             dec->nal_length_size, dec->param_sets_len);
    return dec;
}

bool jce_h265_decode_frame(JceH265Decoder *dec,
                           const void *hvcc_sample,
                           uint32_t sample_bytes,
                           uint8_t nal_length_size,
                           const uint8_t **out_rgba,
                           uint32_t *out_width,
                           uint32_t *out_height)
{
    if (!dec || !hvcc_sample || sample_bytes == 0) return false;

    /* Use the nal_length_size from hvcC if caller passes the stored one. */
    if (nal_length_size == 0)
        nal_length_size = dec->nal_length_size;

    /* Convert HVCC → Annex B. */
    uint32_t max_annexb = sample_bytes + (sample_bytes / 4 + 1) * 4;
    if (max_annexb > dec->annexb_cap) {
        uint8_t *buf = (uint8_t *)JCE_REALLOC(dec->annexb_buf, max_annexb);
        if (!buf) return false;
        dec->annexb_buf = buf;
        dec->annexb_cap = max_annexb;
    }

    uint32_t annexb_len = hvcc_to_annexb(
        (const uint8_t *)hvcc_sample, sample_bytes,
        nal_length_size, dec->annexb_buf, dec->annexb_cap);

    if (annexb_len == 0) {
        LOG_WARN(LOG_TAG, "HVCC→Annex B conversion failed");
        return false;
    }

    /* Ensure output buffers are allocated. */
    if (!dec->out_y && !hevc_alloc_output_bufs(dec))
        return false;

    /* Feed to decoder. */
    uint32_t consumed = 0, w = 0, h = 0;
    bool has_output = false;
    iv_yuv_buf_t yuv;
    memset(&yuv, 0, sizeof(yuv));

    /* Feed all data — decoder may consume in chunks. */
    uint32_t offset = 0;
    while (offset < annexb_len) {
        if (!hevc_decode_raw(dec, dec->annexb_buf + offset,
                             annexb_len - offset,
                             &consumed, &w, &h, &has_output, &yuv))
            return false;

        if (consumed == 0) break;
        offset += consumed;

        /* If dimensions changed, reallocate output buffers. */
        if (w > 0 && h > 0 && !dec->header_decoded) {
            dec->header_decoded = true;
            hevc_alloc_output_bufs(dec);
        }

        if (has_output) break;
    }

    if (!has_output) {
        /* Try one more decode with empty input to flush display. */
        hevc_decode_raw(dec, dec->annexb_buf, 0,
                        &consumed, &w, &h, &has_output, &yuv);
    }

    if (!has_output || w == 0 || h == 0)
        return false;

    /* Convert YUV420P to RGBA8. */
    uint32_t rgba_size = w * h * 4;
    if (rgba_size > dec->rgba_cap) {
        uint8_t *buf = (uint8_t *)JCE_REALLOC(dec->rgba_buf, rgba_size);
        if (!buf) return false;
        dec->rgba_buf = buf;
        dec->rgba_cap = rgba_size;
    }

    jce_yuv420_to_rgba(
        (const uint8_t *)yuv.pv_y_buf, (int)yuv.u4_y_strd,
        (const uint8_t *)yuv.pv_u_buf, (int)yuv.u4_u_strd,
        (const uint8_t *)yuv.pv_v_buf, (int)yuv.u4_v_strd,
        dec->rgba_buf, w, h);

    dec->last_w = w;
    dec->last_h = h;
    *out_rgba = dec->rgba_buf;
    *out_width = w;
    *out_height = h;
    return true;
}

void jce_h265_decoder_flush(JceH265Decoder *dec)
{
    if (!dec || !dec->codec) return;

    /* Issue flush command. */
    {
        ihevcd_cxa_ctl_flush_ip_t ip;
        ihevcd_cxa_ctl_flush_op_t op;

        memset(&ip, 0, sizeof(ip));
        memset(&op, 0, sizeof(op));

        ip.s_ivd_ctl_flush_ip_t.u4_size = sizeof(ip);
        ip.s_ivd_ctl_flush_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
        ip.s_ivd_ctl_flush_ip_t.e_sub_cmd = IVD_CMD_CTL_FLUSH;
        op.s_ivd_ctl_flush_op_t.u4_size = sizeof(op);

        ihevcd_cxa_api_function(dec->codec, (void *)&ip, (void *)&op);
    }

    /* Drain any remaining frames from the flush. */
    for (int i = 0; i < 64; ++i) {
        uint32_t consumed = 0, w = 0, h = 0;
        bool has_output = false;
        iv_yuv_buf_t yuv;

        hevc_decode_raw(dec, NULL, 0, &consumed, &w, &h, &has_output, &yuv);
        if (!has_output) break;
    }

    /* Reset the decoder for fresh decoding. */
    {
        ihevcd_cxa_ctl_reset_ip_t ip;
        ihevcd_cxa_ctl_reset_op_t op;

        memset(&ip, 0, sizeof(ip));
        memset(&op, 0, sizeof(op));

        ip.s_ivd_ctl_reset_ip_t.u4_size = sizeof(ip);
        ip.s_ivd_ctl_reset_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
        ip.s_ivd_ctl_reset_ip_t.e_sub_cmd = IVD_CMD_CTL_RESET;
        op.s_ivd_ctl_reset_op_t.u4_size = sizeof(op);

        ihevcd_cxa_api_function(dec->codec, (void *)&ip, (void *)&op);
    }

    /* Re-feed parameter sets after reset. */
    hevc_set_decode_mode(dec->codec, IVD_DECODE_HEADER);
    if (dec->param_sets && dec->param_sets_len > 0) {
        uint32_t consumed = 0, w = 0, h = 0;
        bool has_output = false;
        iv_yuv_buf_t yuv;

        hevc_decode_raw(dec, dec->param_sets, dec->param_sets_len,
                        &consumed, &w, &h, &has_output, &yuv);
    }
    hevc_set_decode_mode(dec->codec, IVD_DECODE_FRAME);
}

void jce_h265_decoder_close(JceH265Decoder *dec)
{
    if (!dec) return;

    if (dec->codec) {
        ihevcd_cxa_delete_ip_t ip;
        ihevcd_cxa_delete_op_t op;

        memset(&ip, 0, sizeof(ip));
        memset(&op, 0, sizeof(op));

        ip.s_ivd_delete_ip_t.u4_size = sizeof(ip);
        ip.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
        op.s_ivd_delete_op_t.u4_size = sizeof(op);

        ihevcd_cxa_api_function(dec->codec, (void *)&ip, (void *)&op);
        dec->codec = NULL;
    }

    JCE_FREE(dec->annexb_buf);
    JCE_FREE(dec->rgba_buf);
    JCE_FREE(dec->param_sets);
    JCE_FREE(dec->out_y);  /* out_u and out_v are offsets into this block */
    JCE_FREE(dec);
}

#else /* !JCE_ENABLE_PATENTED_CODECS */

JceH265Decoder *jce_h265_decoder_open(const void *hvcc, uint32_t hvcc_bytes)
{ (void)hvcc; (void)hvcc_bytes; return (JceH265Decoder *)0; }

bool jce_h265_decode_frame(JceH265Decoder *dec, const void *hvcc_sample,
                           uint32_t sample_bytes, uint8_t nal_length_size,
                           const uint8_t **out_rgba,
                           uint32_t *out_width, uint32_t *out_height)
{
    (void)dec; (void)hvcc_sample; (void)sample_bytes; (void)nal_length_size;
    if (out_rgba)   *out_rgba   = (const uint8_t *)0;
    if (out_width)  *out_width  = 0u;
    if (out_height) *out_height = 0u;
    return false;
}

void jce_h265_decoder_flush(JceH265Decoder *dec) { (void)dec; }
void jce_h265_decoder_close(JceH265Decoder *dec) { (void)dec; }

#endif /* JCE_ENABLE_PATENTED_CODECS */
