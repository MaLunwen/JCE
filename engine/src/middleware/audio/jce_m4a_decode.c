/*
 * jce_m4a_decode.c  Decode M4A (AAC-in-MP4) audio files to PCM.
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │ ⚠ PATENT NOTICE — AAC (MPEG-4 Audio)                        │
 * │ AAC is covered by patents in the Via Licensing AAC pool.    │
 * │ Distributing binaries that decode/encode AAC may require    │
 * │ patent licenses in many jurisdictions.                      │
 * │                                                             │
 * │ Prefer the royalty-free Opus path (miniaudio Opus backend) │
 * │ for NEW assets. This loader is retained ONLY to import legacy │
 * │ M4A/AAC content and SHOULD NOT be the default cooker output. │
 * └─────────────────────────────────────────────────────────────┘
 *
 * Detects MP4/M4A containers via the ftyp box, parses with minimp4,
 * extracts AAC audio frames, and decodes to s16 PCM.
 *
 * This TU owns the *container* job only; the raw AAC access-unit decode is
 * delegated to the single fdk-aac adapter in middleware/video so that the
 * video player and this loader share one wrapper (and one patent gate).
 */

#include <jce/middleware/audio/jce_m4a_decode.h>

#ifdef JCE_ENABLE_PATENTED_CODECS

#include <jce/middleware/video/jce_mp4_parser.h>
#include <jce/os/core/jce_log.h>

#include "middleware/video/jce_aac_decode.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "jce_m4a"

bool jce_m4a_is_mp4_container(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    /* MP4/M4A files start with a box whose type (at offset 4) is 'ftyp'. */
    if (!p || size < 8) return false;
    return p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p';
}

bool jce_m4a_decode_to_pcm(const void *data, size_t size,
                            int16_t **out_pcm,
                            uint32_t *out_frames,
                            uint32_t *out_channels,
                            uint32_t *out_samplerate)
{
    if (!data || size == 0 || !out_pcm || !out_frames
        || !out_channels || !out_samplerate) {
        return false;
    }

    *out_pcm        = NULL;
    *out_frames     = 0;
    *out_channels   = 0;
    *out_samplerate = 0;

    /* ── Parse MP4 container ───────────────────────────────────── */
    JceMp4Info info;
    JceMp4Parser *parser = jce_mp4_parser_open_memory(data, size, &info);
    if (!parser) {
        LOG_WARN(LOG_TAG, "failed to parse MP4 container (%zu bytes)", size);
        return false;
    }

    if (!info.has_audio_track) {
        LOG_WARN(LOG_TAG, "MP4 has no audio track");
        jce_mp4_parser_close(parser);
        return false;
    }

    /* ── Get audio track info ──────────────────────────────────── */
    JceMp4AudioTrackInfo atr;
    if (!jce_mp4_parser_get_audio_track_info(parser, &atr)) {
        LOG_WARN(LOG_TAG, "failed to get audio track info");
        jce_mp4_parser_close(parser);
        return false;
    }

    if (strcmp(atr.codec, "mp4a") != 0) {
        LOG_WARN(LOG_TAG, "unsupported audio codec in M4A: '%s'", atr.codec);
        jce_mp4_parser_close(parser);
        return false;
    }

    if (!atr.decoder_config || atr.decoder_config_bytes == 0) {
        LOG_WARN(LOG_TAG, "AAC decoder config (ASC) missing in M4A");
        jce_mp4_parser_close(parser);
        return false;
    }

    LOG_INFO(LOG_TAG, "M4A audio: %u samples, %u Hz, %uch, codec=%s",
             atr.sample_count, atr.samplerate_hz, atr.channels, atr.codec);

    /* ── Open the shared AAC adapter (ASC from the esds box) ───── */
    JceAacDecoder *aac = jce_aac_decoder_open(atr.decoder_config,
                                              atr.decoder_config_bytes);
    if (!aac) {
        LOG_ERROR(LOG_TAG, "failed to open AAC decoder for M4A track");
        jce_mp4_parser_close(parser);
        return false;
    }

    /* ── Decode all AAC frames to PCM ──────────────────────────── */
    uint32_t ch_est      = atr.channels > 0 ? atr.channels : 2u;
    uint32_t max_frame   = 2048u; /* AAC frame size, covers SBR */
    uint64_t est_samples = (uint64_t)atr.sample_count * max_frame * ch_est;
    uint64_t est_bytes   = est_samples * sizeof(int16_t);

    /* Safety: limit to ~500 MB of PCM to prevent OOM. */
    if (est_bytes > (uint64_t)500u * 1024u * 1024u) {
        LOG_WARN(LOG_TAG, "M4A audio too large (%llu est bytes), skipping",
                 (unsigned long long)est_bytes);
        jce_aac_decoder_close(aac);
        jce_mp4_parser_close(parser);
        return false;
    }

    int16_t *pcm_buf = (int16_t *)JCE_MALLOC((size_t)est_bytes);
    if (!pcm_buf) {
        LOG_ERROR(LOG_TAG, "out of memory for M4A decode (%llu bytes)",
                  (unsigned long long)est_bytes);
        jce_aac_decoder_close(aac);
        jce_mp4_parser_close(parser);
        return false;
    }

    uint32_t pcm_pos = 0;
    uint32_t pcm_cap = (uint32_t)est_samples;
    uint8_t *sbuf     = NULL;
    uint32_t sbuf_cap = 0;
    bool     ok       = true;
    uint32_t actual_ch = 0;
    uint32_t actual_sr = 0;

    for (uint32_t si = 0; si < atr.sample_count; ++si) {
        JceMp4SampleInfo sinfo;
        if (!jce_mp4_parser_get_audio_sample(parser, si, &sinfo)) {
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
        if (!jce_mp4_parser_copy_audio_sample(parser, si, sbuf,
                                               sbuf_cap, &copied)) {
            ok = false;
            break;
        }

        /* A single undecodable access unit must not abort the whole track. */
        uint32_t remain  = pcm_cap - pcm_pos;
        uint32_t written = 0;
        if (!jce_aac_decode_frame(aac, sbuf, copied,
                                  pcm_buf + pcm_pos, remain, &written)) {
            continue;
        }

        uint32_t ch = jce_aac_decoder_get_channels(aac);
        uint32_t sr = jce_aac_decoder_get_samplerate(aac);
        if (ch > 0 && sr > 0) {
            actual_ch = ch;
            actual_sr = sr;
            pcm_pos  += written;
        }
    }

    JCE_FREE(sbuf);
    jce_aac_decoder_close(aac);
    jce_mp4_parser_close(parser);

    if (!ok || pcm_pos == 0 || actual_ch == 0 || actual_sr == 0) {
        LOG_WARN(LOG_TAG, "M4A decode failed or produced no PCM (ok=%d, pos=%u)",
                 ok, pcm_pos);
        JCE_FREE(pcm_buf);
        return false;
    }

    /* Trim buffer to actual size. */
    uint32_t actual_frames = pcm_pos / actual_ch;
    size_t   actual_bytes  = (size_t)pcm_pos * sizeof(int16_t);
    int16_t *trimmed = (int16_t *)JCE_REALLOC(pcm_buf, actual_bytes);
    if (trimmed) pcm_buf = trimmed;

    *out_pcm        = pcm_buf;
    *out_frames     = actual_frames;
    *out_channels   = actual_ch;
    *out_samplerate = actual_sr;

    LOG_SUCCESS(LOG_TAG, "M4A decoded: %u frames, %u Hz, %uch",
                actual_frames, actual_sr, actual_ch);
    return true;
}

#else /* !JCE_ENABLE_PATENTED_CODECS */

#include <string.h>

bool jce_m4a_is_mp4_container(const void *data, size_t size)
{
    /* Cheap byte sniff stays available even without fdk-aac, so callers
     * can keep their detection logic — they just won't decode. */
    const unsigned char *p = (const unsigned char *)data;
    if (!p || size < 12u) return false;
    return p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p';
}

bool jce_m4a_decode_to_pcm(const void *data, size_t size,
                            int16_t **out_pcm,
                            uint32_t *out_frames,
                            uint32_t *out_channels,
                            uint32_t *out_samplerate)
{
    (void)data; (void)size;
    if (out_pcm)        *out_pcm        = (int16_t *)0;
    if (out_frames)     *out_frames     = 0u;
    if (out_channels)   *out_channels   = 0u;
    if (out_samplerate) *out_samplerate = 0u;
    return false;
}

#endif /* JCE_ENABLE_PATENTED_CODECS */
