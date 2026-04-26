/*
 * jce_miniaudio_opus_backend.c
 *
 * Implements an Ogg-Opus backend for miniaudio.  The whole stream is
 * decoded to s16 PCM upfront in onInitMemory(); the resulting buffer
 * is then served through a minimal ma_data_source whose vtable
 * forwards to ma_audio_buffer_ref helpers.
 *
 * Why upfront?  miniaudio uses the backend for ma_decoder which we
 * always drive with ma_decoder_init_memory + read-all (see
 * jce_audio.c).  Streaming Opus would buy nothing here and triples
 * the code size.
 *
 * Container parsing follows RFC 7845 (Ogg encapsulation of Opus):
 *   page 0  : OpusHead   (channel count, pre-skip)
 *   page 1  : OpusTags   (ignored)
 *   page 2+ : audio packets, decoded with opus_decode()
 */

#include "jce_miniaudio_opus_backend.h"

#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <ogg/ogg.h>
#include <opus.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG          "jce_ma_opus"
#define OPUS_RATE        48000
#define OPUS_MAX_FRAME   5760  /* 120 ms @ 48 kHz, max Opus frame size */

typedef struct {
    ma_data_source_base ds;          /* MUST be first. */
    ma_audio_buffer_ref ref;          /* zero-copy view over pcm. */
    int16_t            *pcm;          /* owned PCM buffer. */
    ma_uint64           frame_count;
    ma_uint32           channels;
    ma_uint32           sample_rate;
    ma_allocation_callbacks alloc;
} jce_ma_opus;

/* ── ma_data_source vtable: forward to ma_audio_buffer_ref ─────── */

static ma_result jce_opus_ds_read(ma_data_source *p,
                                  void *frames_out,
                                  ma_uint64 frame_count,
                                  ma_uint64 *frames_read)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    ma_uint64 r = ma_audio_buffer_ref_read_pcm_frames(
        &self->ref, frames_out, frame_count, MA_FALSE);
    if (frames_read) *frames_read = r;
    return (r == 0 && frame_count > 0) ? MA_AT_END : MA_SUCCESS;
}

static ma_result jce_opus_ds_seek(ma_data_source *p, ma_uint64 frame_index)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    return ma_audio_buffer_ref_seek_to_pcm_frame(&self->ref, frame_index);
}

static ma_result jce_opus_ds_get_data_format(ma_data_source *p,
                                             ma_format *format,
                                             ma_uint32 *channels,
                                             ma_uint32 *sample_rate,
                                             ma_channel *channel_map,
                                             size_t channel_map_cap)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    if (format)      *format      = ma_format_s16;
    if (channels)    *channels    = self->channels;
    if (sample_rate) *sample_rate = self->sample_rate;
    if (channel_map && channel_map_cap > 0) {
        ma_channel_map_init_standard(ma_standard_channel_map_default,
                                     channel_map, channel_map_cap,
                                     self->channels);
    }
    return MA_SUCCESS;
}

static ma_result jce_opus_ds_get_cursor(ma_data_source *p, ma_uint64 *cursor)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    return ma_audio_buffer_ref_get_cursor_in_pcm_frames(&self->ref, cursor);
}

static ma_result jce_opus_ds_get_length(ma_data_source *p, ma_uint64 *length)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    return ma_audio_buffer_ref_get_length_in_pcm_frames(&self->ref, length);
}

static const ma_data_source_vtable g_jce_opus_ds_vtable = {
    jce_opus_ds_read,
    jce_opus_ds_seek,
    jce_opus_ds_get_data_format,
    jce_opus_ds_get_cursor,
    jce_opus_ds_get_length,
    NULL,                           /* onSetLooping */
    0                               /* flags */
};

/* ── Decoder: Ogg-Opus → s16 PCM (in memory) ────────────────────── */

static int16_t *grow_pcm(int16_t *pcm, size_t *cap, size_t needed,
                         const ma_allocation_callbacks *alloc)
{
    size_t c = *cap ? *cap : 4096;
    while (c < needed) c *= 2;
    int16_t *n = (int16_t *)ma_realloc(pcm, c * sizeof(int16_t), alloc);
    if (!n) return NULL;
    *cap = c;
    return n;
}

static ma_result decode_ogg_opus(const void *data, size_t size,
                                 const ma_allocation_callbacks *alloc,
                                 int16_t **out_pcm,
                                 ma_uint64 *out_frames,
                                 ma_uint32 *out_channels)
{
    ogg_sync_state   oy;
    ogg_stream_state os;
    ogg_page         og;
    ogg_packet       op;
    bool stream_inited = false;
    OpusDecoder *dec = NULL;
    int16_t *pcm = NULL;
    size_t   pcm_cap = 0, pcm_used = 0;
    int      channels = 0;
    uint16_t pre_skip = 0;
    ma_result rc = MA_INVALID_FILE;

    ogg_sync_init(&oy);
    char *buf = ogg_sync_buffer(&oy, (long)size);
    if (!buf) goto done;
    memcpy(buf, data, size);
    if (ogg_sync_wrote(&oy, (long)size) != 0) goto done;

    int packet_idx = 0;
    while (ogg_sync_pageout(&oy, &og) == 1) {
        if (!stream_inited) {
            ogg_stream_init(&os, ogg_page_serialno(&og));
            stream_inited = true;
        }
        if (ogg_stream_pagein(&os, &og) < 0) goto done;

        while (ogg_stream_packetout(&os, &op) == 1) {
            if (packet_idx == 0) {
                /* OpusHead: 'OpusHead'(8)+ver(1)+ch(1)+preskip(2)+... */
                if (op.bytes < 19 || memcmp(op.packet, "OpusHead", 8) != 0)
                    goto done;
                channels = op.packet[9];
                pre_skip = (uint16_t)op.packet[10]
                         | ((uint16_t)op.packet[11] << 8);
                if (channels < 1 || channels > 2) goto done;
                int err = 0;
                dec = opus_decoder_create(OPUS_RATE, channels, &err);
                if (!dec || err != OPUS_OK) goto done;
            } else if (packet_idx == 1) {
                /* OpusTags — ignore. */
            } else {
                size_t need = pcm_used
                            + (size_t)OPUS_MAX_FRAME * (size_t)channels;
                if (need > pcm_cap) {
                    int16_t *n = grow_pcm(pcm, &pcm_cap, need, alloc);
                    if (!n) { rc = MA_OUT_OF_MEMORY; goto done; }
                    pcm = n;
                }
                int decoded = opus_decode(dec, op.packet,
                                          (opus_int32)op.bytes,
                                          pcm + pcm_used,
                                          OPUS_MAX_FRAME, 0);
                if (decoded < 0) goto done;
                pcm_used += (size_t)decoded * (size_t)channels;
            }
            packet_idx++;
        }
    }

    if (!dec || pcm_used == 0) goto done;

    /* Trim pre-skip samples (RFC 7845 §4.2). */
    size_t skip = (size_t)pre_skip * (size_t)channels;
    if (skip > pcm_used) skip = pcm_used;
    if (skip > 0) {
        memmove(pcm, pcm + skip, (pcm_used - skip) * sizeof(int16_t));
        pcm_used -= skip;
    }

    *out_pcm      = pcm;
    *out_frames   = (ma_uint64)(pcm_used / (size_t)channels);
    *out_channels = (ma_uint32)channels;
    pcm = NULL;       /* transferred to caller */
    rc = MA_SUCCESS;

done:
    if (stream_inited) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    if (dec) opus_decoder_destroy(dec);
    if (pcm) ma_free(pcm, alloc);
    return rc;
}

/* ── ma_decoding_backend_vtable hooks ───────────────────────────── */

static ma_result on_init_memory(void *user_data,
                                const void *data, size_t data_size,
                                const ma_decoding_backend_config *cfg,
                                const ma_allocation_callbacks *alloc,
                                ma_data_source **out_backend)
{
    (void)user_data; (void)cfg;

    if (!data || data_size < 36) return MA_INVALID_ARGS;

    /* Magic: "OggS" + an "OpusHead" within the first page. */
    const uint8_t *p = (const uint8_t *)data;
    if (p[0] != 'O' || p[1] != 'g' || p[2] != 'g' || p[3] != 'S')
        return MA_INVALID_FILE;
    bool found = false;
    for (size_t i = 28; i + 8 <= data_size && i < 80; ++i) {
        if (memcmp(p + i, "OpusHead", 8) == 0) { found = true; break; }
    }
    if (!found) return MA_INVALID_FILE;

    jce_ma_opus *self = (jce_ma_opus *)ma_malloc(sizeof(*self), alloc);
    if (!self) return MA_OUT_OF_MEMORY;
    memset(self, 0, sizeof(*self));
    self->alloc = *alloc;

    int16_t  *pcm   = NULL;
    ma_uint64 frames = 0;
    ma_uint32 ch     = 0;
    ma_result rc = decode_ogg_opus(data, data_size, alloc,
                                    &pcm, &frames, &ch);
    if (rc != MA_SUCCESS) {
        ma_free(self, alloc);
        LOG_WARN(LOG_TAG, "decode failed (rc=%d, size=%zu)",
                 (int)rc, data_size);
        return rc;
    }

    self->pcm         = pcm;
    self->frame_count = frames;
    self->channels    = ch;
    self->sample_rate = OPUS_RATE;

    if (ma_audio_buffer_ref_init(ma_format_s16, ch, pcm, frames,
                                 &self->ref) != MA_SUCCESS) {
        ma_free(pcm, alloc);
        ma_free(self, alloc);
        return MA_INVALID_OPERATION;
    }

    ma_data_source_config ds_cfg = ma_data_source_config_init();
    ds_cfg.vtable = &g_jce_opus_ds_vtable;
    if (ma_data_source_init(&ds_cfg, &self->ds) != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&self->ref);
        ma_free(pcm, alloc);
        ma_free(self, alloc);
        return MA_INVALID_OPERATION;
    }

    *out_backend = (ma_data_source *)self;
    return MA_SUCCESS;
}

static void on_uninit(void *user_data,
                      ma_data_source *backend,
                      const ma_allocation_callbacks *alloc)
{
    (void)user_data;
    if (!backend) return;
    jce_ma_opus *self = (jce_ma_opus *)backend;
    ma_data_source_uninit(&self->ds);
    ma_audio_buffer_ref_uninit(&self->ref);
    if (self->pcm) ma_free(self->pcm, alloc);
    ma_free(self, alloc);
}

const ma_decoding_backend_vtable g_jce_ma_opus_backend_vtable = {
    NULL,             /* onInit       (file/stream — we do upfront in-memory) */
    NULL,             /* onInitFile   */
    NULL,             /* onInitFileW  */
    on_init_memory,   /* onInitMemory */
    on_uninit,
};
