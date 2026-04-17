/*
 * jce_video.cpp  MP4 metadata + cross-platform frame decode.
 *
 * Container:   Manual MP4/ISO-BMFF parsing via jce_mp4_parser (all platforms).
 * Decode:      OpenH264 software H.264 decoder (cross-platform).
 */

#include <jce/video/jce_video.h>
#include <jce/video/jce_mp4_parser.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"
#include "jce_aac_decode.h"
#include "jce_h264_decode.h"
#include "jce_h265_decode.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>

/* ======================================================================
 *  Shared slot pool
 * ====================================================================== */

#define LOG_TAG       "jce_video"
#define JCE_MAX_VIDEOS 8
#define JCE_VIDEO_AUDIO_PCM_MAX_BYTES (100u * 1024u * 1024u) /* 100 MB */

typedef struct {
    JceMp4Parser        *parser;        /* stays open for sample access */
    JceH264Decoder      *h264;          /* OpenH264 H.264 decoder */
    JceH265Decoder      *h265;          /* AOSP libhevc H.265/HEVC decoder */
    JceMp4VideoTrackInfo vtrack;        /* cached video track info */
    uint32_t             sample_idx;    /* next sample to decode */
    bool                 ended;
    uint32_t             width;
    uint32_t             height;
    uint8_t             *mp4_copy;      /* owned copy of raw MP4 data */
    size_t               mp4_copy_size;
} JceVideoDecoder;

typedef struct {
    bool     used;

    int      width;
    int      height;
    double   framerate;
    double   duration;

    bool     has_audio;
    int      samplerate;
    int      audio_channels;
    uint32_t audio_sample_count;
    JceVideoAudioStatus audio_status;
    char     audio_status_text[128];

    bool     metadata_only;
    char     video_codec[5];
    char     audio_codec[5];

    bool     loop;
    bool     ended;
    double   time;

    uint8_t *rgba;
    int      rgba_w;
    int      rgba_h;
    double   frame_time;
    uint64_t frame_counter;
    bool     decode_ts_base_set;
    double   decode_ts_base_sec;
    bool     post_seek;          /* suppress monotonicity in normalize_frame_time */

    /* Decoded embedded audio (full-clip s16 PCM, owned). */
    int16_t *audio_pcm;
    uint32_t audio_pcm_frames;
    uint32_t audio_pcm_channels;
    uint32_t audio_pcm_samplerate;

    JceVideoDecoder decoder;
} VideoSlot;

static VideoSlot s_slots[JCE_MAX_VIDEOS];

static void set_audio_status(VideoSlot *slot,
                             JceVideoAudioStatus status,
                             const char *text)
{
    if (!slot) {
        return;
    }
    slot->audio_status = status;
    snprintf(slot->audio_status_text,
             sizeof(slot->audio_status_text),
             "%s",
             text ? text : "");
}

static VideoSlot *slot_from_handle(JceVideo v)
{
    if (v == JCE_VIDEO_INVALID) return NULL;
    uint32_t idx = (uint32_t)v - 1u;
    if (idx >= JCE_MAX_VIDEOS) return NULL;
    if (!s_slots[idx].used) return NULL;
    return &s_slots[idx];
}

static int alloc_slot(void)
{
    for (int i = 0; i < JCE_MAX_VIDEOS; ++i)
        if (!s_slots[i].used) return i;
    return -1;
}

/* ======================================================================
 *  Shared RGBA buffer helper
 * ====================================================================== */

static bool ensure_rgba(VideoSlot *slot, uint32_t w, uint32_t h)
{
    if (!slot || w == 0u || h == 0u) return false;
    if (slot->rgba && slot->rgba_w == (int)w && slot->rgba_h == (int)h)
        return true;

    size_t need = (size_t)w * (size_t)h * 4u;
    JCE_FREE(slot->rgba);
    slot->rgba = (uint8_t *)JCE_MALLOC(need);
    if (!slot->rgba) { slot->rgba_w = 0; slot->rgba_h = 0; return false; }
    slot->rgba_w = (int)w;
    slot->rgba_h = (int)h;
    return true;
}

static double frame_interval_seconds(const VideoSlot *slot)
{
    double fps = slot ? slot->framerate : 0.0;
    if (!(fps >= 1.0 && fps <= 240.0)) {
        fps = 30.0;
    }
    return 1.0 / fps;
}

/*
 * Normalize backend timestamps to a monotonic local playback clock.
 * Some files expose large/non-zero starts or repeated timestamps,
 * which would otherwise stall frame advancement logic.
 */
static double normalize_frame_time(VideoSlot *slot,
                                   double raw_time_sec,
                                   bool has_raw_time)
{
    if (!slot) {
        return 0.0;
    }

    if (slot->frame_counter == 0u) {
        if (has_raw_time && raw_time_sec >= 0.0) {
            slot->decode_ts_base_set = true;
            slot->decode_ts_base_sec = raw_time_sec;
        } else {
            slot->decode_ts_base_set = false;
            slot->decode_ts_base_sec = 0.0;
        }
        return 0.0;
    }

    double t = slot->frame_time + frame_interval_seconds(slot);
    if (has_raw_time && raw_time_sec >= 0.0) {
        if (!slot->decode_ts_base_set) {
            slot->decode_ts_base_set = true;
            slot->decode_ts_base_sec = raw_time_sec;
        }
        t = raw_time_sec - slot->decode_ts_base_sec;
        if (t < 0.0) {
            t = 0.0;
        }
        /* Only enforce monotonicity during normal playback, not after
         * a seek — backward seeks legitimately produce earlier timestamps. */
        if (!slot->post_seek && t <= slot->frame_time + 0.0005) {
            t = slot->frame_time + frame_interval_seconds(slot);
        }
    }
    return t;
}

/* ======================================================================
 *  Unified OpenH264 decoder wrappers
 * ====================================================================== */

static bool decoder_open(VideoSlot *slot, const void *data, uint32_t size,
                         JceMp4Parser *parser)
{
    if (!slot || !parser) return false;

    JceMp4VideoTrackInfo vti;
    if (!jce_mp4_parser_get_video_track_info(parser, &vti)) {
        LOG_WARN(LOG_TAG, "decoder_open: no video track in MP4");
        return false;
    }

    /* Check for supported codecs: H.264 (AVC) or H.265 (HEVC). */
    bool is_avc  = (strcmp(vti.codec, "avc1") == 0 || strcmp(vti.codec, "avc3") == 0);
    bool is_hevc = (strcmp(vti.codec, "hvc1") == 0 || strcmp(vti.codec, "hev1") == 0);
    if (!is_avc && !is_hevc) {
        LOG_WARN(LOG_TAG, "decoder_open: unsupported video codec '%s'",
                 vti.codec);
        return false;
    }

    uint32_t min_dsi = is_avc ? 7u : 23u;
    if (!vti.decoder_config || vti.decoder_config_bytes < min_dsi) {
        LOG_WARN(LOG_TAG, "decoder_open: missing %s decoder config",
                 is_avc ? "avcC" : "hvcC");
        return false;
    }

    /* Copy the raw MP4 data so the parser's blob reference stays valid. */
    slot->decoder.mp4_copy = (uint8_t *)JCE_MALLOC(size);
    if (!slot->decoder.mp4_copy) return false;
    memcpy(slot->decoder.mp4_copy, data, size);
    slot->decoder.mp4_copy_size = (size_t)size;

    /* Re-open parser on the owned copy (the original may be freed). */
    JceMp4Info dummy;
    slot->decoder.parser = jce_mp4_parser_open_memory(
        slot->decoder.mp4_copy, (size_t)size, &dummy);
    if (!slot->decoder.parser) {
        JCE_FREE(slot->decoder.mp4_copy);
        slot->decoder.mp4_copy = NULL;
        return false;
    }

    /* Re-query video track info from the new parser instance. */
    if (!jce_mp4_parser_get_video_track_info(slot->decoder.parser,
                                             &slot->decoder.vtrack)) {
        jce_mp4_parser_close(slot->decoder.parser);
        slot->decoder.parser = NULL;
        JCE_FREE(slot->decoder.mp4_copy);
        slot->decoder.mp4_copy = NULL;
        return false;
    }

    /* Create the appropriate decoder backend. */
    if (is_avc) {
        slot->decoder.h264 = jce_h264_decoder_open(
            slot->decoder.vtrack.decoder_config,
            slot->decoder.vtrack.decoder_config_bytes);
        if (!slot->decoder.h264) {
            LOG_WARN(LOG_TAG, "decoder_open: failed to init OpenH264");
            jce_mp4_parser_close(slot->decoder.parser);
            slot->decoder.parser = NULL;
            JCE_FREE(slot->decoder.mp4_copy);
            slot->decoder.mp4_copy = NULL;
            return false;
        }
    } else {
        slot->decoder.h265 = jce_h265_decoder_open(
            slot->decoder.vtrack.decoder_config,
            slot->decoder.vtrack.decoder_config_bytes);
        if (!slot->decoder.h265) {
            LOG_WARN(LOG_TAG, "decoder_open: failed to init libhevc");
            jce_mp4_parser_close(slot->decoder.parser);
            slot->decoder.parser = NULL;
            JCE_FREE(slot->decoder.mp4_copy);
            slot->decoder.mp4_copy = NULL;
            return false;
        }
    }

    slot->decoder.width = slot->decoder.vtrack.width;
    slot->decoder.height = slot->decoder.vtrack.height;
    slot->decoder.sample_idx = 0;
    slot->decoder.ended = false;

    /* Decode first frame. */
    JceMp4SampleInfo si;
    if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, 0, &si)) {
        LOG_WARN(LOG_TAG, "decoder_open: could not read first video sample");
        if (slot->decoder.h264) {
            jce_h264_decoder_close(slot->decoder.h264);
            slot->decoder.h264 = NULL;
        }
        if (slot->decoder.h265) {
            jce_h265_decoder_close(slot->decoder.h265);
            slot->decoder.h265 = NULL;
        }
        jce_mp4_parser_close(slot->decoder.parser);
        slot->decoder.parser = NULL;
        JCE_FREE(slot->decoder.mp4_copy);
        slot->decoder.mp4_copy = NULL;
        return false;
    }

    const uint8_t *rgba = NULL;
    uint32_t dw = 0, dh = 0;
    bool got_frame;
    if (slot->decoder.h265) {
        got_frame = jce_h265_decode_frame(
            slot->decoder.h265,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    } else {
        got_frame = jce_h264_decode_frame(
            slot->decoder.h264,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    }

    if (got_frame && rgba && dw > 0 && dh > 0) {
        slot->decoder.width = dw;
        slot->decoder.height = dh;
        if (ensure_rgba(slot, dw, dh)) {
            memcpy(slot->rgba, rgba, (size_t)dw * dh * 4);
        }
        slot->frame_counter = 1;
    } else {
        /* First frame didn't produce output (B-frame delay or SPS/PPS only).
         * Leave frame_counter at 0 so upload_latest_frame won't try to
         * display a non-existent frame. */
        slot->frame_counter = 0;
        LOG_INFO(LOG_TAG, "decoder_open: first frame deferred (B-frame delay)");
    }
    slot->decoder.sample_idx = 1;
    slot->frame_time = normalize_frame_time(slot, 0.0, true);
    return true;
}

static bool decoder_read_next(VideoSlot *slot)
{
    if (!slot || (!slot->decoder.h264 && !slot->decoder.h265)
        || !slot->decoder.parser) return false;

    /* Drain any pending frame buffered by the H.264 two-step DPB drain.
     * This frame was produced during the previous decode call but could
     * not be returned because the feed step already produced output. */
    if (slot->decoder.h264) {
        const uint8_t *rgba = NULL;
        uint32_t dw = 0, dh = 0;
        if (jce_h264_decoder_drain_pending(slot->decoder.h264,
                                            &rgba, &dw, &dh)) {
            slot->decoder.width = dw;
            slot->decoder.height = dh;
            if (!ensure_rgba(slot, dw, dh)) return false;
            memcpy(slot->rgba, rgba, (size_t)dw * dh * 4);
            /* Use synthetic timestamp — normalize_frame_time enforces
             * monotonicity and will bump by one frame interval. */
            slot->frame_time = normalize_frame_time(slot, slot->frame_time, true);
            slot->frame_counter++;
            slot->decoder.ended = false;
            return true;
        }
    }

    if (slot->decoder.sample_idx >= slot->decoder.vtrack.sample_count) {
        slot->decoder.ended = true;
        return false;
    }

    JceMp4SampleInfo si;
    if (!jce_mp4_parser_get_video_sample(slot->decoder.parser,
                                         slot->decoder.sample_idx, &si)) {
        slot->decoder.ended = true;
        return false;
    }

    const uint8_t *rgba = NULL;
    uint32_t dw = 0, dh = 0;
    bool got_frame;
    if (slot->decoder.h265) {
        got_frame = jce_h265_decode_frame(
            slot->decoder.h265,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    } else {
        got_frame = jce_h264_decode_frame(
            slot->decoder.h264,
            slot->decoder.mp4_copy + si.offset,
            si.size_bytes,
            slot->decoder.vtrack.nal_length_size,
            &rgba, &dw, &dh);
    }

    slot->decoder.sample_idx++;

    if (!got_frame || !rgba || dw == 0 || dh == 0) {
        /* Decoder didn't produce a frame (B-frame delay, non-keyframe
         * without refs, etc).  Not an error — caller should keep going. */
        if (slot->decoder.sample_idx <= 8) {
            LOG_INFO(LOG_TAG, "sample[%u]: no frame (size=%u nal_len=%u)",
                     slot->decoder.sample_idx - 1, si.size_bytes,
                     (unsigned)slot->decoder.vtrack.nal_length_size);
        }
        return false;
    }

    if (slot->frame_counter < 3) {
        LOG_INFO(LOG_TAG, "sample[%u]: got frame %ux%u",
                 slot->decoder.sample_idx - 1, dw, dh);
    }

    slot->decoder.width = dw;
    slot->decoder.height = dh;
    if (!ensure_rgba(slot, dw, dh)) return false;
    memcpy(slot->rgba, rgba, (size_t)dw * dh * 4);

    double ts_sec = 0.0;
    if (slot->decoder.vtrack.timescale > 0) {
        ts_sec = (double)si.timestamp / (double)slot->decoder.vtrack.timescale;
    }
    slot->frame_time = normalize_frame_time(slot, ts_sec, true);
    slot->frame_counter++;
    slot->decoder.ended = false;
    return true;
}

static bool decoder_seek(VideoSlot *slot, double time_sec, bool exact)
{
    if (!slot || (!slot->decoder.h264 && !slot->decoder.h265)
        || !slot->decoder.parser) return false;
    if (time_sec < 0.0) time_sec = 0.0;

    /* Find sample index closest to |time_sec|. */
    uint32_t target_idx = 0;
    uint32_t sc = slot->decoder.vtrack.sample_count;
    uint32_t ts_scale = slot->decoder.vtrack.timescale;
    if (ts_scale == 0) ts_scale = 1;

    for (uint32_t i = 0; i < sc; ++i) {
        JceMp4SampleInfo si;
        if (!jce_mp4_parser_get_video_sample(slot->decoder.parser, i, &si))
            break;
        double st = (double)si.timestamp / (double)ts_scale;
        if (st > time_sec) break;
        target_idx = i;
    }

    /* Flush decoder state (discard stale reference frames). */
    if (slot->decoder.h265)
        jce_h265_decoder_flush(slot->decoder.h265);
    else
        jce_h264_decoder_flush(slot->decoder.h264);

    /* Reset frame counter so normalize_frame_time re-calibrates. */
    slot->frame_counter = 0;
    slot->decode_ts_base_set = false;
    slot->decoder.sample_idx = target_idx;
    slot->decoder.ended = false;
    slot->ended = false;
    slot->time = time_sec;

    /* Decode one frame at the seek position. */
    if (!decoder_read_next(slot)) {
        /* May not produce a frame (non-keyframe after flush).
         * Advance a few more samples to find a decodable frame. */
        int safety = 0;
        while (safety < 30 && slot->decoder.sample_idx < sc) {
            if (decoder_read_next(slot)) break;
            safety++;
        }
    }

    /* If exact, keep decoding until we reach the target time. */
    if (exact) {
        int safety = 0;
        while (slot->frame_time + 0.000001 < time_sec
               && safety < 240
               && slot->decoder.sample_idx < sc) {
            if (!decoder_read_next(slot)) {
                if (slot->decoder.ended) break;
                continue;
            }
            ++safety;
        }
    }
    return true;
}

static void decoder_close(VideoSlot *slot)
{
    if (!slot) return;
    if (slot->decoder.h264) {
        jce_h264_decoder_close(slot->decoder.h264);
        slot->decoder.h264 = NULL;
    }
    if (slot->decoder.h265) {
        jce_h265_decoder_close(slot->decoder.h265);
        slot->decoder.h265 = NULL;
    }
    if (slot->decoder.parser) {
        jce_mp4_parser_close(slot->decoder.parser);
        slot->decoder.parser = NULL;
    }
    JCE_FREE(slot->decoder.mp4_copy);
    slot->decoder.mp4_copy = NULL;
    slot->decoder.mp4_copy_size = 0;
}

/* ======================================================================
 *  Public API  (extern "C")
 * ====================================================================== */

extern "C" {

JceVideo jce_video_load_memory(const void *data, uint32_t size,
                               const char *hint_path)
{
    JceMp4Parser *parser;
    if (!data || size == 0u) return JCE_VIDEO_INVALID;

    int idx = alloc_slot();
    if (idx < 0) {
        LOG_WARN(LOG_TAG, "no free video slots");
        return JCE_VIDEO_INVALID;
    }

    JceMp4Info mp4;
    parser = jce_mp4_parser_open_memory(data, (size_t)size, &mp4);
    if (!parser) {
        LOG_ERROR(LOG_TAG, "load failed for '%s': %s",
            hint_path ? hint_path : "<memory>",
            mp4.error[0] ? mp4.error : "invalid mp4");
        return JCE_VIDEO_INVALID;
    }

    VideoSlot *slot = &s_slots[idx];
    memset(slot, 0, sizeof(*slot));

    slot->width      = (int)mp4.width;
    slot->height     = (int)mp4.height;
    slot->framerate  = mp4.framerate;
    slot->duration   = mp4.duration_seconds;
    slot->has_audio  = mp4.has_audio_track;
    slot->samplerate = (int)mp4.audio_samplerate_hz;
    slot->audio_channels = (int)mp4.audio_channels;
    slot->audio_sample_count = 0u;
    set_audio_status(slot, JCE_VIDEO_AUDIO_STATUS_NONE, "no embedded audio");
    slot->metadata_only = true;
    snprintf(slot->video_codec, sizeof(slot->video_codec), "%s",
        mp4.video_codec[0] ? mp4.video_codec : "unkn");
    snprintf(slot->audio_codec, sizeof(slot->audio_codec), "%s",
        mp4.audio_codec[0] ? mp4.audio_codec : "unkn");

    if (slot->has_audio) {
        JceMp4AudioTrackInfo atr;
        if (jce_mp4_parser_get_audio_track_info(parser, &atr)) {
            slot->audio_sample_count = atr.sample_count;

            if (strcmp(atr.codec, "mp4a") == 0
                && atr.decoder_config && atr.decoder_config_bytes > 0) {
                /* ── AAC decode to PCM ─────────────────────────── */
                JceAacDecoder *aac = jce_aac_decoder_open(
                    atr.decoder_config, atr.decoder_config_bytes);
                if (!aac) {
                    set_audio_status(slot,
                        JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                        "failed to initialize AAC decoder");
                } else {
                    uint32_t ch_est = atr.channels > 0 ? atr.channels : 2u;
                    uint32_t max_frame = 2048u; /* covers SBR */
                    uint64_t est_samples = (uint64_t)atr.sample_count
                                         * max_frame * ch_est;
                    uint64_t est_bytes = est_samples * sizeof(int16_t);

                    if (est_bytes > JCE_VIDEO_AUDIO_PCM_MAX_BYTES) {
                        set_audio_status(slot,
                            JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                            "audio track exceeds memory limit; video-only");
                    } else {
                        int16_t *pcm_buf = (int16_t *)JCE_MALLOC(
                            (size_t)est_bytes);
                        if (!pcm_buf) {
                            set_audio_status(slot,
                                JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                                "out of memory for audio decode");
                        } else {
                            uint32_t pcm_pos = 0;
                            uint32_t pcm_cap = (uint32_t)est_samples;
                            bool     ok      = true;
                            uint8_t *sbuf    = NULL;
                            uint32_t sbuf_cap = 0;

                            for (uint32_t si = 0; si < atr.sample_count; ++si) {
                                JceMp4SampleInfo sinfo;
                                if (!jce_mp4_parser_get_audio_sample(
                                        parser, si, &sinfo)) {
                                    ok = false;
                                    break;
                                }
                                if (sinfo.size_bytes > sbuf_cap) {
                                    JCE_FREE(sbuf);
                                    sbuf_cap = sinfo.size_bytes + 256u;
                                    sbuf = (uint8_t *)JCE_MALLOC(sbuf_cap);
                                    if (!sbuf) { ok = false; break; }
                                }
                                uint32_t copied = 0;
                                if (!jce_mp4_parser_copy_audio_sample(
                                        parser, si, sbuf,
                                        sbuf_cap, &copied)) {
                                    ok = false;
                                    break;
                                }
                                uint32_t samp_out = 0;
                                uint32_t remain   = pcm_cap - pcm_pos;
                                if (!jce_aac_decode_frame(
                                        aac, sbuf, copied,
                                        pcm_buf + pcm_pos,
                                        remain, &samp_out)) {
                                    continue; /* skip bad frames */
                                }
                                pcm_pos += samp_out;
                            }
                            JCE_FREE(sbuf);

                            if (ok && pcm_pos > 0) {
                                uint32_t ach = jce_aac_decoder_get_channels(aac);
                                uint32_t asr = jce_aac_decoder_get_samplerate(aac);
                                if (ach == 0) ach = ch_est;
                                if (asr == 0) asr = atr.samplerate_hz;
                                uint32_t aframes = pcm_pos / ach;

                                /* Trim buffer to actual size. */
                                size_t actual = (size_t)pcm_pos * sizeof(int16_t);
                                int16_t *trimmed = (int16_t *)JCE_REALLOC(
                                    pcm_buf, actual);
                                if (trimmed) pcm_buf = trimmed;

                                slot->audio_pcm            = pcm_buf;
                                slot->audio_pcm_frames     = aframes;
                                slot->audio_pcm_channels   = ach;
                                slot->audio_pcm_samplerate = asr;
                                set_audio_status(slot,
                                    JCE_VIDEO_AUDIO_STATUS_READY,
                                    "AAC audio decoded to PCM");
                                pcm_buf = NULL; /* ownership transferred */
                            } else {
                                set_audio_status(slot,
                                    JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                                    "failed to decode AAC audio samples");
                            }
                            JCE_FREE(pcm_buf);
                        }
                    }
                    jce_aac_decoder_close(aac);
                }
            } else if (strcmp(atr.codec, "mp4a") == 0) {
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_DECODER_UNAVAILABLE,
                                 "AAC track found but decoder config missing");
            } else if (atr.codec[0]) {
                char msg[128];
                snprintf(msg,
                         sizeof(msg),
                         "unsupported embedded audio codec: %s",
                         atr.codec);
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_UNSUPPORTED_CODEC,
                                 msg);
            } else {
                set_audio_status(slot,
                                 JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                                 "audio track found but codec metadata is missing");
            }
        } else {
            set_audio_status(slot,
                             JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR,
                             "audio track advertised but sample metadata is unavailable");
        }
    }

    /* Try cross-platform OpenH264 decoder (must happen before closing parser). */
    if (!decoder_open(slot, data, size, parser)) {
        LOG_WARN(LOG_TAG,
            "decoder backend unavailable for '%s' (codec=%s) — metadata-only mode",
            hint_path ? hint_path : "<memory>",
            slot->video_codec);
        slot->metadata_only = true;
    } else {
        if (slot->width <= 0 && slot->decoder.width > 0u)
            slot->width = (int)slot->decoder.width;
        if (slot->height <= 0 && slot->decoder.height > 0u)
            slot->height = (int)slot->decoder.height;
        if (slot->framerate <= 0.0)
            slot->framerate = 30.0;
        slot->metadata_only = false;
        slot->ended = false;
        slot->time  = 0.0;
    }

    /* Close the original parser — decoder_open made its own copy. */
    jce_mp4_parser_close(parser);

    slot->used = true;

    LOG_SUCCESS(LOG_TAG,
        "parsed '%s' codec=%s %dx%d fps=%.3f dur=%.3fs audio=%d(%s %dHz %dch) decode=%s",
        hint_path ? hint_path : "<memory>",
        slot->video_codec, slot->width, slot->height,
        slot->framerate, slot->duration,
        (int)slot->has_audio,
        slot->audio_codec,
        slot->samplerate,
        slot->audio_channels,
        slot->metadata_only ? "metadata-only" : "active");

    return (JceVideo)(idx + 1);
}

void jce_video_unload(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    decoder_close(slot);

    if (slot->audio_pcm) { JCE_FREE(slot->audio_pcm); slot->audio_pcm = NULL; }
    if (slot->rgba) { JCE_FREE(slot->rgba); slot->rgba = NULL; }
    memset(slot, 0, sizeof(*slot));
}

bool jce_video_get_info(JceVideo v, JceVideoInfo *out)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !out) return false;

    out->width          = slot->width;
    out->height         = slot->height;
    out->duration       = slot->duration;
    out->framerate      = slot->framerate;
    out->has_audio      = slot->has_audio;
    out->samplerate     = slot->samplerate;
    out->audio_channels = slot->audio_channels;
    out->metadata_only  = slot->metadata_only;
    snprintf(out->video_codec, sizeof(out->video_codec), "%s", slot->video_codec);
    snprintf(out->audio_codec, sizeof(out->audio_codec), "%s", slot->audio_codec);
    return true;
}

double jce_video_get_time(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->time : 0.0;
}

double jce_video_get_duration(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->duration : 0.0;
}

bool jce_video_has_ended(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->ended : true;
}

void jce_video_get_size(JceVideo v, int *out_w, int *out_h)
{
    VideoSlot *slot = slot_from_handle(v);
    if (out_w) *out_w = slot ? slot->width  : 0;
    if (out_h) *out_h = slot ? slot->height : 0;
}

void jce_video_advance(JceVideo v, double dt_seconds)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || dt_seconds <= 0.0) return;
    if (dt_seconds > 1.0) dt_seconds = 1.0;

    /* Metadata-only: tick the clock. */
    if (slot->metadata_only) {
        if (slot->duration <= 0.0) {
            slot->time += dt_seconds;
            slot->ended = false;
            return;
        }
        double t = slot->time + dt_seconds;
        if (t >= slot->duration) {
            if (slot->loop) {
                t = fmod(t, slot->duration);
                if (t < 0.0) t = 0.0;
                slot->ended = false;
            } else { t = slot->duration; slot->ended = true; }
        } else {
            slot->ended = false;
        }
        slot->time = t;
        return;
    }

    /* Decode path. */
    double target = slot->time + dt_seconds;
    if (slot->duration > 0.0) {
        if (slot->loop) {
            while (target >= slot->duration) {
                target -= slot->duration;
                slot->post_seek = true;
                if (!decoder_seek(slot, 0.0, false)) {
                    slot->post_seek = false;
                    LOG_WARN(LOG_TAG,
                        "loop seek failed; stopping playback at clip boundary");
                    slot->time = slot->duration;
                    slot->ended = true;
                    return;
                }
                slot->post_seek = false;
                slot->frame_time = 0.0;
            }
        } else if (target >= slot->duration) {
            target = slot->duration;
        }
    }
    slot->time = target;

    const uint64_t pixels = (uint64_t)(slot->width > 0 ? slot->width : 0)
                          * (uint64_t)(slot->height > 0 ? slot->height : 0);
    const bool heavy_decode = pixels >= (uint64_t)2560u * (uint64_t)1440u;
    const int max_decode_per_tick = heavy_decode ? 2 : 8;
    const auto decode_budget = std::chrono::milliseconds(heavy_decode ? 6 : 12);
    const auto decode_start = std::chrono::steady_clock::now();

    int safety = 0;
    while (slot->frame_time + 0.000001 < slot->time
        && safety < max_decode_per_tick) {
        if (!decoder_read_next(slot)) {
            bool eof = slot->decoder.ended;
            if (eof) {
                if (slot->loop && slot->duration > 0.0) {
                    slot->post_seek = true;
                    if (!decoder_seek(slot, 0.0, false)) {
                        slot->post_seek = false;
                        break;
                    }
                    slot->post_seek = false;
                    slot->frame_time = 0.0;
                    continue;
                }
                slot->ended = true;
                if (slot->duration > 0.0) slot->time = slot->duration;
                break;
            }
            /* Decoder didn't produce a frame yet (B-frame reordering delay).
             * Keep feeding more samples instead of waiting for the next tick. */
            ++safety;
            if (std::chrono::steady_clock::now() - decode_start >= decode_budget)
                break;
            continue;
        }
        slot->ended = false;
        ++safety;

        if (std::chrono::steady_clock::now() - decode_start >= decode_budget) {
            break;
        }
    }

    /* Avoid huge per-frame catch-up work on heavy clips. */
    if (safety >= max_decode_per_tick
        && slot->frame_time + 0.000001 < slot->time) {
        slot->time = slot->frame_time + frame_interval_seconds(slot);
        if (slot->duration > 0.0 && slot->time > slot->duration) {
            slot->time = slot->duration;
        }
    }

    if (slot->duration > 0.0 && !slot->loop
        && slot->time >= slot->duration - 0.000001) {
        if (slot->decoder.ended) slot->ended = true;
    }
}

void jce_video_seek(JceVideo v, double time_sec, bool exact)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    if (time_sec < 0.0) time_sec = 0.0;
    if (slot->duration > 0.0 && time_sec > slot->duration)
        time_sec = slot->duration;

    if (slot->metadata_only) {
        slot->time  = time_sec;
        slot->ended = (slot->duration > 0.0
            && time_sec >= slot->duration && !slot->loop);
        return;
    }

    slot->post_seek = true;
    if (!decoder_seek(slot, time_sec, exact)) {
        slot->time  = time_sec;
        slot->ended = false;
    }
    slot->post_seek = false;
    /* Force frame_time to match the seek target so advance() does not
     * stall trying to catch up from a stale pre-seek timestamp. */
    slot->frame_time = time_sec;
}

void jce_video_rewind(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;

    if (slot->metadata_only) {
        slot->time  = 0.0;
        slot->ended = false;
        return;
    }
    slot->post_seek = true;
    if (!decoder_seek(slot, 0.0, true)) {
        slot->time  = 0.0;
        slot->ended = false;
    }
    slot->post_seek = false;
    slot->frame_time = 0.0;
}

void jce_video_set_loop(JceVideo v, bool loop)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) return;
    slot->loop = loop;
    if (loop && slot->ended) slot->ended = false;
}

const uint8_t *jce_video_get_frame_rgba(JceVideo v,
                                        int *out_w, int *out_h,
                                        double *out_frame_time)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        if (out_frame_time) *out_frame_time = 0.0;
        return NULL;
    }
    if (out_w) *out_w = slot->rgba_w;
    if (out_h) *out_h = slot->rgba_h;
    if (out_frame_time) *out_frame_time = slot->frame_time;
    return slot->rgba;
}

uint64_t jce_video_get_frame_counter(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->frame_counter : 0u;
}

JceVideoAudioStatus jce_video_get_audio_status(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) {
        return JCE_VIDEO_AUDIO_STATUS_DEMUX_ERROR;
    }
    return slot->audio_status;
}

const char *jce_video_get_audio_status_text(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot) {
        return "invalid video handle";
    }
    return slot->audio_status_text;
}

uint32_t jce_video_get_audio_sample_count(JceVideo v)
{
    VideoSlot *slot = slot_from_handle(v);
    return slot ? slot->audio_sample_count : 0u;
}

const int16_t *jce_video_get_audio_pcm(JceVideo v,
                                        uint32_t *out_frame_count,
                                        uint32_t *out_channels,
                                        uint32_t *out_samplerate)
{
    VideoSlot *slot = slot_from_handle(v);
    if (!slot || !slot->audio_pcm) {
        if (out_frame_count) *out_frame_count = 0;
        if (out_channels)    *out_channels    = 0;
        if (out_samplerate)  *out_samplerate  = 0;
        return NULL;
    }
    if (out_frame_count) *out_frame_count = slot->audio_pcm_frames;
    if (out_channels)    *out_channels    = slot->audio_pcm_channels;
    if (out_samplerate)  *out_samplerate  = slot->audio_pcm_samplerate;
    return slot->audio_pcm;
}

} /* extern "C" */
