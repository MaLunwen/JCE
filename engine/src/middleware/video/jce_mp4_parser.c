/*
 * jce_mp4_parser.c  minimp4-backed MP4 metadata parser.
 */

#include <jce/middleware/video/jce_mp4_parser.h>

#ifdef _MSC_VER
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/core/jce_memory.h"

#define MP4D_INFO_SUPPORTED 1
#define MP4D_PRINT_INFO_SUPPORTED 0
#define MP4D_TRACE_SUPPORTED 0
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4018 4101 4244 4267 4310 4456 4996)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#define MINIMP4_IMPLEMENTATION
#include "middleware/video/third_party/minimp4.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    const unsigned char *data;
    size_t size;
} JceMp4Blob;

struct JceMp4Parser {
    JceMp4Blob  blob;
    MP4D_demux_t mp4;
    int         video_track_idx;
    int         audio_track_idx;
    JceMp4Info  info;
};

static void jce_mp4_set_error(JceMp4Info *out, const char *msg)
{
    if (!out || !msg) {
        return;
    }
    snprintf(out->error, sizeof(out->error), "%s", msg);
}

static int jce_mp4_read_cb(int64_t offset, void *buffer, size_t bytes, void *token)
{
    const JceMp4Blob *blob = (const JceMp4Blob *)token;
    size_t off = 0u;

    if (!blob || !blob->data || !buffer) {
        return -1;
    }
    if (offset < 0) {
        return -1;
    }

    off = (size_t)offset;
    if ((int64_t)off != offset) {
        return -1;
    }
    if (off > blob->size) {
        return -1;
    }
    if (bytes > (blob->size - off)) {
        return -1;
    }

    memcpy(buffer, blob->data + off, bytes);
    return 0;
}

static int jce_mp4_find_track(const MP4D_demux_t *mp4, unsigned handler_type)
{
    unsigned i;
    if (!mp4 || !mp4->track) {
        return -1;
    }

    for (i = 0u; i < mp4->track_count; ++i) {
        if (mp4->track[i].handler_type == handler_type) {
            return (int)i;
        }
    }
    return -1;
}

static void jce_mp4_codec_from_object_type(unsigned oti, char out_codec[5])
{
    if (!out_codec) {
        return;
    }

    out_codec[0] = '\0';

    switch (oti) {
        case MP4_OBJECT_TYPE_AVC:
            snprintf(out_codec, 5u, "avc1");
            break;
        case MP4_OBJECT_TYPE_HEVC:
            snprintf(out_codec, 5u, "hvc1");
            break;
        case MP4_OBJECT_TYPE_AV1:
            snprintf(out_codec, 5u, "av01");
            break;
        case MP4_OBJECT_TYPE_OPUS:
            snprintf(out_codec, 5u, "opus");
            break;
        case 0x20:
            snprintf(out_codec, 5u, "mp4v");
            break;
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_MAIN_PROFILE:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_LC_PROFILE:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_SSR_PROFILE:
            snprintf(out_codec, 5u, "mp4a");
            break;
        case 0x69:
        case 0x6B:
            snprintf(out_codec, 5u, "mp3 ");
            break;
        default:
            break;
    }
}

static double jce_mp4_duration_seconds(unsigned hi, unsigned lo, unsigned timescale)
{
    double ticks;
    if (timescale == 0u) {
        return 0.0;
    }
    ticks = ((double)hi * 4294967296.0) + (double)lo;
    if (ticks <= 0.0) {
        return 0.0;
    }
    return ticks / (double)timescale;
}

static int jce_mp4_find_audio_track(const MP4D_demux_t *mp4);
static void jce_mp4_fill_audio_info(const MP4D_track_t *audio_track,
                                    JceMp4Info *out);
static bool jce_mp4_fill_info(const MP4D_demux_t *mp4,
                              JceMp4Info *out,
                              int *out_video_idx,
                              int *out_audio_idx);
static bool jce_mp4_get_audio_track_info_internal(const MP4D_track_t *audio_track,
                                                  JceMp4AudioTrackInfo *out_info);

JceMp4Parser *jce_mp4_parser_open_memory(const void *data, size_t size,
                                         JceMp4Info *out_info)
{
    JceMp4Parser *parser;

    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
    }

    if (!data || size < 8u) {
        if (out_info) {
            jce_mp4_set_error(out_info, "input is empty or too small");
        }
        return NULL;
    }

    parser = (JceMp4Parser *)JCE_CALLOC(1u, sizeof(*parser));
    if (!parser) {
        if (out_info) {
            jce_mp4_set_error(out_info, "out of memory");
        }
        return NULL;
    }

    parser->blob.data = (const unsigned char *)data;
    parser->blob.size = size;
    parser->video_track_idx = -1;
    parser->audio_track_idx = -1;

    if (!MP4D_open(&parser->mp4,
                   jce_mp4_read_cb,
                   &parser->blob,
                   (int64_t)size)) {
        if (out_info) {
            jce_mp4_set_error(out_info, "invalid MP4 structure");
        }
        JCE_FREE(parser);
        return NULL;
    }

    if (!jce_mp4_fill_info(&parser->mp4, &parser->info,
                           &parser->video_track_idx,
                           &parser->audio_track_idx)) {
        if (out_info) {
            *out_info = parser->info;
        }
        MP4D_close(&parser->mp4);
        JCE_FREE(parser);
        return NULL;
    }

    if (out_info) {
        *out_info = parser->info;
    }
    return parser;
}

void jce_mp4_parser_close(JceMp4Parser *parser)
{
    if (!parser) {
        return;
    }
    MP4D_close(&parser->mp4);
    JCE_FREE(parser);
}

bool jce_mp4_parse_memory(const void *data, size_t size, JceMp4Info *out)
{
    JceMp4Parser *parser;

    if (!out) {
        return false;
    }

    parser = jce_mp4_parser_open_memory(data, size, out);
    if (!parser) {
        return false;
    }

    jce_mp4_parser_close(parser);
    return true;
}

bool jce_mp4_parser_get_audio_track_info(const JceMp4Parser *parser,
                                         JceMp4AudioTrackInfo *out_info)
{
    const MP4D_track_t *audio_track;

    if (!parser || !out_info) {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));
    if (parser->audio_track_idx < 0) {
        return false;
    }

    audio_track = &parser->mp4.track[parser->audio_track_idx];
    out_info->track_index = (uint32_t)parser->audio_track_idx;
    return jce_mp4_get_audio_track_info_internal(audio_track, out_info);
}

bool jce_mp4_parser_get_audio_sample(const JceMp4Parser *parser,
                                     uint32_t sample_index,
                                     JceMp4SampleInfo *out_sample)
{
    const MP4D_track_t *audio_track;
    unsigned frame_bytes = 0u;
    unsigned timestamp = 0u;
    unsigned duration = 0u;
    MP4D_file_offset_t offset;
    uint64_t off64;

    if (!parser || !out_sample) {
        return false;
    }
    memset(out_sample, 0, sizeof(*out_sample));

    if (parser->audio_track_idx < 0) {
        return false;
    }

    audio_track = &parser->mp4.track[parser->audio_track_idx];
    if (sample_index >= audio_track->sample_count) {
        return false;
    }

    offset = MP4D_frame_offset(&parser->mp4,
                               (unsigned)parser->audio_track_idx,
                               (unsigned)sample_index,
                               &frame_bytes,
                               &timestamp,
                               &duration);
    off64 = (uint64_t)offset;
    if (off64 > parser->blob.size || frame_bytes > (parser->blob.size - off64)) {
        return false;
    }

    out_sample->offset = off64;
    out_sample->size_bytes = frame_bytes;
    out_sample->timestamp = (uint64_t)timestamp;
    out_sample->duration = duration;
    return true;
}

bool jce_mp4_parser_copy_audio_sample(const JceMp4Parser *parser,
                                      uint32_t sample_index,
                                      void *dst,
                                      size_t dst_capacity,
                                      uint32_t *out_bytes)
{
    JceMp4SampleInfo sample;

    if (!jce_mp4_parser_get_audio_sample(parser, sample_index, &sample)) {
        return false;
    }

    if (out_bytes) {
        *out_bytes = sample.size_bytes;
    }

    if (!dst || dst_capacity < sample.size_bytes) {
        return false;
    }

    memcpy(dst,
            parser->blob.data + (size_t)sample.offset,
           sample.size_bytes);
    return true;
}

static bool jce_mp4_fill_info(const MP4D_demux_t *mp4,
                              JceMp4Info *out,
                              int *out_video_idx,
                              int *out_audio_idx)
{
    int video_idx;
    int audio_idx;
    const MP4D_track_t *video_track;
    const MP4D_track_t *audio_track;

    if (!mp4 || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->is_mp4 = true;
    out->fragmented = false;

    video_idx = jce_mp4_find_track(mp4, MP4D_HANDLER_TYPE_VIDE);
    audio_idx = jce_mp4_find_audio_track(mp4);

    out->has_video_track = (video_idx >= 0);
    out->has_audio_track = (audio_idx >= 0);

    if (!out->has_video_track && !out->has_audio_track) {
        jce_mp4_set_error(out, "no video or audio track found in MP4");
        return false;
    }

    if (video_idx >= 0) {
        video_track = &mp4->track[video_idx];

        out->video_track_id = (uint32_t)(video_idx + 1);
        out->width = video_track->SampleDescription.video.width;
        out->height = video_track->SampleDescription.video.height;
        out->sample_count = video_track->sample_count;
        out->keyframe_count = video_track->sample_count;

        out->duration_seconds = jce_mp4_duration_seconds(
            video_track->duration_hi,
            video_track->duration_lo,
            video_track->timescale);
        if (out->duration_seconds <= 0.0) {
            out->duration_seconds = jce_mp4_duration_seconds(
                mp4->duration_hi,
                mp4->duration_lo,
                mp4->timescale);
        }
        if (out->duration_seconds > 0.0 && out->sample_count > 0u) {
            out->framerate = (double)out->sample_count / out->duration_seconds;
        }

        jce_mp4_codec_from_object_type(video_track->object_type_indication,
                                       out->video_codec);
    }
    if (audio_idx >= 0) {
        audio_track = &mp4->track[audio_idx];
        out->audio_track_id = (uint32_t)(audio_idx + 1);
        jce_mp4_fill_audio_info(audio_track, out);

        if (out->duration_seconds <= 0.0) {
            out->duration_seconds = jce_mp4_duration_seconds(
                audio_track->duration_hi,
                audio_track->duration_lo,
                audio_track->timescale);
        }
    }

    if (out_video_idx) {
        *out_video_idx = video_idx;
    }
    if (out_audio_idx) {
        *out_audio_idx = audio_idx;
    }
    return true;
}

static bool jce_mp4_is_audio_object_type(unsigned oti)
{
    switch (oti) {
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_MAIN_PROFILE:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_LC_PROFILE:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_SSR_PROFILE:
        case 0x69: /* MPEG-2 Layer III */
        case 0x6B: /* MPEG-1 Layer III */
        case MP4_OBJECT_TYPE_OPUS: /* Opus in MP4 */
            return true;
        default:
            break;
    }
    return false;
}

static int jce_mp4_find_audio_track(const MP4D_demux_t *mp4)
{
    unsigned i;
    int best = -1;
    int best_score = -1;

    if (!mp4 || !mp4->track) {
        return -1;
    }

    for (i = 0u; i < mp4->track_count; ++i) {
        const MP4D_track_t *tr = &mp4->track[i];
        int score = 0;

        if (tr->handler_type != MP4D_HANDLER_TYPE_SOUN) {
            continue;
        }

        if (tr->sample_count > 0u) {
            score += 8;
        }
        if (tr->SampleDescription.audio.samplerate_hz > 0u) {
            score += 4;
        }
        if (tr->SampleDescription.audio.channelcount > 0u) {
            score += 2;
        }
        if (jce_mp4_is_audio_object_type(tr->object_type_indication)) {
            score += 1;
        }

        if (score > best_score) {
            best = (int)i;
            best_score = score;
        }
    }

    return best;
}

static bool jce_mp4_read_bits(const unsigned char *data,
                              size_t data_bytes,
                              size_t *bit_pos,
                              uint32_t bits,
                              uint32_t *out_value)
{
    size_t i;
    uint32_t value = 0u;

    if (!data || !bit_pos || !out_value || bits == 0u || bits > 24u) {
        return false;
    }
    if ((*bit_pos + (size_t)bits) > (data_bytes * 8u)) {
        return false;
    }

    for (i = 0u; i < (size_t)bits; ++i) {
        size_t abs_bit = *bit_pos + i;
        size_t byte_ix = abs_bit >> 3;
        uint32_t shift = 7u - (uint32_t)(abs_bit & 7u);
        value = (value << 1u) | (uint32_t)((data[byte_ix] >> shift) & 1u);
    }

    *bit_pos += (size_t)bits;
    *out_value = value;
    return true;
}

static bool jce_mp4_parse_aac_asc(const unsigned char *dsi,
                                   unsigned dsi_bytes,
                                   uint32_t *out_samplerate_hz,
                                   uint32_t *out_channels)
{
    static const uint32_t k_aac_sample_rates[13] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };
    static const uint8_t k_aac_channel_map[8] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u
    };

    size_t bit_pos = 0u;
    uint32_t aot = 0u;
    uint32_t sf_index = 0u;
    uint32_t explicit_rate = 0u;
    uint32_t ch_cfg = 0u;

    if (!dsi || dsi_bytes == 0u) {
        return false;
    }

    if (!jce_mp4_read_bits(dsi, (size_t)dsi_bytes, &bit_pos, 5u, &aot)) {
        return false;
    }
    if (aot == 31u) {
        uint32_t ext = 0u;
        if (!jce_mp4_read_bits(dsi, (size_t)dsi_bytes, &bit_pos, 6u, &ext)) {
            return false;
        }
        aot = 32u + ext;
    }

    if (!jce_mp4_read_bits(dsi, (size_t)dsi_bytes, &bit_pos, 4u, &sf_index)) {
        return false;
    }
    if (sf_index == 0xFu) {
        if (!jce_mp4_read_bits(dsi, (size_t)dsi_bytes, &bit_pos, 24u, &explicit_rate)) {
            return false;
        }
    }

    if (!jce_mp4_read_bits(dsi, (size_t)dsi_bytes, &bit_pos, 4u, &ch_cfg)) {
        return false;
    }

    (void)aot;

    if (out_samplerate_hz) {
        if (sf_index < 13u) {
            *out_samplerate_hz = k_aac_sample_rates[sf_index];
        } else if (sf_index == 0xFu) {
            *out_samplerate_hz = explicit_rate;
        } else {
            *out_samplerate_hz = 0u;
        }
    }

    if (out_channels) {
        if (ch_cfg < 8u) {
            *out_channels = (uint32_t)k_aac_channel_map[ch_cfg];
        } else {
            *out_channels = 0u;
        }
    }

    return true;
}

static void jce_mp4_fill_audio_info(const MP4D_track_t *audio_track,
                                    JceMp4Info *out)
{
    uint32_t samplerate = 0u;
    uint32_t channels = 0u;
    uint32_t asc_samplerate = 0u;
    uint32_t asc_channels = 0u;

    if (!audio_track || !out) {
        return;
    }

    samplerate = (uint32_t)audio_track->SampleDescription.audio.samplerate_hz;
    channels = (uint32_t)audio_track->SampleDescription.audio.channelcount;

    if (audio_track->dsi && audio_track->dsi_bytes > 0u) {
        (void)jce_mp4_parse_aac_asc(audio_track->dsi,
                                    audio_track->dsi_bytes,
                                    &asc_samplerate,
                                    &asc_channels);
    }

    if (samplerate == 0u) {
        if (asc_samplerate > 0u) {
            samplerate = asc_samplerate;
        } else if (audio_track->timescale > 0u) {
            samplerate = (uint32_t)audio_track->timescale;
        }
    }

    if (channels == 0u && asc_channels > 0u) {
        channels = asc_channels;
    }

    out->audio_samplerate_hz = samplerate;
    out->audio_channels = channels;

    jce_mp4_codec_from_object_type(audio_track->object_type_indication,
                                   out->audio_codec);

    if (!out->audio_codec[0] && asc_samplerate > 0u) {
        snprintf(out->audio_codec, 5u, "mp4a");
    }
}

static bool jce_mp4_get_audio_track_info_internal(const MP4D_track_t *audio_track,
                                                  JceMp4AudioTrackInfo *out_info)
{
    uint32_t samplerate = 0u;
    uint32_t channels = 0u;
    uint32_t asc_samplerate = 0u;
    uint32_t asc_channels = 0u;

    if (!audio_track || !out_info) {
        return false;
    }

    out_info->sample_count = audio_track->sample_count;
    out_info->timescale = audio_track->timescale;

    samplerate = (uint32_t)audio_track->SampleDescription.audio.samplerate_hz;
    channels = (uint32_t)audio_track->SampleDescription.audio.channelcount;

    if (audio_track->dsi && audio_track->dsi_bytes > 0u) {
        (void)jce_mp4_parse_aac_asc(audio_track->dsi,
                                    audio_track->dsi_bytes,
                                    &asc_samplerate,
                                    &asc_channels);
    }

    if (samplerate == 0u) {
        if (asc_samplerate > 0u) {
            samplerate = asc_samplerate;
        } else if (audio_track->timescale > 0u) {
            samplerate = (uint32_t)audio_track->timescale;
        }
    }
    if (channels == 0u && asc_channels > 0u) {
        channels = asc_channels;
    }

    out_info->samplerate_hz = samplerate;
    out_info->channels = channels;
    out_info->decoder_config = audio_track->dsi;
    out_info->decoder_config_bytes = audio_track->dsi_bytes;

    jce_mp4_codec_from_object_type(audio_track->object_type_indication,
                                   out_info->codec);
    if (!out_info->codec[0] && asc_samplerate > 0u) {
        snprintf(out_info->codec, sizeof(out_info->codec), "mp4a");
    }
    return true;
}

/* -- Video track sample access ---------------------------------------- */

bool jce_mp4_parser_get_video_track_info(const JceMp4Parser *parser,
                                         JceMp4VideoTrackInfo *out_info)
{
    const MP4D_track_t *vt;

    if (!parser || !out_info) {
        return false;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (parser->video_track_idx < 0) {
        return false;
    }

    vt = &parser->mp4.track[parser->video_track_idx];
    out_info->track_index = (uint32_t)parser->video_track_idx;
    out_info->sample_count = vt->sample_count;
    out_info->timescale = vt->timescale;
    out_info->width = vt->SampleDescription.video.width;
    out_info->height = vt->SampleDescription.video.height;
    out_info->decoder_config = vt->dsi;
    out_info->decoder_config_bytes = vt->dsi_bytes;

    jce_mp4_codec_from_object_type(vt->object_type_indication, out_info->codec);

    /* Parse NAL length size from avcC box: byte[4] bits[7:6]=reserved,
       bits[1:0]=lengthSizeMinusOne.  Default to 4 if missing. */
    if (vt->dsi && vt->dsi_bytes >= 7u
        && vt->object_type_indication == MP4_OBJECT_TYPE_AVC) {
        out_info->nal_length_size = (uint8_t)((vt->dsi[4] & 0x03u) + 1u);
    } else if (vt->dsi && vt->dsi_bytes >= 23u
               && vt->object_type_indication == MP4_OBJECT_TYPE_HEVC) {
        /* hvcC: byte[21] bits[1:0]=lengthSizeMinusOne */
        out_info->nal_length_size = (uint8_t)((vt->dsi[21] & 0x03u) + 1u);
    } else {
        out_info->nal_length_size = 4u;
    }
    return true;
}

bool jce_mp4_parser_get_video_sample(const JceMp4Parser *parser,
                                     uint32_t sample_index,
                                     JceMp4SampleInfo *out_sample)
{
    const MP4D_track_t *vt;
    unsigned frame_bytes = 0u;
    unsigned timestamp = 0u;
    unsigned duration = 0u;
    MP4D_file_offset_t offset;
    uint64_t off64;

    if (!parser || !out_sample) {
        return false;
    }
    memset(out_sample, 0, sizeof(*out_sample));

    if (parser->video_track_idx < 0) {
        return false;
    }

    vt = &parser->mp4.track[parser->video_track_idx];
    if (sample_index >= vt->sample_count) {
        return false;
    }

    offset = MP4D_frame_offset(&parser->mp4,
                               (unsigned)parser->video_track_idx,
                               (unsigned)sample_index,
                               &frame_bytes,
                               &timestamp,
                               &duration);
    off64 = (uint64_t)offset;
    if (off64 > parser->blob.size || frame_bytes > (parser->blob.size - off64)) {
        return false;
    }

    out_sample->offset = off64;
    out_sample->size_bytes = frame_bytes;
    out_sample->timestamp = (uint64_t)timestamp;
    out_sample->duration = duration;
    return true;
}

bool jce_mp4_parser_copy_video_sample(const JceMp4Parser *parser,
                                      uint32_t sample_index,
                                      void *dst,
                                      size_t dst_capacity,
                                      uint32_t *out_bytes)
{
    JceMp4SampleInfo sample;

    if (!jce_mp4_parser_get_video_sample(parser, sample_index, &sample)) {
        return false;
    }

    if (out_bytes) {
        *out_bytes = sample.size_bytes;
    }

    if (!dst || dst_capacity < sample.size_bytes) {
        return false;
    }

    memcpy(dst,
           parser->blob.data + (size_t)sample.offset,
           sample.size_bytes);
    return true;
}