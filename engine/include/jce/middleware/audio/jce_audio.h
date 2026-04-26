/*
 * jce_audio.h  Cross-platform audio system.
 *
 * Loads WAV/OGG from the PAK archive, manages voices for playback.
 */

#ifndef JCE_AUDIO_H
#define JCE_AUDIO_H


#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdint.h>

#include <jce/middleware/audio/jce_audio_types.h>

JCE_EXTERN_C_BEGIN

/* Forward declaration  avoids pulling in pak_loader.h in every TU. */
typedef struct JcePakArchive JcePakArchive;

typedef struct JceAudio JceAudio;

/* -- Lifecycle ------------------------------------------------------ */

JceAudio *jce_audio_create(void);
void      jce_audio_destroy(JceAudio *audio);

/* -- Sound loading (from PAK) --------------------------------------- */

/* Load a sound from the PAK archive.  Supports .wav files.
   Returns JCE_SOUND_INVALID on failure. */
JceSound  jce_audio_load(JceAudio *audio, const JcePakArchive *pak, const char *path);

/* Upload pre-decoded PCM data as a sound.
   channels: 1 or 2, bits: 8 or 16.
   Caller retains ownership of pcm_data. */
JceSound  jce_audio_load_pcm(JceAudio *audio,
                               const void *pcm_data, uint32_t pcm_size,
                               uint16_t channels, uint32_t sample_rate,
                               uint16_t bits_per_sample);

/* Unload a previously loaded sound. */
void      jce_audio_unload(JceAudio *audio, JceSound snd);

/* Load a sound from raw file bytes in memory (WAV/OGG/MP3).
   hint_path is used for format detection only; may be NULL. */
JceSound  jce_audio_load_memory(JceAudio *audio, const void *data,
                                 uint32_t size, const char *hint_path);

/* -- Queries -------------------------------------------------------- */

/* Duration of a loaded sound in seconds.  Returns 0 on error. */
float     jce_audio_get_duration(const JceAudio *audio, JceSound snd);

/* Current playback position of a voice in seconds. */
float     jce_audio_get_time(const JceAudio *audio, JceVoice voice);

/* Seek a playing/paused voice to a specific time (seconds). */
void      jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec);

/* Access decoded PCM data of a loaded sound (16-bit signed).
   Returns NULL on error.  Caller must NOT free the returned pointer. */
const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
                                       uint32_t *out_frame_count,
                                       uint32_t *out_channels);

/* -- Playback ------------------------------------------------------- */

/* Play a sound.  Returns a voice handle for further control.
   volume: 0.01.0,  pitch: 1.0 = normal. */
JceVoice  jce_audio_play(JceAudio *audio, JceSound snd,
                          bool loop, float volume, float pitch);

void      jce_audio_stop(JceAudio *audio, JceVoice voice);
void      jce_audio_pause(JceAudio *audio, JceVoice voice);
void      jce_audio_resume(JceAudio *audio, JceVoice voice);

void      jce_audio_set_volume(JceAudio *audio, JceVoice voice, float volume);
void      jce_audio_set_pitch(JceAudio *audio, JceVoice voice, float pitch);
void      jce_audio_set_looping(JceAudio *audio, JceVoice voice, bool loop);
bool      jce_audio_is_playing(const JceAudio *audio, JceVoice voice);

/* -- Streaming playback ---------------------------------------------- */

/* Pull callback for jce_audio_play_stream(). Must write up to
 * `frames` interleaved s16 frames into `out` and return the number
 * actually produced. Returning 0 signals end-of-stream. Called from
 * the audio device thread. Must be thread-safe. */
typedef uint32_t (*JceAudioStreamPullFn)(void *ud,
                                          int16_t *out,
                                          uint32_t frames);

/* Begin streaming playback driven by a pull callback. Returns a voice
 * handle. The caller retains ownership of `ud` and is responsible for
 * keeping it valid until the voice is stopped (jce_audio_stop). */
JceVoice  jce_audio_play_stream(JceAudio *audio,
                                 JceAudioStreamPullFn on_read, void *ud,
                                 uint16_t channels, uint32_t sample_rate,
                                 float volume, float pitch);

/* -- Global --------------------------------------------------------- */

void      jce_audio_set_master_volume(JceAudio *audio, float volume);
void      jce_audio_stop_all(JceAudio *audio);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_H */
