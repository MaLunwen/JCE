#include <jce/middleware/audio/jce_audio_file.h>
#include <jce/middleware/video/jce_video.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_memory.h"
#include "middleware/video/jce_audio_stream.h"
#include <string.h>

#ifndef JCE_NO_AUDIO
#include "jce_miniaudio_opus_backend.h"
#include "jce_adts_file.h"

typedef struct {
    ma_decoder decoder;
    bool native;
    JceAdtsFile *adts;
    JceVideo video;
    uint32_t channels, samplerate;
    uint64_t frames;
    double duration;
} FileDecoder;

struct JceAudioFile {
    FileDecoder *decoder;
    JceAudioStream *stream;
    char *path;
    JceMutex *mutex;
    JceAsyncTask *wave_task;
    uint8_t peaks[JCE_AUDIO_FILE_WAVEFORM_BINS];
    float progress;
};

static FileDecoder *decoder_open(const char *path)
{
    FileDecoder *decoder = JCE_CALLOC(1u,sizeof(*decoder));
    static const ma_decoding_backend_vtable *backends[] = {&g_jce_ma_opus_backend_vtable};
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16,0u,0u);
    if (!decoder) return NULL;
    config.ppCustomBackendVTables = backends;
    config.customBackendCount = 1u;
    if (ma_decoder_init_file(path,&config,&decoder->decoder) == MA_SUCCESS) {
        ma_uint64 frames=0u;
        decoder->native=true;
        decoder->channels=decoder->decoder.outputChannels;
        decoder->samplerate=decoder->decoder.outputSampleRate;
        (void)ma_decoder_get_length_in_pcm_frames(&decoder->decoder,&frames);
        decoder->frames=frames;
        decoder->duration=decoder->samplerate ? (double)frames/decoder->samplerate : 0.0;
    } else if ((decoder->adts=jce_adts_file_open(path))!=NULL) {
        jce_adts_file_format(decoder->adts,&decoder->channels,&decoder->samplerate,
                             &decoder->frames);
        decoder->duration=(double)decoder->frames/decoder->samplerate;
    } else {
        decoder->video=jce_video_load_file(path);
        if (decoder->video == JCE_VIDEO_INVALID ||
            !jce_video_get_audio_format(decoder->video,&decoder->channels,
                                       &decoder->samplerate,&decoder->duration)) {
            jce_video_unload(decoder->video);
            JCE_FREE(decoder);
            return NULL;
        }
        decoder->frames=(uint64_t)(decoder->duration*decoder->samplerate);
    }
    if (!decoder->channels || decoder->channels > 8u || !decoder->samplerate) {
        if (decoder->native) ma_decoder_uninit(&decoder->decoder);
        else if (decoder->adts) jce_adts_file_close(decoder->adts);
        else jce_video_unload(decoder->video);
        JCE_FREE(decoder);
        return NULL;
    }
    return decoder;
}

static void decoder_close(void *user)
{
    FileDecoder *decoder=user;
    if (!decoder) return;
    if (decoder->native) ma_decoder_uninit(&decoder->decoder);
    else if (decoder->adts) jce_adts_file_close(decoder->adts);
    else jce_video_unload(decoder->video);
    JCE_FREE(decoder);
}

static uint32_t decoder_read(void *user, int16_t *out, uint32_t capacity)
{
    FileDecoder *decoder=user;
    ma_uint64 frames=0u;
    if (decoder->adts) return jce_adts_file_read(decoder->adts,out,capacity);
    if (!decoder->native) return jce_video_audio_pull(decoder->video,out,capacity);
    (void)ma_decoder_read_pcm_frames(&decoder->decoder,out,capacity,&frames);
    return (uint32_t)frames;
}

static void decoder_seek(void *user, double seconds)
{
    FileDecoder *decoder=user;
    if (decoder->adts) {
        (void)jce_adts_file_seek(decoder->adts,(uint64_t)(seconds*decoder->samplerate));
        return;
    }
    (void)ma_decoder_seek_to_pcm_frame(&decoder->decoder,
                                      (ma_uint64)(seconds*decoder->samplerate));
}

static JceAsyncRunResult waveform_work(JceAsyncContext *ctx, void *user)
{
    JceAudioFile *file=user;
    FileDecoder *decoder=decoder_open(file->path);
    int16_t pcm[4096u*8u];
    uint8_t peaks[JCE_AUDIO_FILE_WAVEFORM_BINS]={0};
    uint64_t position=0u;
    unsigned chunks=0u;
    if (!decoder || !decoder->frames) {
        decoder_close(decoder);
        return JCE_ASYNC_RUN_FAILED;
    }
    while (!jce_async_context_cancel_requested(ctx)) {
        uint32_t frames=decoder_read(decoder,pcm,4096u), i;
        if (!frames) {
            if (!decoder->native && !decoder->adts && !jce_video_audio_eof(decoder->video)) {
                jce_thread_sleep_ms(1u);
                continue;
            }
            break;
        }
        for (i=0u; i<frames; ++i) {
            size_t bin=(size_t)((double)(position+i)*JCE_AUDIO_FILE_WAVEFORM_BINS/decoder->frames);
            uint32_t channel;
            if (bin >= JCE_AUDIO_FILE_WAVEFORM_BINS) bin=JCE_AUDIO_FILE_WAVEFORM_BINS-1u;
            for (channel=0u; channel<decoder->channels; ++channel) {
                int amplitude=pcm[(size_t)i*decoder->channels+channel];
                uint8_t peak;
                if (amplitude<0) amplitude=-amplitude;
                peak=(uint8_t)(amplitude*255/32768);
                if (peak > peaks[bin]) peaks[bin]=peak;
            }
        }
        position+=frames;
        jce_mutex_lock(file->mutex);
        memcpy(file->peaks,peaks,sizeof(peaks));
        file->progress=(float)((double)position/decoder->frames);
        if (file->progress>1.0f) file->progress=1.0f;
        jce_mutex_unlock(file->mutex);
        if (++chunks%16u == 0u) jce_thread_sleep_ms(1u);
    }
    if (!jce_async_context_cancel_requested(ctx)) {
        jce_mutex_lock(file->mutex);
        file->progress=1.0f;
        jce_mutex_unlock(file->mutex);
    }
    decoder_close(decoder);
    return jce_async_context_cancel_requested(ctx) ? JCE_ASYNC_RUN_CANCELLED : JCE_ASYNC_RUN_SUCCESS;
}

JceAudioFile *jce_audio_file_open(const char *path)
{
    JceAudioFile *file;
    JceAsyncTaskDesc task;
    JceAudioStreamDesc stream={0};
    size_t length;
    if (!path || !path[0]) return NULL;
    file=JCE_CALLOC(1u,sizeof(*file));
    if (!file) return NULL;
    length=strlen(path)+1u;
    file->path=JCE_MALLOC(length);
    file->mutex=jce_mutex_create();
    if (!file->path || !file->mutex) { jce_audio_file_close(file); return NULL; }
    memcpy(file->path,path,length);
    file->decoder=decoder_open(path);
    if (!file->decoder) { jce_audio_file_close(file); return NULL; }
    if (file->decoder->native || file->decoder->adts) {
        stream.decode_next=decoder_read; stream.seek=decoder_seek;
        /* file owns decoder: stream destroys only its PCM ring/worker. */
        stream.ud=file->decoder;
        stream.channels=file->decoder->channels;
        stream.samplerate=file->decoder->samplerate;
        stream.duration_sec=file->decoder->duration;
        file->stream=jce_audio_stream_create(&stream);
        if (!file->stream) { jce_audio_file_close(file); return NULL; }
    }
    jce_async_task_desc_init(&task);
    task.work=waveform_work; task.user_data=file;
    task.priority=JCE_ASYNC_PRIORITY_BACKGROUND;
    task.debug_name="audio waveform";
    file->wave_task=jce_async_submit(jce_async_default_executor(),&task);
    return file;
}

void jce_audio_file_close(JceAudioFile *file)
{
    if (!file) return;
    if (file->wave_task) jce_async_task_discard(file->wave_task);
    jce_audio_stream_destroy(file->stream);
    decoder_close(file->decoder);
    jce_mutex_destroy(file->mutex);
    JCE_FREE(file->path);
    JCE_FREE(file);
}

bool jce_audio_file_format(const JceAudioFile *file, uint32_t *channels,
                           uint32_t *samplerate, double *duration)
{
    if (!file || !file->decoder) return false;
    if (channels) *channels=file->decoder->channels;
    if (samplerate) *samplerate=file->decoder->samplerate;
    if (duration) *duration=file->decoder->duration;
    return true;
}

uint32_t jce_audio_file_pull(JceAudioFile *file, int16_t *out, uint32_t frames)
{
    if (!file || !out) return 0u;
    return file->stream ? jce_audio_stream_pull(file->stream,out,frames)
        : jce_video_audio_pull(file->decoder->video,out,frames);
}

void jce_audio_file_seek(JceAudioFile *file, double seconds)
{
    if (!file) return;
    if (!(seconds>=0.0)) seconds=0.0;
    if (file->stream) jce_audio_stream_seek(file->stream,seconds);
    else jce_video_audio_seek(file->decoder->video,seconds);
}

double jce_audio_file_time(const JceAudioFile *file)
{
    if (!file) return 0.0;
    return file->stream ? jce_audio_stream_get_time(file->stream)
        : jce_video_audio_get_time(file->decoder->video);
}

bool jce_audio_file_eof(const JceAudioFile *file)
{
    if (!file) return true;
    return file->stream ? jce_audio_stream_eof(file->stream)
        : jce_video_audio_eof(file->decoder->video);
}

bool jce_audio_file_waveform(JceAudioFile *file, uint8_t *peaks,
                             size_t capacity, float *progress)
{
    if (!file || !peaks || capacity < JCE_AUDIO_FILE_WAVEFORM_BINS) return false;
    jce_mutex_lock(file->mutex);
    memcpy(peaks,file->peaks,sizeof(file->peaks));
    if (progress) *progress=file->progress;
    jce_mutex_unlock(file->mutex);
    return true;
}
#else
JceAudioFile *jce_audio_file_open(const char *path) { (void)path; return NULL; }
void jce_audio_file_close(JceAudioFile *file) { (void)file; }
bool jce_audio_file_format(const JceAudioFile *f,uint32_t *c,uint32_t *s,double *d)
{ (void)f; if(c)*c=0u; if(s)*s=0u; if(d)*d=0.0; return false; }
uint32_t jce_audio_file_pull(JceAudioFile *f,int16_t *o,uint32_t n)
{ (void)f; (void)o; (void)n; return 0u; }
void jce_audio_file_seek(JceAudioFile *f,double t) { (void)f; (void)t; }
double jce_audio_file_time(const JceAudioFile *f) { (void)f; return 0.0; }
bool jce_audio_file_eof(const JceAudioFile *f) { (void)f; return true; }
bool jce_audio_file_waveform(JceAudioFile *f,uint8_t *p,size_t n,float *v)
{ (void)f; (void)p; (void)n; if(v)*v=0.0f; return false; }
#endif
