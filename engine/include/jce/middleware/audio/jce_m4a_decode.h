/*
 * jce_m4a_decode.h  Decode M4A (AAC-in-MP4) audio files to PCM.
 *
 * Uses minimp4 for container parsing and FDK-AAC for AAC decoding.
 */

#ifndef JCE_M4A_DECODE_H
#define JCE_M4A_DECODE_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Check if a memory buffer looks like an MP4/M4A container (ftyp box). */
JCE_API bool jce_m4a_is_mp4_container(const void *data, size_t size);

/* Decode M4A/MP4 audio to interleaved signed 16-bit PCM.
 * On success, *out_pcm is JCE_MALLOC'd and must be freed by the caller.
 * Returns false on failure (no audio track, unsupported codec, etc.). */
JCE_API bool JCE_CALL jce_m4a_decode_to_pcm(const void *data, size_t size,
                                            int16_t **out_pcm,
                                            uint32_t *out_frames,
                                            uint32_t *out_channels,
                                            uint32_t *out_samplerate);

JCE_EXTERN_C_END

#endif /* JCE_M4A_DECODE_H */
