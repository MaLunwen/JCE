/*
 * jce_audio_stream.h  Generic streaming audio source.
 *
 * Decouples codec-specific packet decode from playback. The owning
 * parser/decoder are held by the JceAudioStream and ticked from a
 * dedicated worker thread, which writes decoded interleaved s16 PCM
 * into a bounded ring buffer. The audio backend (or anyone else) pulls
 * from the ring on demand.
 *
 * Thread-safety:
 *   - jce_audio_stream_pull() may be called from the audio device
 *     thread.
 *   - jce_audio_stream_seek() and jce_audio_stream_destroy() must be
 *     called from the owner thread (typically the main thread). Both
 *     synchronize with the worker.
 *
 * This header is engine-internal (sits next to the .cpp implementation).
 */

#ifndef JCE_AUDIO_STREAM_H
#define JCE_AUDIO_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceAudioStream JceAudioStream;

/* Codec adapter callbacks. All called from the worker thread. */
typedef struct JceAudioStreamDesc {
    /* Decode some PCM into out[]. Return frames produced (0..out_max_frames).
     * Return 0 to signal end-of-stream. The implementation may produce
     * fewer than max_frames per call (e.g. one packet at a time). */
    uint32_t (*decode_next)(void *ud,
                            int16_t *out,
                            uint32_t out_max_frames);
    /* Seek the underlying parser to time_sec. After return, decode_next
     * must yield samples starting at (or near) that timestamp. */
    void     (*seek)(void *ud, double time_sec);
    /* Release codec/parser/user-data resources. Called once, on the
     * owner thread, after the worker has exited. */
    void     (*destroy)(void *ud);

    void    *ud;

    uint32_t channels;
    uint32_t samplerate;
    double   duration_sec;       /* used for clamping seeks; 0 = unknown */

    /* Tunables (0 = library defaults). */
    uint32_t ring_capacity_frames; /* default ~ 2 sec of audio */
    uint32_t lo_water_frames;      /* default ~ 0.5 sec */
} JceAudioStreamDesc;

JceAudioStream *jce_audio_stream_create(const JceAudioStreamDesc *desc);

/* Stops the worker, calls desc.destroy(ud), then frees the stream. */
void jce_audio_stream_destroy(JceAudioStream *s);

/* Read up to `frames` interleaved frames into out[]. Returns the number
 * actually written. May write less than requested (under-run) — caller
 * should fill the remainder with silence if it needs continuous output.
 *
 * Safe to call from the audio device thread. */
uint32_t jce_audio_stream_pull(JceAudioStream *s,
                                int16_t *out,
                                uint32_t frames);

/* Schedule a seek. Cheap from the caller's side: drops any pre-buffered
 * audio, sets a flag, then wakes the worker which performs the seek and
 * resumes decoding. */
void jce_audio_stream_seek(JceAudioStream *s, double time_sec);

/* Playback time in seconds, derived from frames pulled / samplerate. */
double jce_audio_stream_get_time(const JceAudioStream *s);

/* True iff worker reported EOF and the ring is fully drained. */
bool jce_audio_stream_eof(const JceAudioStream *s);

/* True once the first real (non-silence) sample has been pulled. While
 * false the audio clock is "cold" and pinned to the seek base. Useful
 * for A/V sync masters that want to fall back to wall-clock until audio
 * actually starts producing. */
bool jce_audio_stream_is_primed(const JceAudioStream *s);

uint32_t jce_audio_stream_channels(const JceAudioStream *s);
uint32_t jce_audio_stream_samplerate(const JceAudioStream *s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_AUDIO_STREAM_H */
