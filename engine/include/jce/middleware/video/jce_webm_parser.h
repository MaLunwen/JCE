/*
 * jce_webm_parser.h  Minimal WebM / Matroska demuxer (royalty-free).
 *
 * Wraps libwebm's mkvparser (BSD-3) behind a stable C ABI. Designed for
 * the JCE 0.8 royalty-free media path:
 *
 *     [WebM/.mkv file]
 *           │
 *           ▼
 *     jce_webm_parser  ──── video packets ───►  jce_vp8 / jce_av1
 *           │           ──── audio packets ───►  miniaudio (Opus/Vorbis)
 *           ▼
 *
 * Only what jce_video / jce_audio need is exposed: open from source or memory,
 * codec/dimensions/duration, sequential packet read for video & audio
 * tracks, coarse seek to a timestamp. Matroska's full feature surface
 * (subtitles, attachments, multiple A/V tracks, encryption, …) is not
 * forwarded — we pick the first video track and the first audio track.
 */

#ifndef JCE_WEBM_PARSER_H
#define JCE_WEBM_PARSER_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_read_source.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceWebmVideoCodec {
    JCE_WEBM_VIDEO_NONE = 0,
    JCE_WEBM_VIDEO_VP8,
    JCE_WEBM_VIDEO_VP9,
    JCE_WEBM_VIDEO_AV1
} JceWebmVideoCodec;

typedef enum JceWebmAudioCodec {
    JCE_WEBM_AUDIO_NONE = 0,
    JCE_WEBM_AUDIO_OPUS,
    JCE_WEBM_AUDIO_VORBIS
} JceWebmAudioCodec;

typedef struct JceWebmInfo {
    JceWebmVideoCodec video_codec;
    JceWebmAudioCodec audio_codec;
    uint32_t          width;            /* 0 if no video */
    uint32_t          height;           /* 0 if no video */
    uint32_t          audio_channels;   /* 0 if no audio */
    uint32_t          audio_samplerate; /* Hz; 0 if no audio */
    uint64_t          duration_ns;      /* total duration in nanoseconds */
} JceWebmInfo;

typedef struct JceWebmParser JceWebmParser;

/* Quick magic-byte check (EBML header at offset 0). */
JCE_API bool jce_webm_is_webm(const void *data, size_t size);

/* Open an in-memory WebM/Matroska stream. Buffer must outlive the parser. */
JCE_API JceWebmParser *jce_webm_open_memory(const void *data, size_t size,
                                    JceWebmInfo *out_info);

/* Retains source. Loads clusters on demand, with a bounded retained index. */
JCE_API JceWebmParser *jce_webm_open_source(JceReadSource *source,
                                          JceWebmInfo *out_info);

/* Codec-private data (e.g. Vorbis 3-packet header, Opus extradata).
 * Pointer is owned by parser and valid until close. Returns false if absent. */
JCE_API bool jce_webm_get_audio_codec_private(JceWebmParser *p,
                                      const uint8_t **out_data,
                                      size_t *out_size);

/* Read the next video packet. Output pointer remains valid until the next
 * call. Returns false at EOF or on error. *out_pts_ns is the block's
 * presentation timestamp in nanoseconds. *out_keyframe is true iff this
 * frame can be used as a seek target. */
JCE_API bool jce_webm_read_video_packet(JceWebmParser *p,
                                const uint8_t **out_data, size_t *out_size,
                                uint64_t *out_pts_ns, bool *out_keyframe);

/* Read the next audio packet. Same lifetime rules. */
JCE_API bool jce_webm_read_audio_packet(JceWebmParser *p,
                                const uint8_t **out_data, size_t *out_size,
                                uint64_t *out_pts_ns);

/* Coarse seek: rewind both readers to the first cluster whose timestamp
 * is <= the requested time. The next video/audio read returns frames
 * starting from there (the caller should drop pre-roll frames itself). */
JCE_API bool jce_webm_seek(JceWebmParser *p, uint64_t time_ns);

JCE_API void jce_webm_close(JceWebmParser *p);

JCE_EXTERN_C_END

#endif /* JCE_WEBM_PARSER_H */
