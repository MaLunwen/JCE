/*
 * jce_audio_decode.c  Encoded bytes -> interleaved s16 PCM.
 *
 * Split out of jce_audio.c when that file passed AGENTS.md §11's 3000-line
 * cap.  Nothing here reads or writes JceAudio: the input is a byte range and
 * the output is a buffer the caller owns, which is why this can run on a
 * worker thread while the audio device keeps mixing.
 *
 * See jce_audio_decode.h for the contract.
 */

#include "middleware/audio/jce_audio_decode.h"

#ifndef JCE_NO_AUDIO

#include <jce/middleware/audio/jce_m4a_decode.h>
#include <jce/os/core/jce_log.h>

#include "jce_miniaudio_opus_backend.h"
/* The single fdk-aac wrapper lives in middleware/video; jce_audio links
 * jce_video PRIVATE, and jce_m4a_decode.c already reaches it the same way. */
#include "middleware/video/jce_aac_decode.h"
#include "os/core/jce_memory.h"

#include <miniaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decode a raw ADTS (.aac) stream to interleaved s16 PCM.  Grows the output
 * geometrically because ADTS carries no sample count anywhere — the only way
 * to know the length is to decode it. */
static bool jce_adts_decode_to_pcm(const uint8_t *data, size_t size,
                                   int16_t **out_pcm, ma_uint64 *out_frames,
                                   ma_uint32 *out_channels, ma_uint32 *out_rate,
                                   const char *path)
{
    JceAacDecoder *dec = jce_aac_decoder_open_adts();
    int16_t *pcm = NULL;
    size_t cap = 0u, pos = 0u;          /* in samples (int16 units) */
    size_t cursor = 0u;
    uint32_t ch = 0u, sr = 0u;
    bool ok = false;

    if (!dec) {
        return false;                    /* royalty-free build: stub returns NULL */
    }
    /* 1024 samples/frame * 8ch headroom, doubled as needed. */
    cap = 8192u;
    pcm = (int16_t *)JCE_MALLOC(cap * sizeof(int16_t));
    if (!pcm) {
        jce_aac_decoder_close(dec);
        return false;
    }

    /* Drain-then-feed. Fill() swallows several frames at a time and
     * DecodeFrame() emits one, so anything that feeds and decodes in lockstep
     * strands the remainder inside the decoder and loses the tail. Emptying
     * the decoder before every refill — and once more after the input runs
     * out — is what makes the frame count come out right. */
    for (;;) {
        uint32_t written = 0u;
        uint32_t consumed;

        if (cap - pos < 8192u) {
            size_t grown = cap * 2u;
            int16_t *bigger;
            if (grown > (size_t)512u * 1024u * 1024u / sizeof(int16_t)) {
                LOG_WARN("jce_audio", "ADTS stream too large: '%s'", path);
                break;
            }
            bigger = (int16_t *)JCE_REALLOC(pcm, grown * sizeof(int16_t));
            if (!bigger) break;
            pcm = bigger;
            cap = grown;
        }

        if (jce_aac_adts_decode(dec, pcm + pos, (uint32_t)(cap - pos),
                                &written)) {
            pos += written;
            if (written > 0u) {
                ch = jce_aac_decoder_get_channels(dec);
                sr = jce_aac_decoder_get_samplerate(dec);
            }
            continue;                    /* keep draining */
        }

        if (cursor >= size) {
            break;                       /* drained and no input left */
        }
        consumed = jce_aac_adts_feed(dec, data + cursor,
                                     (uint32_t)(size - cursor));
        if (consumed == 0u) {
            break;                       /* cannot make progress */
        }
        cursor += consumed;
    }

    jce_aac_decoder_close(dec);

    if (pos > 0u && ch > 0u && sr > 0u) {
        *out_pcm = pcm;
        *out_frames = (ma_uint64)(pos / ch);
        *out_channels = ch;
        *out_rate = sr;
        LOG_DEBUG("jce_audio", "decoded ADTS '%s' (%u Hz, %uch, %llu frames)",
                  path, sr, ch, (unsigned long long)(pos / ch));
        ok = true;
    } else {
        JCE_FREE(pcm);
    }
    return ok;
}

/* See jce_audio_decode.h for the contract. */
bool jce_audio_decode_pcm_mem(const uint8_t *data, size_t size, const char *path,
                           int16_t **out_pcm, ma_uint64 *out_frames,
                           ma_uint32 *out_channels, ma_uint32 *out_rate)
{
    *out_pcm = NULL; *out_frames = 0; *out_channels = 0; *out_rate = 0;

    /* ── M4A / AAC-in-MP4 detection ─────────────────────────────── */
    if (jce_m4a_is_mp4_container(data, size)) {
        int16_t *pcm = NULL;
        uint32_t frames = 0, ch = 0, sr = 0;
        if (jce_m4a_decode_to_pcm(data, size, &pcm, &frames, &ch, &sr)) {
            *out_pcm = pcm; *out_frames = (ma_uint64)frames;
            *out_channels = ch; *out_rate = sr;
            LOG_DEBUG("jce_audio", "decoded M4A '%s' (%u Hz, %uch, %u frames)",
                      path, sr, ch, frames);
            return true;
        }
        LOG_WARN("jce_audio", "M4A decode failed for '%s', trying miniaudio", path);
        /* Fall through to miniaudio as last resort. */
    }

    /* ── Raw ADTS (.aac) ────────────────────────────────────────── *
     * A bare .aac file is a stream of ADTS frames with no container. It
     * cannot go to miniaudio: the MPEG sync-word sniff below matches ADTS
     * too and hints ma_encoding_format_mp3, so dr_mp3 gets handed AAC
     * payload, fails, and the auto-detect retry fails as well — the file
     * reports "cannot decode" in a build that has fdk-aac linked in.
     * fdk-aac reads ADTS natively; route it there.
     *
     * Syncword is 12 bits of 1s followed by ID(1) and layer(2). Layer MUST
     * be 00 for ADTS, which is exactly what separates it from MP3/MP2/MP1
     * (they use layer 01/10/11), so test 0xFF / 0xF6 -> 0xF0 rather than the
     * looser 0xE0 mask. */
    if (size >= 7u && data[0] == 0xFFu && (data[1] & 0xF6u) == 0xF0u) {
        if (jce_adts_decode_to_pcm(data, size, out_pcm, out_frames,
                                   out_channels, out_rate, path)) {
            return true;
        }
        LOG_WARN("jce_audio", "ADTS decode failed for '%s', trying miniaudio",
                 path);
    }

    ma_decoder_config cfg = ma_decoder_config_init(
        ma_format_s16, 0 /* auto channels */, 0 /* auto sample rate */);
    ma_decoder decoder;

    /* Wire our Opus custom backend so .opus / Ogg-Opus is handled
       transparently (miniaudio probes custom backends before
       built-ins, so an Ogg page carrying OpusHead routes here). */
    static const ma_decoding_backend_vtable *jce_custom_backends[] = {
        &g_jce_ma_opus_backend_vtable,
    };
    cfg.ppCustomBackendVTables = (ma_decoding_backend_vtable **)jce_custom_backends;
    cfg.customBackendCount     = (ma_uint32)(sizeof(jce_custom_backends)
                                            / sizeof(jce_custom_backends[0]));

    /* Hint the encoding format from magic bytes so miniaudio picks
       the correct built-in decoder (dr_mp3, dr_wav, dr_flac, stb_vorbis).
       Note: we do NOT hint Ogg-Opus as vorbis — miniaudio's custom-backend
       probe phase (which runs first) already routes OpusHead pages to
       g_jce_ma_opus_backend_vtable. Plain Ogg-Vorbis still falls through. */
    if (size >= 4 && data[0] == 'O' && data[1] == 'g'
                  && data[2] == 'g' && data[3] == 'S') {
        /* Sniff for OpusHead — if present, leave format unknown so the
           custom backend wins; otherwise hint vorbis. */
        bool is_opus = false;
        for (size_t i = 28; i + 8 <= size && i < 80; ++i) {
            if (data[i] == 'O' && memcmp(data + i, "OpusHead", 8) == 0) {
                is_opus = true; break;
            }
        }
        if (!is_opus) cfg.encodingFormat = ma_encoding_format_vorbis;
    } else if (size >= 4 && data[0] == 'f' && data[1] == 'L'
                         && data[2] == 'a' && data[3] == 'C') {
        cfg.encodingFormat = ma_encoding_format_flac;
    } else if (size >= 4 && data[0] == 'R' && data[1] == 'I'
                         && data[2] == 'F' && data[3] == 'F') {
        cfg.encodingFormat = ma_encoding_format_wav;
    } else if (size >= 3 && data[0] == 'I' && data[1] == 'D'
                         && data[2] == '3') {
        /* ID3v2 tag header — almost always an MP3 file. */
        cfg.encodingFormat = ma_encoding_format_mp3;
    } else if (size >= 2 && data[0] == 0xFF
               && (data[1] & 0xE0) == 0xE0) {
        /* MPEG audio sync word (0xFFE0+): MP3 / MP2 / MP1. */
        cfg.encodingFormat = ma_encoding_format_mp3;
    }

    ma_result res = ma_decoder_init_memory(data, size, &cfg, &decoder);

    /* If the hinted format failed, retry with auto-detection. */
    if (res != MA_SUCCESS && cfg.encodingFormat != ma_encoding_format_unknown) {
        cfg.encodingFormat = ma_encoding_format_unknown;
        res = ma_decoder_init_memory(data, size, &cfg, &decoder);
    }
    if (res != MA_SUCCESS) {
        /* Log the first bytes to help diagnose unsupported files. */
        char hdr[48] = {0};
        size_t hlen = size < 16 ? size : 16;
        for (size_t i = 0; i < hlen; ++i) {
            snprintf(hdr + i * 3, sizeof(hdr) - i * 3, "%02X ", data[i]);
        }
        LOG_ERROR("jce_audio",
            "decode failed for '%s' (ma_result=%d, size=%zu, header=[%s])",
            path, (int)res, size, hdr);
        return false;
    }

    /* Get total frame count. */
    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 rate     = decoder.outputSampleRate;
    int16_t  *pcm      = NULL;

    if (total_frames == 0) {
        /* Unknown length (streaming format) — decode in chunks. */
        size_t alloc_frames = 1024 * 256;
        size_t used_frames  = 0;
        pcm = (int16_t *)JCE_MALLOC(alloc_frames * channels * sizeof(int16_t));
        if (!pcm) {
            ma_decoder_uninit(&decoder);
            return false;
        }

        for (;;) {
            if (used_frames + 4096 > alloc_frames) {
                alloc_frames *= 2;
                int16_t *tmp = (int16_t *)JCE_REALLOC(pcm,
                    alloc_frames * channels * sizeof(int16_t));
                if (!tmp) {
                    JCE_FREE(pcm);
                    ma_decoder_uninit(&decoder);
                    return false;
                }
                pcm = tmp;
            }
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&decoder, pcm + used_frames * channels,
                                        4096, &read);
            if (read == 0) break;
            used_frames += (size_t)read;
        }

        total_frames = (ma_uint64)used_frames;
    } else {
        /* Known length — single allocation. */
        pcm = (int16_t *)JCE_MALLOC((size_t)(total_frames * channels * sizeof(int16_t)));
        if (!pcm) {
            ma_decoder_uninit(&decoder);
            return false;
        }

        ma_uint64 frames_read = 0;
        ma_decoder_read_pcm_frames(&decoder, pcm, total_frames, &frames_read);
        total_frames = frames_read;
    }

    ma_decoder_uninit(&decoder);

    *out_pcm      = pcm;
    *out_frames   = total_frames;
    *out_channels = channels;
    *out_rate     = rate;
    LOG_DEBUG("jce_audio", "decoded '%s' (%u Hz, %uch, %llu frames)",
              path, rate, channels, (unsigned long long)total_frames);
    return true;
}

#else  /* JCE_NO_AUDIO */

/* ISO C forbids an empty translation unit, and this whole file is one
 * decoder: with audio compiled out there is nothing to decode. */
typedef int jce_audio_decode_disabled_tu;

#endif /* !JCE_NO_AUDIO */
