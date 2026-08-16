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

/* Open a decoder for a RAW ADTS stream (a bare .aac file). ADTS carries its
 * own per-frame headers, so there is no AudioSpecificConfig to pass and the
 * sample rate / channel count only become known after the first decoded
 * frame. Returns NULL on failure. */
JceAacDecoder *jce_aac_decoder_open_adts(void);

/* Feed and drain are SEPARATE on purpose.
 *
 * fdk-aac's Fill() takes as much as its internal buffer holds — several
 * frames at a time — while DecodeFrame() emits exactly one. A combined
 * "feed once, decode once" call therefore leaves frames stranded inside the
 * decoder, and a caller that loops until its input is exhausted silently
 * truncates the tail (measured: 552 of 624 frames, 12.8 s of a 14.5 s file,
 * with no error reported anywhere).
 *
 * Correct use is: drain to empty, then feed, repeat; at end of input, drain
 * to empty one last time. */

/* Push bytes into the decoder. Returns how many were consumed (0 when the
 * internal buffer is full — drain first). */
uint32_t jce_aac_adts_feed(JceAacDecoder *dec, const void *data,
                           uint32_t bytes);

/* Emit one frame from data already inside the decoder, WITHOUT feeding.
 * Returns false when the decoder needs more input (i.e. it is drained) or on
 * a hard error; *out_samples is 0 in both cases. */
bool jce_aac_adts_decode(JceAacDecoder *dec,
                         int16_t *out_pcm, uint32_t out_capacity,
                         uint32_t *out_samples);

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
