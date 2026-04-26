/*
 * jce_mp4_parser.h  minimp4-backed MP4 metadata parser.
 *
 * This wrapper keeps a stable JCE-facing API while delegating ISO-BMFF
 * parsing to minimp4. It extracts common fields needed by the
 * editor/runtime pipeline: video/audio track presence, codec tag,
 * geometry, duration, frame-rate estimate, sample count, and keyframe
 * estimate.
 */

#ifndef JCE_MP4_PARSER_H
#define JCE_MP4_PARSER_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    bool     is_mp4;
    bool     has_video_track;
    bool     has_audio_track;
    bool     fragmented;

    uint32_t video_track_id;
    uint32_t audio_track_id;
    uint32_t width;
    uint32_t height;
    uint32_t audio_samplerate_hz;
    uint32_t audio_channels;

    double   duration_seconds;
    double   framerate;

    uint32_t sample_count;
    uint32_t keyframe_count;

    char     video_codec[5];
    char     audio_codec[5];

    char     error[128];
} JceMp4Info;

typedef struct JceMp4Parser JceMp4Parser;

typedef struct {
    uint32_t    track_index;     /* zero-based index in MP4D_demux_t::track */
    uint32_t    sample_count;
    uint32_t    timescale;
    uint32_t    samplerate_hz;
    uint32_t    channels;
    char        codec[5];
    const void *decoder_config;  /* AudioSpecificConfig / DSI blob */
    uint32_t    decoder_config_bytes;
} JceMp4AudioTrackInfo;

typedef struct {
    uint64_t offset;      /* absolute byte offset in the MP4 blob */
    uint32_t size_bytes;
    uint64_t timestamp;   /* in track timescale units */
    uint32_t duration;    /* in track timescale units */
} JceMp4SampleInfo;

typedef struct {
    uint32_t    track_index;
    uint32_t    sample_count;
    uint32_t    timescale;
    uint32_t    width;
    uint32_t    height;
    char        codec[5];
    const void *decoder_config;   /* avcC/hvcC blob */
    uint32_t    decoder_config_bytes;
    uint8_t     nal_length_size;  /* 1,2 or 4 — from avcC lengthSizeMinusOne+1 */
} JceMp4VideoTrackInfo;

/* Parse an MP4/ISO-BMFF file from a full in-memory blob.
 * Returns true when parsing succeeds and at least one video track
 * was identified. On failure, out->error contains a short reason. */
bool jce_mp4_parse_memory(const void *data, size_t size, JceMp4Info *out);

/* Open a reusable parser context over an in-memory MP4 blob.
 * The input blob must remain valid until jce_mp4_parser_close().
 * Returns NULL on parse failure. */
JceMp4Parser *jce_mp4_parser_open_memory(const void *data, size_t size,
                                         JceMp4Info *out_info);

void jce_mp4_parser_close(JceMp4Parser *parser);

/* Query the selected primary audio track metadata.
 * Returns false when no audio track is present. */
bool jce_mp4_parser_get_audio_track_info(const JceMp4Parser *parser,
                                         JceMp4AudioTrackInfo *out_info);

/* Resolve one audio sample by index.
 * Returns false when index is out of bounds or no audio track exists. */
bool jce_mp4_parser_get_audio_sample(const JceMp4Parser *parser,
                                     uint32_t sample_index,
                                     JceMp4SampleInfo *out_sample);

/* Copy a single encoded audio sample into caller memory.
 * out_bytes receives the required/written byte count.
 * Returns false when buffer is too small, out of bounds, or no audio track exists. */
bool jce_mp4_parser_copy_audio_sample(const JceMp4Parser *parser,
                                      uint32_t sample_index,
                                      void *dst,
                                      size_t dst_capacity,
                                      uint32_t *out_bytes);

/* -- Video track sample access ---------------------------------------- */

/* Query the selected primary video track metadata.
 * Returns false when no video track is present. */
bool jce_mp4_parser_get_video_track_info(const JceMp4Parser *parser,
                                         JceMp4VideoTrackInfo *out_info);

/* Resolve one video sample by index.
 * Returns false when index is out of bounds or no video track exists. */
bool jce_mp4_parser_get_video_sample(const JceMp4Parser *parser,
                                     uint32_t sample_index,
                                     JceMp4SampleInfo *out_sample);

/* Copy a single encoded video sample into caller memory.
 * Returns false when buffer is too small or out of bounds. */
bool jce_mp4_parser_copy_video_sample(const JceMp4Parser *parser,
                                      uint32_t sample_index,
                                      void *dst,
                                      size_t dst_capacity,
                                      uint32_t *out_bytes);

JCE_EXTERN_C_END

#endif /* JCE_MP4_PARSER_H */