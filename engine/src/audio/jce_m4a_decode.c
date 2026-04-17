/*
 * jce_m4a_decode.c  Decode M4A (AAC-in-MP4) audio files to PCM.
 *
 * Detects MP4/M4A containers via the ftyp box, parses with minimp4,
 * extracts AAC audio frames, and decodes to s16 PCM using FDK-AAC.
 */

#include <jce/audio/jce_m4a_decode.h>
#include <jce/video/jce_mp4_parser.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"

#include <fdk-aac/aacdecoder_lib.h>
#include <string.h>

/* Ensure the fdk-aac build matches our expected s16 output. */
typedef char jce_m4a_pcm16_check_[(sizeof(INT_PCM) == sizeof(int16_t)) ? 1 : -1];

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

    /* ── Open FDK-AAC decoder ──────────────────────────────────── */
    HANDLE_AACDECODER aac = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!aac) {
        LOG_ERROR(LOG_TAG, "aacDecoder_Open failed");
        jce_mp4_parser_close(parser);
        return false;
    }

    UCHAR *conf_array[1];
    UINT   conf_sizes[1];
    conf_array[0] = (UCHAR *)atr.decoder_config;
    conf_sizes[0] = (UINT)atr.decoder_config_bytes;

    AAC_DECODER_ERROR err = aacDecoder_ConfigRaw(aac, conf_array, conf_sizes);
    if (err != AAC_DEC_OK) {
        LOG_ERROR(LOG_TAG, "aacDecoder_ConfigRaw failed: 0x%04x", (unsigned)err);
        aacDecoder_Close(aac);
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
        aacDecoder_Close(aac);
        jce_mp4_parser_close(parser);
        return false;
    }

    int16_t *pcm_buf = (int16_t *)JCE_MALLOC((size_t)est_bytes);
    if (!pcm_buf) {
        LOG_ERROR(LOG_TAG, "out of memory for M4A decode (%llu bytes)",
                  (unsigned long long)est_bytes);
        aacDecoder_Close(aac);
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

        /* Feed data to FDK-AAC. */
        UCHAR *in_buf[1]  = { (UCHAR *)sbuf };
        UINT   in_size[1] = { (UINT)copied };
        UINT   valid       = (UINT)copied;

        err = aacDecoder_Fill(aac, in_buf, in_size, &valid);
        if (err != AAC_DEC_OK) continue;

        uint32_t remain = pcm_cap - pcm_pos;
        err = aacDecoder_DecodeFrame(aac, (INT_PCM *)(pcm_buf + pcm_pos),
                                      (INT)remain, 0);
        if (err != AAC_DEC_OK) continue;

        CStreamInfo *si_info = aacDecoder_GetStreamInfo(aac);
        if (si_info && si_info->numChannels > 0 && si_info->sampleRate > 0) {
            actual_ch = (uint32_t)si_info->numChannels;
            actual_sr = (uint32_t)si_info->sampleRate;
            uint32_t written = (uint32_t)si_info->frameSize * actual_ch;
            pcm_pos += written;
        }
    }

    JCE_FREE(sbuf);
    aacDecoder_Close(aac);
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
