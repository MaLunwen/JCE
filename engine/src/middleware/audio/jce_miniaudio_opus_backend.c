/* JCE's incremental Ogg-Opus adapter for miniaudio file/memory sources.
 * No full encoded input, full PCM, or third-party source modification. */

#include "jce_miniaudio_opus_backend.h"

#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include "jce_ogg_opus.h"
#include <stdlib.h>
#include <string.h>

#define LOG_TAG          "jce_ma_opus"
#define OPUS_RATE        48000
#define OPUS_MAX_FRAME   5760  /* 120 ms @ 48 kHz, max Opus frame size */

typedef struct {
    ma_data_source_base ds;
    JceOggOpus *opus;
    ma_uint64 frame_count;
    ma_uint32 channels, sample_rate;
} jce_ma_opus;

/* ── ma_data_source vtable: forward to ma_audio_buffer_ref ─────── */

static ma_result jce_opus_ds_read(ma_data_source *p,
                                  void *frames_out,
                                  ma_uint64 frame_count,
                                  ma_uint64 *frames_read)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    ma_uint64 r = jce_ogg_opus_read(self->opus,(int16_t *)frames_out,
                                    frame_count>UINT32_MAX?UINT32_MAX:(uint32_t)frame_count);
    if (frames_read) *frames_read = r;
    return (r == 0 && frame_count > 0) ? MA_AT_END : MA_SUCCESS;
}

static ma_result jce_opus_ds_seek(ma_data_source *p, ma_uint64 frame_index)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    return jce_ogg_opus_seek(self->opus,frame_index)?MA_SUCCESS:MA_INVALID_OPERATION;
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
    if (cursor) *cursor=jce_ogg_opus_cursor(self->opus);
    return MA_SUCCESS;
}

static ma_result jce_opus_ds_get_length(ma_data_source *p, ma_uint64 *length)
{
    jce_ma_opus *self = (jce_ma_opus *)p;
    if (length) *length=self->frame_count;
    return MA_SUCCESS;
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

static ma_result init_source(JceReadSource *source,
                              const ma_allocation_callbacks *alloc,
                              ma_data_source **out_backend)
{
    jce_ma_opus *self;
    JceOggOpus *opus;
    ma_data_source_config config=ma_data_source_config_init();
    if (!source) return MA_INVALID_FILE;
    opus=jce_ogg_opus_open(source);
    jce_read_source_close(source);
    if (!opus) return MA_INVALID_FILE;
    self=ma_malloc(sizeof(*self),alloc);
    if (!self) { jce_ogg_opus_close(opus); return MA_OUT_OF_MEMORY; }
    memset(self,0,sizeof(*self));
    self->opus=opus;
    self->channels=jce_ogg_opus_channels(opus);
    self->sample_rate=OPUS_RATE;
    self->frame_count=jce_ogg_opus_length(opus);
    config.vtable=&g_jce_opus_ds_vtable;
    if (ma_data_source_init(&config,&self->ds)!=MA_SUCCESS) {
        jce_ogg_opus_close(opus); ma_free(self,alloc); return MA_INVALID_OPERATION;
    }
    *out_backend=(ma_data_source *)self;
    return MA_SUCCESS;
}

static ma_result on_init_memory(void *user_data,const void *data,size_t size,
                                const ma_decoding_backend_config *config,
                                const ma_allocation_callbacks *alloc,
                                ma_data_source **out_backend)
{
    (void)user_data; (void)config;
    return init_source(jce_read_source_open_memory(data,size,false),alloc,out_backend);
}

static ma_result on_init_file(void *user_data,const char *path,
                              const ma_decoding_backend_config *config,
                              const ma_allocation_callbacks *alloc,
                              ma_data_source **out_backend)
{
    (void)user_data; (void)config;
    return init_source(jce_read_source_open_file(path),alloc,out_backend);
}

static void on_uninit(void *user_data,ma_data_source *backend,
                      const ma_allocation_callbacks *alloc)
{
    jce_ma_opus *self=(jce_ma_opus *)backend;
    (void)user_data;
    if (!self) return;
    ma_data_source_uninit(&self->ds);
    jce_ogg_opus_close(self->opus);
    ma_free(self,alloc);
}

const ma_decoding_backend_vtable g_jce_ma_opus_backend_vtable = {
    NULL, on_init_file, NULL, on_init_memory, on_uninit
};
