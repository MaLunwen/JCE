/*
 * jce_webm_encoder.cpp  VP9 (+ Opus) -> Matroska/WebM encoder.
 *
 * BGRA -> I420 (BT.601) -> VP9 (libvpx, realtime); f32 PCM -> Opus (libopus).
 * Both streams are buffered into a timestamp-sorted pending queue and muxed in
 * order via libwebm mkvmuxer (which requires non-decreasing AddFrame ts).
 */

#include <jce/middleware/video/jce_webm_encoder.h>
#include <jce/os/core/jce_log.h>

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_image.h>

#include <opus.h>

#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>

#include <new>
#include <map>
#include <vector>
#include <cstdint>
#include <cstring>

#define LOG_TAG     "jce_webm_enc"
#define MUX_LAG_NS  500000000ull   /* 0.5s reorder window for A/V interleave */

namespace {
struct Pkt {
    uint64_t            track;
    bool                key;
    std::vector<uint8_t> data;
};
}

struct JceWebmEncoder {
    uint32_t            w = 0, h = 0, fps = 30;

    /* video */
    vpx_codec_ctx_t     codec{};
    vpx_image_t         img{};
    bool                codec_ok = false, img_ok = false;

    /* audio (Opus) */
    OpusEncoder        *opus = nullptr;
    int                 aud_ch = 0;
    uint64_t            aud_track = 0;
    bool                audio_ok = false;
    std::vector<float>  aud_acc;          /* interleaved f32 accumulator */
    uint64_t            aud_samples = 0;  /* per-channel samples encoded */
    uint64_t            aud_anchor_ms = 0;
    bool                aud_anchored = false;

    /* mux */
    mkvmuxer::MkvWriter writer;
    mkvmuxer::Segment   segment;
    uint64_t            vid_track = 0;
    bool                mux_ok = false;

    std::multimap<uint64_t, Pkt> pending;   /* ts_ns -> packet */
    uint64_t            max_ts_ns = 0;
    int64_t             vframes = 0, aframes = 0;
};

/* BGRA8 -> I420 (BT.601 limited range). */
static void bgra_to_i420(const uint8_t *src, uint32_t pitch, int yflip,
                         vpx_image_t *img, uint32_t w, uint32_t h)
{
    uint8_t *yp = img->planes[VPX_PLANE_Y];
    uint8_t *up = img->planes[VPX_PLANE_U];
    uint8_t *vp = img->planes[VPX_PLANE_V];
    const int ys = img->stride[VPX_PLANE_Y];
    const int us = img->stride[VPX_PLANE_U];
    const int vs = img->stride[VPX_PLANE_V];
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t sy = yflip ? (h - 1u - y) : y;
        const uint8_t *row = src + (size_t)sy * pitch;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *p = row + (size_t)x * 4u;
            const int B = p[0], G = p[1], R = p[2];
            int Y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
            yp[(size_t)y * ys + x] = (uint8_t)(Y < 0 ? 0 : (Y > 255 ? 255 : Y));
            if ((y & 1u) == 0 && (x & 1u) == 0) {
                int U = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                int V = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
                up[(size_t)(y >> 1) * us + (x >> 1)] = (uint8_t)(U < 0 ? 0 : (U > 255 ? 255 : U));
                vp[(size_t)(y >> 1) * vs + (x >> 1)] = (uint8_t)(V < 0 ? 0 : (V > 255 ? 255 : V));
            }
        }
    }
}

static void queue_pkt(JceWebmEncoder *e, uint64_t ts_ns, uint64_t track,
                      bool key, const uint8_t *data, size_t len)
{
    Pkt p; p.track = track; p.key = key;
    p.data.assign(data, data + len);
    e->pending.emplace(ts_ns, std::move(p));
    if (ts_ns > e->max_ts_ns) e->max_ts_ns = ts_ns;
}

/* Flush pending packets older than the reorder window (or all when final). */
static void flush_ready(JceWebmEncoder *e, bool final_flush)
{
    const uint64_t horizon = (!final_flush && e->max_ts_ns > MUX_LAG_NS)
        ? e->max_ts_ns - MUX_LAG_NS : 0;
    while (!e->pending.empty()) {
        auto it = e->pending.begin();
        if (!final_flush && it->first > horizon) break;
        e->segment.AddFrame(it->second.data.data(), it->second.data.size(),
                            it->second.track, it->first, it->second.key);
        e->pending.erase(it);
    }
}

static void encoder_destroy(JceWebmEncoder *e)
{
    if (e->img_ok)   vpx_img_free(&e->img);
    if (e->codec_ok) vpx_codec_destroy(&e->codec);
    if (e->opus)     opus_encoder_destroy(e->opus);
    e->writer.Close();
    delete e;
}

extern "C" JceWebmEncoder *jce_webm_encoder_create(const char *path,
    uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
    uint32_t audio_sample_rate, uint32_t audio_channels)
{
    if (!path || !*path) return nullptr;
    width  &= ~1u; height &= ~1u;
    if (!width || !height) return nullptr;

    JceWebmEncoder *e = new (std::nothrow) JceWebmEncoder();
    if (!e) return nullptr;
    e->w = width; e->h = height; e->fps = fps ? fps : 30;

    /* ── VP9 ── */
    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0)) {
        LOG_ERROR(LOG_TAG, "vpx enc_config_default failed"); delete e; return nullptr;
    }
    cfg.g_w = width; cfg.g_h = height;
    cfg.g_timebase.num = 1; cfg.g_timebase.den = 1000;
    cfg.rc_target_bitrate = bitrate_kbps ? bitrate_kbps : 8000;
    cfg.rc_end_usage = VPX_VBR;
    cfg.g_error_resilient = 0; cfg.g_threads = 4;
    cfg.kf_max_dist = e->fps * 4u;
    if (vpx_codec_enc_init(&e->codec, vpx_codec_vp9_cx(), &cfg, 0)) {
        LOG_ERROR(LOG_TAG, "vpx enc_init failed: %s", vpx_codec_error(&e->codec));
        delete e; return nullptr;
    }
    e->codec_ok = true;
    vpx_codec_control(&e->codec, VP8E_SET_CPUUSED, 8);
    if (!vpx_img_alloc(&e->img, VPX_IMG_FMT_I420, width, height, 16)) {
        LOG_ERROR(LOG_TAG, "vpx_img_alloc failed"); encoder_destroy(e); return nullptr;
    }
    e->img_ok = true;

    /* ── Muxer ── */
    if (!e->writer.Open(path)) {
        LOG_ERROR(LOG_TAG, "open failed: %s", path); encoder_destroy(e); return nullptr;
    }
    if (!e->segment.Init(&e->writer)) {
        LOG_ERROR(LOG_TAG, "segment init failed"); encoder_destroy(e); return nullptr;
    }
    e->segment.set_mode(mkvmuxer::Segment::kFile);
    e->segment.OutputCues(true);
    e->vid_track = e->segment.AddVideoTrack((int)width, (int)height, 0);
    if (!e->vid_track) {
        LOG_ERROR(LOG_TAG, "AddVideoTrack failed"); encoder_destroy(e); return nullptr;
    }
    if (auto *vt = static_cast<mkvmuxer::VideoTrack *>(
            e->segment.GetTrackByNumber(e->vid_track)))
        vt->set_codec_id("V_VP9");

    /* ── Opus (optional) ── */
    if (audio_sample_rate > 0) {
        const int ch = (audio_channels >= 2) ? 2 : 1;
        int oerr = 0;
        e->opus = opus_encoder_create(48000, ch, OPUS_APPLICATION_AUDIO, &oerr);
        if (e->opus && oerr == OPUS_OK) {
            opus_encoder_ctl(e->opus, OPUS_SET_BITRATE(128000));
            int lookahead = 0;
            opus_encoder_ctl(e->opus, OPUS_GET_LOOKAHEAD(&lookahead));
            e->aud_ch = ch;
            e->aud_track = e->segment.AddAudioTrack(48000, ch, 0);
            if (e->aud_track) {
                /* OpusHead (RFC 7845), channel mapping family 0. */
                uint8_t head[19];
                memcpy(head, "OpusHead", 8);
                head[8] = 1;                 /* version */
                head[9] = (uint8_t)ch;       /* channel count */
                const uint16_t preskip = (uint16_t)lookahead;
                head[10] = (uint8_t)(preskip & 0xff);
                head[11] = (uint8_t)((preskip >> 8) & 0xff);
                const uint32_t rate = 48000;
                head[12] = (uint8_t)(rate & 0xff);
                head[13] = (uint8_t)((rate >> 8) & 0xff);
                head[14] = (uint8_t)((rate >> 16) & 0xff);
                head[15] = (uint8_t)((rate >> 24) & 0xff);
                head[16] = 0; head[17] = 0;  /* output gain */
                head[18] = 0;                /* mapping family */
                if (auto *at = static_cast<mkvmuxer::AudioTrack *>(
                        e->segment.GetTrackByNumber(e->aud_track))) {
                    at->set_codec_id("A_OPUS");
                    at->SetCodecPrivate(head, sizeof(head));
                    /* Matroska A_OPUS conformance: CodecDelay = pre-skip in ns,
                       SeekPreRoll = 80ms (RFC 7845 / Matroska Opus spec). Without
                       these, strict parsers warn ("Error parsing Opus packet
                       header") and the first frames aren't delay-trimmed. */
                    at->set_codec_delay((uint64_t)lookahead * 1000000000ull / 48000ull);
                    at->set_seek_pre_roll(80000000ull);
                }
                e->audio_ok = true;
            }
        } else {
            LOG_WARN(LOG_TAG, "opus encoder create failed (%d) — video only", oerr);
            if (e->opus) { opus_encoder_destroy(e->opus); e->opus = nullptr; }
        }
    }

    e->mux_ok = true;
    LOG_SUCCESS(LOG_TAG, "encoder %ux%u @%ufps vp9%s -> %s",
                width, height, e->fps, e->audio_ok ? "+opus" : "", path);
    return e;
}

extern "C" bool jce_webm_encoder_push_bgra(JceWebmEncoder *e, const void *bgra,
    uint32_t pitch, int yflip, uint64_t ts_ms)
{
    if (!e || !e->codec_ok || !e->mux_ok || !bgra) return false;
    bgra_to_i420((const uint8_t *)bgra, pitch, yflip, &e->img, e->w, e->h);
    if (vpx_codec_encode(&e->codec, &e->img, (vpx_codec_pts_t)ts_ms, 1, 0,
                         VPX_DL_REALTIME) != VPX_CODEC_OK) {
        LOG_ERROR(LOG_TAG, "vpx encode failed: %s", vpx_codec_error(&e->codec));
        return false;
    }
    vpx_codec_iter_t iter = nullptr;
    const vpx_codec_cx_pkt_t *pkt;
    while ((pkt = vpx_codec_get_cx_data(&e->codec, &iter)) != nullptr) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
        const bool key = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
        queue_pkt(e, (uint64_t)pkt->data.frame.pts * 1000000ull, e->vid_track, key,
                  (const uint8_t *)pkt->data.frame.buf, pkt->data.frame.sz);
        e->vframes++;
    }
    flush_ready(e, false);
    return true;
}

extern "C" bool jce_webm_encoder_push_audio(JceWebmEncoder *e, const float *pcm,
    uint32_t frames, uint64_t ts_ms)
{
    if (!e || !e->audio_ok || !e->opus || !pcm || !frames) return false;
    if (!e->aud_anchored) { e->aud_anchor_ms = ts_ms; e->aud_anchored = true; }

    e->aud_acc.insert(e->aud_acc.end(), pcm, pcm + (size_t)frames * e->aud_ch);

    const size_t frame_samples = 960;                 /* 20ms @ 48kHz */
    const size_t chunk = frame_samples * (size_t)e->aud_ch;
    unsigned char out[4000];
    while (e->aud_acc.size() >= chunk) {
        const int n = opus_encode_float(e->opus, e->aud_acc.data(),
                                        (int)frame_samples, out, sizeof(out));
        if (n > 1) {
            const uint64_t ts_ns =
                (e->aud_anchor_ms + e->aud_samples / 48ull) * 1000000ull;
            queue_pkt(e, ts_ns, e->aud_track, true, out, (size_t)n);
            e->aframes++;
        }
        e->aud_samples += frame_samples;
        e->aud_acc.erase(e->aud_acc.begin(), e->aud_acc.begin() + chunk);
    }
    flush_ready(e, false);
    return true;
}

extern "C" void jce_webm_encoder_finish(JceWebmEncoder *e)
{
    if (!e) return;
    /* Flush VP9. */
    if (e->codec_ok) {
        for (;;) {
            if (vpx_codec_encode(&e->codec, nullptr, 0, 1, 0, VPX_DL_REALTIME) != VPX_CODEC_OK)
                break;
            vpx_codec_iter_t iter = nullptr;
            const vpx_codec_cx_pkt_t *pkt;
            bool any = false;
            while ((pkt = vpx_codec_get_cx_data(&e->codec, &iter)) != nullptr) {
                any = true;
                if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
                const bool key = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
                queue_pkt(e, (uint64_t)pkt->data.frame.pts * 1000000ull, e->vid_track,
                          key, (const uint8_t *)pkt->data.frame.buf, pkt->data.frame.sz);
            }
            if (!any) break;
        }
    }
    flush_ready(e, true);            /* drain all pending in ts order */
    if (e->mux_ok) e->segment.Finalize();
    LOG_SUCCESS(LOG_TAG, "finalized: %lld video + %lld audio frames",
                (long long)e->vframes, (long long)e->aframes);
    encoder_destroy(e);
}
