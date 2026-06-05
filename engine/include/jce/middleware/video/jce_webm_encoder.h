/*
 * jce_webm_encoder.h  VP9 -> WebM video encoder (C ABI over libvpx + libwebm).
 *
 * Encodes a stream of BGRA frames to a VP9 video track in a .webm container.
 * All calls must come from a single thread (the recorder's worker). Audio
 * (Opus) is a planned follow-up; v1 is video-only.
 *
 * Layer: Middleware / Video (links libvpx + libwebm).
 */

#ifndef JCE_WEBM_ENCODER_H
#define JCE_WEBM_ENCODER_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceWebmEncoder JceWebmEncoder;

/* Create a VP9 (+ optional Opus) Matroska/WebM encoder writing to `path`.
 * Video dims are rounded down to even (I420). `fps` seeds the keyframe
 * interval. If `audio_sample_rate` > 0 an Opus audio track is added
 * (`audio_channels` = 1 or 2); pass 0 for video-only. Video+audio frames are
 * internally reordered by timestamp before muxing. Returns NULL on failure. */
JCE_API JceWebmEncoder *jce_webm_encoder_create(const char *path,
                                                uint32_t width, uint32_t height,
                                                uint32_t fps,
                                                uint32_t bitrate_kbps,
                                                uint32_t audio_sample_rate,
                                                uint32_t audio_channels);

/* Encode + queue one BGRA8 frame. `pitch` = source bytes/row; `yflip` = 1 when
 * source rows are bottom-up. `ts_ms` = presentation time (ms from record start,
 * monotonic). Returns false on error. */
JCE_API bool jce_webm_encoder_push_bgra(JceWebmEncoder *e, const void *bgra,
                                        uint32_t pitch, int yflip, uint64_t ts_ms);

/* Feed interleaved f32 audio PCM (`frames` per channel). `ts_ms` = wall-clock
 * time (ms from record start) of this chunk, used to anchor the audio timeline
 * to the video. No-op if the encoder has no audio track. */
JCE_API bool jce_webm_encoder_push_audio(JceWebmEncoder *e, const float *pcm,
                                         uint32_t frames, uint64_t ts_ms);

/* Flush, finalize the container, and free. */
JCE_API void jce_webm_encoder_finish(JceWebmEncoder *e);

JCE_EXTERN_C_END

#endif /* JCE_WEBM_ENCODER_H */
