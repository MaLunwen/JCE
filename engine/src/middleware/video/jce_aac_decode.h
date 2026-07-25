/*
 * jce_aac_decode.h  Thin AAC-LC decoder adapter wrapping libfdk_aac.
 *
 * Accepts AudioSpecificConfig for initialization and decodes individual
 * AAC access units to interleaved signed 16-bit PCM.
 *
 * This is the ONE fdk-aac wrapper in the engine: the video player uses it for
 * MP4 audio tracks and jce_m4a_decode.c (audio layer) for standalone M4A
 * files. Don't add a second one — extend this instead.
 */

#ifndef JCE_AAC_DECODE_H
#define JCE_AAC_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceAacDecoder JceAacDecoder;

/* Open an AAC decoder initialized from an AudioSpecificConfig blob.
 * Returns NULL on failure (unsupported profile, bad config, etc.). */
JceAacDecoder *jce_aac_decoder_open(const void *asc_config, uint32_t asc_bytes);

/* Decode one AAC access unit to interleaved signed 16-bit PCM.
 * out_pcm must hold at least out_capacity samples (frames * channels).
 * On success, *out_samples receives the number of PCM samples written
 * (= decoded_frames * channels).
 * Returns false on decode error or if output buffer is too small. */
bool jce_aac_decode_frame(JceAacDecoder *dec,
                          const void *aac_frame, uint32_t frame_bytes,
                          int16_t *out_pcm, uint32_t out_capacity,
                          uint32_t *out_samples);

/* Query decoded stream info (valid after at least one successful decode). */
uint32_t jce_aac_decoder_get_channels(const JceAacDecoder *dec);
uint32_t jce_aac_decoder_get_samplerate(const JceAacDecoder *dec);
uint32_t jce_aac_decoder_get_frame_size(const JceAacDecoder *dec);

void jce_aac_decoder_close(JceAacDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* JCE_AAC_DECODE_H */
