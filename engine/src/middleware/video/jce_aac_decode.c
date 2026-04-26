/*
 * jce_aac_decode.c  AAC-LC decoder adapter using libfdk_aac.
 *
 * ⚠ PATENT NOTICE — see jce_m4a_decode.c. AAC is patent-encumbered.
 * Prefer Opus (miniaudio Opus backend) for new assets.
 *
 * Wraps the Fraunhofer FDK AAC decoder with a minimal C API.
 * Accepts one access unit at a time and outputs interleaved s16 PCM.
 *
 * The full implementation is gated on JCE_ENABLE_PATENTED_CODECS.
 * When the option is OFF (default), this file provides stubs that
 * always fail-open — the runtime gracefully degrades to "unsupported
 * codec" instead of pulling in libfdk_aac.
 */

#include "jce_aac_decode.h"

#ifdef JCE_ENABLE_PATENTED_CODECS

#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <aacdecoder_lib.h>
#include <string.h>

/* Ensure the fdk-aac build matches our expected s16 output. */
typedef char jce_aac_pcm16_check_[(sizeof(INT_PCM) == sizeof(int16_t)) ? 1 : -1];

#define LOG_TAG "jce_aac"

struct JceAacDecoder {
    HANDLE_AACDECODER handle;
    uint32_t channels;
    uint32_t samplerate;
    uint32_t frame_size;
    bool     info_valid;
};

JceAacDecoder *jce_aac_decoder_open(const void *asc_config, uint32_t asc_bytes)
{
    if (!asc_config || asc_bytes == 0) return NULL;

    HANDLE_AACDECODER handle = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!handle) {
        LOG_ERROR(LOG_TAG, "aacDecoder_Open failed");
        return NULL;
    }

    UCHAR *conf_array[1];
    UINT   conf_sizes[1];
    conf_array[0] = (UCHAR *)asc_config;
    conf_sizes[0] = (UINT)asc_bytes;

    AAC_DECODER_ERROR err = aacDecoder_ConfigRaw(handle, conf_array, conf_sizes);
    if (err != AAC_DEC_OK) {
        LOG_ERROR(LOG_TAG, "aacDecoder_ConfigRaw failed: 0x%04x", (unsigned)err);
        aacDecoder_Close(handle);
        return NULL;
    }

    JceAacDecoder *dec = (JceAacDecoder *)JCE_CALLOC(1, sizeof(*dec));
    if (!dec) {
        aacDecoder_Close(handle);
        return NULL;
    }

    dec->handle = handle;
    return dec;
}

bool jce_aac_decode_frame(JceAacDecoder *dec,
                          const void *aac_frame, uint32_t frame_bytes,
                          int16_t *out_pcm, uint32_t out_capacity,
                          uint32_t *out_samples)
{
    if (!dec || !dec->handle || !aac_frame || frame_bytes == 0 || !out_pcm)
        return false;

    UCHAR *in_buf[1];
    UINT   in_size[1];
    UINT   valid;

    in_buf[0]  = (UCHAR *)aac_frame;
    in_size[0] = (UINT)frame_bytes;
    valid      = (UINT)frame_bytes;

    AAC_DECODER_ERROR err = aacDecoder_Fill(dec->handle, in_buf, in_size, &valid);
    if (err != AAC_DEC_OK) return false;

    err = aacDecoder_DecodeFrame(dec->handle, (INT_PCM *)out_pcm,
                                 (INT)out_capacity, 0);
    if (err != AAC_DEC_OK) return false;

    CStreamInfo *info = aacDecoder_GetStreamInfo(dec->handle);
    if (info && info->numChannels > 0 && info->sampleRate > 0) {
        dec->channels   = (uint32_t)info->numChannels;
        dec->samplerate = (uint32_t)info->sampleRate;
        dec->frame_size = (uint32_t)info->frameSize;
        dec->info_valid = true;

        uint32_t written = (uint32_t)info->frameSize * (uint32_t)info->numChannels;
        if (out_samples) *out_samples = written;
    } else {
        if (out_samples) *out_samples = 0;
    }

    return true;
}

uint32_t jce_aac_decoder_get_channels(const JceAacDecoder *dec)
{
    return dec ? dec->channels : 0;
}

uint32_t jce_aac_decoder_get_samplerate(const JceAacDecoder *dec)
{
    return dec ? dec->samplerate : 0;
}

uint32_t jce_aac_decoder_get_frame_size(const JceAacDecoder *dec)
{
    return dec ? dec->frame_size : 0;
}

void jce_aac_decoder_close(JceAacDecoder *dec)
{
    if (!dec) return;
    if (dec->handle) aacDecoder_Close(dec->handle);
    JCE_FREE(dec);
}

#else /* !JCE_ENABLE_PATENTED_CODECS */

/* Royalty-free build: stubs that fail-open. Callers fall through to
 * "unsupported codec" status without pulling in libfdk_aac. */
JceAacDecoder *jce_aac_decoder_open(const void *asc_config, uint32_t asc_bytes)
{ (void)asc_config; (void)asc_bytes; return (JceAacDecoder *)0; }

bool jce_aac_decode_frame(JceAacDecoder *dec,
                          const void *aac_frame, uint32_t frame_bytes,
                          int16_t *out_pcm, uint32_t out_capacity,
                          uint32_t *out_samples)
{
    (void)dec; (void)aac_frame; (void)frame_bytes;
    (void)out_pcm; (void)out_capacity;
    if (out_samples) *out_samples = 0u;
    return false;
}

uint32_t jce_aac_decoder_get_channels(const JceAacDecoder *dec) { (void)dec; return 0u; }
uint32_t jce_aac_decoder_get_samplerate(const JceAacDecoder *dec) { (void)dec; return 0u; }
uint32_t jce_aac_decoder_get_frame_size(const JceAacDecoder *dec) { (void)dec; return 0u; }

void jce_aac_decoder_close(JceAacDecoder *dec) { (void)dec; }

#endif /* JCE_ENABLE_PATENTED_CODECS */
