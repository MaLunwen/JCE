/*
 * jce_async_loader.c  Asynchronous PAK asset loading via SDL threads.
 *
 * Each task spawns an SDL thread that performs CPU-heavy work
 * (decompress + decode). The main thread finalizes by creating
 * GPU/audio resources (which must happen on the main thread).
 */

#include "jce_async_loader.h"
#include "pak_loader.h"
#include "graphics/jce_texture.h"
#include "audio/jce_audio.h"
#include "foundation/jce_log.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef JCE_NO_AUDIO
#include <miniaudio.h>
#endif

#define LOG_TAG "jce_async"

/* -- Task types ---------------------------------------------------- */

typedef enum {
    TASK_TEXTURE,
    TASK_AUDIO
} TaskType;

/* Decoded audio result (produced by worker, consumed by main thread). */
typedef struct {
    void    *pcm_data;
    uint32_t pcm_size;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
} DecodedAudio;

/* Decoded texture result (produced by worker, consumed by main thread). */
typedef struct {
    SDL_Surface *surface;   /* RGBA8, ready for GPU upload */
    int          sampler_mode;
} DecodedTexture;

struct JceAsyncTask {
    TaskType     type;
    SDL_Thread  *thread;
    SDL_AtomicInt done;     /* 0 = working, 1 = done */
    bool         success;

    /* Input (read-only after creation). */
    PakArchive  *pak;
    char         path[256];

    /* Output (written by worker, read by main after done). */
    union {
        DecodedTexture tex;
        DecodedAudio   audio;
    } result;
};

/* -- Worker: texture decode ---------------------------------------- */

static SDL_Surface *ensure_rgba8(SDL_Surface *src)
{
    if (!src) return NULL;
    if (src->format == SDL_PIXELFORMAT_RGBA32) return src;

    SDL_Surface *converted = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(src);
    return converted;
}

static int texture_worker(void *data)
{
    JceAsyncTask *task = (JceAsyncTask *)data;

    const PakAsset *asset = pak_find(task->pak, task->path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found: %s", task->path);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    void *buf = SDL_malloc((size_t)asset->original_size);
    if (!buf) {
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompress failed: %s", task->path);
        SDL_free(buf);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)asset->original_size);
    if (!io) {
        SDL_free(buf);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    SDL_Surface *surf = IMG_Load_IO(io, true);
    SDL_free(buf);

    if (!surf) {
        LOG_ERROR(LOG_TAG, "IMG_Load_IO failed: %s", task->path);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    surf = ensure_rgba8(surf);
    if (surf) {
        task->result.tex.surface = surf;
        task->success = true;
    }

    SDL_SetAtomicInt(&task->done, 1);
    return 0;
}

/* -- Worker: audio decode ------------------------------------------ */

static int audio_worker(void *data)
{
    JceAsyncTask *task = (JceAsyncTask *)data;

    const PakAsset *asset = pak_find(task->pak, task->path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found: %s", task->path);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    void *buf = SDL_malloc((size_t)asset->original_size);
    if (!buf) {
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompress failed: %s", task->path);
        SDL_free(buf);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

#ifndef JCE_NO_AUDIO
    /* Decode WAV/OGG via miniaudio — always output s16 PCM. */
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder decoder;

    ma_result ma_res = ma_decoder_init_memory(buf, n, &cfg, &decoder);
    if (ma_res != MA_SUCCESS) {
        LOG_ERROR(LOG_TAG, "audio decode failed: %s (ma_result=%d)", task->path, (int)ma_res);
        SDL_free(buf);
        SDL_SetAtomicInt(&task->done, 1);
        return 0;
    }

    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 sample_rate = decoder.outputSampleRate;
    void *pcm = NULL;

    if (total_frames == 0) {
        /* Unknown length — decode in growing chunks. */
        size_t alloc_frames = 1024 * 256;
        size_t used_frames  = 0;
        pcm = SDL_malloc(alloc_frames * channels * sizeof(int16_t));
        if (!pcm) { ma_decoder_uninit(&decoder); SDL_free(buf);
                     SDL_SetAtomicInt(&task->done, 1); return 0; }

        for (;;) {
            if (used_frames + 4096 > alloc_frames) {
                alloc_frames *= 2;
                void *tmp = SDL_realloc(pcm, alloc_frames * channels * sizeof(int16_t));
                if (!tmp) { SDL_free(pcm); ma_decoder_uninit(&decoder);
                             SDL_free(buf); SDL_SetAtomicInt(&task->done, 1); return 0; }
                pcm = tmp;
            }
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&decoder,
                (int16_t *)pcm + used_frames * channels, 4096, &read);
            if (read == 0) break;
            used_frames += (size_t)read;
        }
        total_frames = (ma_uint64)used_frames;
    } else {
        pcm = SDL_malloc((size_t)(total_frames * channels * sizeof(int16_t)));
        if (!pcm) { ma_decoder_uninit(&decoder); SDL_free(buf);
                     SDL_SetAtomicInt(&task->done, 1); return 0; }

        ma_uint64 frames_read = 0;
        ma_decoder_read_pcm_frames(&decoder, pcm, total_frames, &frames_read);
        total_frames = frames_read;
    }

    ma_decoder_uninit(&decoder);

    task->result.audio.pcm_data        = pcm;
    task->result.audio.pcm_size        = (uint32_t)(total_frames * channels * sizeof(int16_t));
    task->result.audio.channels        = (uint16_t)channels;
    task->result.audio.sample_rate     = sample_rate;
    task->result.audio.bits_per_sample = 16;
    task->success = true;
#else
    LOG_WARN(LOG_TAG, "audio disabled, skipping: %s", task->path);
#endif

    SDL_free(buf);
    SDL_SetAtomicInt(&task->done, 1);
    return 0;
}

/* -- Public API ---------------------------------------------------- */

JceAsyncTask *jce_async_load_texture(PakArchive *pak, const char *path,
                                      int sampler_mode)
{
    if (!pak || !path) return NULL;

    JceAsyncTask *task = (JceAsyncTask *)SDL_calloc(1, sizeof(*task));
    if (!task) return NULL;

    task->type = TASK_TEXTURE;
    task->pak  = pak;
    task->result.tex.sampler_mode = sampler_mode;
    snprintf(task->path, sizeof(task->path), "%s", path);

    task->thread = SDL_CreateThread(texture_worker, task->path, task);
    if (!task->thread) {
        SDL_free(task);
        return NULL;
    }

    return task;
}

JceAsyncTask *jce_async_load_audio(PakArchive *pak, const char *path)
{
    if (!pak || !path) return NULL;

    JceAsyncTask *task = (JceAsyncTask *)SDL_calloc(1, sizeof(*task));
    if (!task) return NULL;

    task->type = TASK_AUDIO;
    task->pak  = pak;
    snprintf(task->path, sizeof(task->path), "%s", path);

    task->thread = SDL_CreateThread(audio_worker, task->path, task);
    if (!task->thread) {
        SDL_free(task);
        return NULL;
    }

    return task;
}

bool jce_async_task_done(const JceAsyncTask *task)
{
    if (!task) return true;
    return SDL_GetAtomicInt((SDL_AtomicInt *)&task->done) != 0;
}

JceTexture jce_async_finalize_texture(JceAsyncTask *task)
{
    JceTexture invalid = { UINT16_MAX };
    if (!task || task->type != TASK_TEXTURE) return invalid;
    if (!jce_async_task_done(task) || !task->success) return invalid;

    SDL_Surface *surf = task->result.tex.surface;
    if (!surf) return invalid;

    JceTexture tex = jce_texture_load_from_surface(
        surf, task->result.tex.sampler_mode);

    /* Surface ownership transferred  don't destroy here,
       jce_texture_load_from_surface doesn't take ownership, so destroy. */
    SDL_DestroySurface(surf);
    task->result.tex.surface = NULL;

    if (tex.idx != UINT16_MAX)
        LOG_DEBUG(LOG_TAG, "finalized texture: %s", task->path);

    return tex;
}

JceSound jce_async_finalize_audio(JceAsyncTask *task, JceAudio *audio)
{
    if (!task || !audio || task->type != TASK_AUDIO) return JCE_SOUND_INVALID;
    if (!jce_async_task_done(task) || !task->success) return JCE_SOUND_INVALID;

    DecodedAudio *a = &task->result.audio;
    JceSound snd = jce_audio_load_pcm(audio, a->pcm_data, a->pcm_size,
                                       a->channels, a->sample_rate,
                                       a->bits_per_sample);

    /* PCM data no longer needed after upload. */
    if (a->pcm_data) {
        SDL_free(a->pcm_data);
        a->pcm_data = NULL;
    }

    if (snd != JCE_SOUND_INVALID)
        LOG_DEBUG(LOG_TAG, "finalized audio: %s", task->path);

    return snd;
}

void jce_async_task_free(JceAsyncTask *task)
{
    if (!task) return;

    /* Wait for thread to finish. */
    if (task->thread)
        SDL_WaitThread(task->thread, NULL);

    /* Clean up any unclaimed results. */
    if (task->type == TASK_TEXTURE && task->result.tex.surface)
        SDL_DestroySurface(task->result.tex.surface);
    if (task->type == TASK_AUDIO && task->result.audio.pcm_data)
        SDL_free(task->result.audio.pcm_data);

    SDL_free(task);
}
