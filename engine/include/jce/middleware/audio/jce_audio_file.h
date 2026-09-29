#ifndef JCE_AUDIO_FILE_H
#define JCE_AUDIO_FILE_H
#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
JCE_EXTERN_C_BEGIN

typedef struct JceAudioFile JceAudioFile;
#define JCE_AUDIO_FILE_WAVEFORM_BINS 2048u

/* Host-file audio with bounded worker PCM buffering. Supports the native
 * audio decoders, raw ADTS AAC and MP4 embedded audio when enabled. Ogg Opus
 * currently supports mono/stereo mapping family 0; chained links are rejected. Never
 * retains a full encoded file or full-clip PCM. Owner-thread lifecycle/seek;
 * pull may run on the audio device. Stop the voice before closing this file. */
JCE_API JceAudioFile *jce_audio_file_open(const char *path);
JCE_API void jce_audio_file_close(JceAudioFile *file);
JCE_API bool jce_audio_file_format(const JceAudioFile *file,
                                  uint32_t *channels, uint32_t *samplerate,
                                  double *duration);
JCE_API uint32_t jce_audio_file_pull(JceAudioFile *file, int16_t *out,
                                    uint32_t frames);
JCE_API void jce_audio_file_seek(JceAudioFile *file, double seconds);
JCE_API double jce_audio_file_time(const JceAudioFile *file);
JCE_API bool jce_audio_file_eof(const JceAudioFile *file);
/* Copies fixed, progressively generated peak bins. No PCM is exposed. */
JCE_API bool jce_audio_file_waveform(JceAudioFile *file, uint8_t *peaks,
                                    size_t capacity, float *progress);

JCE_EXTERN_C_END
#endif
