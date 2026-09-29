/*
 * jce_audio_decode.h  Encoded bytes -> interleaved s16 PCM.
 *
 * Split out of jce_audio.c when that file passed AGENTS.md §11's 3000-line
 * cap.  The seam is not arbitrary: these functions touch no JceAudio state,
 * no node graph and no device, so they were never part of the engine object.
 * Keeping them here also keeps the format zoo (WAV / OGG / MP3 / FLAC / M4A /
 * raw ADTS / Opus) in one place instead of interleaved with voice and bus
 * lifetime.
 *
 * PRIVATE to engine/src/middleware/audio: not a public API, not in
 * engine/include/, and therefore not in the ABI snapshot.
 */

#ifndef JCE_AUDIO_DECODE_H
#define JCE_AUDIO_DECODE_H

#ifndef JCE_NO_AUDIO

#include <miniaudio.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode WAV / OGG / MP3 / FLAC / M4A / AAC from memory into a standalone s16
 * PCM buffer.  Touches no JceAudio state, so it is safe to call from any
 * thread -- which is what the worker-decode path relies on.
 *
 * `path` is used only for log messages and for the container sniff; the bytes
 * are authoritative.  On success *out_pcm is a JCE_MALLOC'd buffer the CALLER
 * owns and must release with JCE_FREE.  On failure every out-param is zeroed
 * and nothing is allocated.
 */
bool jce_audio_decode_pcm_mem(const uint8_t *data, size_t size,
                              const char *path,
                              int16_t **out_pcm, ma_uint64 *out_frames,
                              ma_uint32 *out_channels, ma_uint32 *out_rate);

#ifdef __cplusplus
}
#endif

#endif /* !JCE_NO_AUDIO */
#endif /* JCE_AUDIO_DECODE_H */
