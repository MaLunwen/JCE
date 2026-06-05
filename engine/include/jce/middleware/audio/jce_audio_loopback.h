/*
 * jce_audio_loopback.h  System-output (WASAPI loopback) audio capture.
 *
 * Captures whatever the system is playing ("what you hear") as interleaved
 * f32 PCM, for screen recording. Independent of the jce_audio engine.
 * Windows-only (WASAPI loopback); a no-op elsewhere.
 *
 * Layer: Middleware / Audio (links miniaudio).
 */

#ifndef JCE_AUDIO_LOOPBACK_H
#define JCE_AUDIO_LOOPBACK_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Invoked on the audio capture thread with interleaved f32 system-output PCM. */
typedef void (*JceAudioLoopbackFn)(void *ud, const float *pcm, uint32_t frames,
                                   uint32_t sample_rate, uint32_t channels);

/* Start loopback capture of the system output. Returns false on failure or on
 * non-Windows platforms. Single global instance. */
JCE_API bool jce_audio_loopback_start(JceAudioLoopbackFn cb, void *ud);

/* Stop loopback capture. */
JCE_API void jce_audio_loopback_stop(void);

JCE_API bool jce_audio_loopback_is_active(void);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_LOOPBACK_H */
