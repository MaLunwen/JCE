/*
 * jce_async_loader.h  Asynchronous PAK asset loading via SDL threads.
 *
 * CPU-heavy work (decompress, decode) runs on worker threads.
 * GPU/AL resource creation must be finalized on the main thread.
 */

#ifndef JCE_ASYNC_LOADER_H
#define JCE_ASYNC_LOADER_H

#include <stdbool.h>
#include <stdint.h>

/* Lightweight type includes  avoids pulling in full renderer/audio APIs. */
#include <jce/graphics/jce_texture_types.h>
#include <jce/audio/jce_audio_types.h>

typedef struct PakArchive PakArchive;
typedef struct JceAudio   JceAudio;

/* Opaque async task handle. */
typedef struct JceAsyncTask JceAsyncTask;

/* -- Texture loading ----------------------------------------------- */

/* Start async texture decode (decompress + IMG_Load on worker thread).
   sampler_mode: JCE_TEX_CLAMP / JCE_TEX_WRAP / JCE_TEX_MIRROR. */
JceAsyncTask *jce_async_load_texture(PakArchive *pak, const char *path,
                                      int sampler_mode);

/* Finalize on main thread: creates bgfx texture from decoded surface.
   Returns JCE_TEXTURE_INVALID if task not done or failed. */
JceTexture jce_async_finalize_texture(JceAsyncTask *task);

/* -- Audio loading ------------------------------------------------- */

/* Start async audio decode (decompress + WAV/OGG parse on worker thread). */
JceAsyncTask *jce_async_load_audio(PakArchive *pak, const char *path);

/* Finalize on main thread: uploads decoded PCM as a sound.
   Returns JCE_SOUND_INVALID if task not done or failed. */
JceSound jce_async_finalize_audio(JceAsyncTask *task, JceAudio *audio);

/* -- Common -------------------------------------------------------- */

/* Non-blocking check: is the worker thread done? */
bool jce_async_task_done(const JceAsyncTask *task);

/* Free the task (waits for thread if still running). */
void jce_async_task_free(JceAsyncTask *task);

#endif /* JCE_ASYNC_LOADER_H */
