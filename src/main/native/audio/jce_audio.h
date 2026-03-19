/*
 * jce_audio.h  Cross-platform audio via miniaudio.
 *
 * Loads WAV/OGG from the PAK archive via miniaudio, manages
 * voices for playback.
 */

#ifndef JCE_AUDIO_H
#define JCE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "jce_audio_types.h"

/* Forward declaration  avoids pulling in pak_loader.h in every TU. */
typedef struct PakArchive PakArchive;

typedef struct JceAudio JceAudio;

/* -- Lifecycle ------------------------------------------------------ */

JceAudio *jce_audio_create(void);
void      jce_audio_destroy(JceAudio *audio);

/* -- Sound loading (from PAK) --------------------------------------- */

/* Load a sound from the PAK archive.  Supports .wav files.
   Returns JCE_SOUND_INVALID on failure. */
JceSound  jce_audio_load(JceAudio *audio, PakArchive *pak, const char *path);

/* Upload pre-decoded PCM data as a sound.
   channels: 1 or 2, bits: 8 or 16.
   Caller retains ownership of pcm_data. */
JceSound  jce_audio_load_pcm(JceAudio *audio,
                               const void *pcm_data, uint32_t pcm_size,
                               uint16_t channels, uint32_t sample_rate,
                               uint16_t bits_per_sample);

/* Unload a previously loaded sound. */
void      jce_audio_unload(JceAudio *audio, JceSound snd);

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

/* -- Global --------------------------------------------------------- */

void      jce_audio_set_master_volume(JceAudio *audio, float volume);
void      jce_audio_stop_all(JceAudio *audio);

#endif /* JCE_AUDIO_H */
