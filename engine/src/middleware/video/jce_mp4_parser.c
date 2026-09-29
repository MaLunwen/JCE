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

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* Upstream defines its options inside the declaration guard. Include that
 * first, then configure only its implementation through the wrapper. */
#include <minimp4.h>
#undef MP4D_AVC_SUPPORTED
#undef MP4D_HEVC_SUPPORTED
#define MP4D_AVC_SUPPORTED 0
#define MP4D_HEVC_SUPPORTED 0
#define MINIMP4_IMPLEMENTATION
#include <minimp4.h>
#undef MINIMP4_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "jce_mp4_source.h"

typedef struct {
    JceReadSource *source;
    uint64_t size;
    unsigned char *metadata;
    size_t metadata_size;
    uint64_t metadata_offset;
} JceMp4Blob;

/* Private dispatch values for codecs without an MPEG-4 object type. */
#define JCE_MP4_OBJECT_AV1  0xA1u
#define JCE_MP4_OBJECT_VP9  0xA2u
#define JCE_MP4_OBJECT_OPUS 0xADu

/* ── Fragmented MP4 (moof / traf / trun) sample index ───────────────── *
 *
 * minimp4 reads sample tables out of moov/trak/stbl only.  A fragmented
 * file (CMAF, DASH, anything an adaptive streamer produced) carries an
 * *empty* stbl in moov and puts the real sample records in a moof box in
 * front of every mdat, so minimp4 reports sample_count == 0 and the very
 * first jce_mp4_parser_get_video_sample() fails.  jce_video.cpp treats
 * that as "no decode backend" and drops to metadata-only — after it has
 * already successfully opened the H.264/H.265 decoder, which is why the
 * symptom reads as a codec problem rather than a container one.
 *
 * This builds the missing index by walking the fragments ourselves and
 * leaves the rest of the file (and minimp4) untouched for progressive
 * MP4s.  ISO/IEC 14496-12 §8.8.
 */

#define JCE_MP4_FRAG_MAX_SAMPLES 4000000u   /* hostile-input backstop */

typedef struct {
    uint64_t offset;     /* absolute byte offset of the sample payload */
    uint32_t size;
    uint64_t dts;        /* track timescale units */
    int64_t  cts_offset; /* signed composition offset, not decode order */
    uint32_t duration;   /* track timescale units */
    bool     sync;       /* random-access point */
} JceMp4FragSample;

typedef struct {
    uint32_t          track_id;
    uint32_t          count;
    uint32_t          cap;
    JceMp4FragSample *samples;
    uint64_t          next_dts;      /* running DTS when tfdt is absent */
    uint32_t          trex_duration; /* mvex/trex per-track defaults */
    uint32_t          trex_size;
    uint32_t          trex_flags;
    int64_t           pts_base;
} JceMp4FragTrack;

struct JceMp4Parser {
    JceMp4Blob  blob;
    MP4D_demux_t mp4;
    int         video_track_idx;
    int         audio_track_idx;
    JceMp4Info  info;
    /* Reconstruct progressive stts timestamps beyond minimp4's 32-bit limit. */
    uint64_t   *video_dts64;
    uint64_t   *audio_dts64;
    uint64_t   *video_pts64;
    bool       *borrowed_config;
    const unsigned char *sync_samples;
    uint32_t sync_count;
    bool sync_present;

    /* Present only for fragmented files; parallel to mp4.track[]. */
    bool             fragmented;
    uint32_t         frag_count;
    JceMp4FragTrack *frag;
};

static uint64_t *jce_mp4_build_dts64(const MP4D_track_t *track)
{
    uint64_t *index;
    uint64_t tick = 0u;
    uint32_t i;
    if (!track || !track->duration || track->sample_count == 0u
        || track->sample_count > JCE_MP4_FRAG_MAX_SAMPLES)
        return NULL;
    index = (uint64_t *)JCE_MALLOC((size_t)track->sample_count * sizeof(*index));
    if (!index) return NULL;
    for (i = 0u; i < track->sample_count; ++i) {
        index[i] = tick;
        tick += track->duration[i];
    }
    return index;
}

#define JCE_MP4_BOX(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  |  (uint32_t)(d))

static uint32_t jce_mp4_rd_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t jce_mp4_rd_u64(const unsigned char *p)
{
    return ((uint64_t)jce_mp4_rd_u32(p) << 32) | (uint64_t)jce_mp4_rd_u32(p + 4);
}

/* Decode the box header at `off`. On success reports the box type, the
 * offset of its payload, and the offset one past the box. Every field is
 * bounds-checked against `end`: this parses untrusted bytes. */
static bool jce_mp4_box_at(const unsigned char *d, uint64_t off, uint64_t end,
                           uint32_t *out_type, uint64_t *out_body,
                           uint64_t *out_next)
{
    uint64_t size;
    uint64_t body;

    if (off + 8u > end || off > end) {
        return false;
    }
    size = (uint64_t)jce_mp4_rd_u32(d + off);
    *out_type = jce_mp4_rd_u32(d + off + 4u);
    body = off + 8u;

    if (size == 1u) {                      /* 64-bit largesize */
        if (body + 8u > end) {
            return false;
        }
        size = jce_mp4_rd_u64(d + body);
        body += 8u;
    } else if (size == 0u) {               /* extends to end of file */
        size = end - off;
    }

    /* `off + size > end` would be the natural check and it is WRONG: size is
     * an attacker-controlled 64-bit largesize, so the addition wraps. A box
     * at off=0x200 declaring largesize 2^64-0x200 makes off+size == 0, which
     * passes, and *out_next then points BACKWARDS — every caller does
     * `off = next` inside `while (off < end)` and spins forever at 100% CPU
     * while re-pushing the same samples until the allocation backstop.
     * Subtract instead of adding, and require forward progress explicitly so
     * no future arithmetic slip can reintroduce a non-monotonic walk. */
    if (size < (body - off) || size > end - off) {
        return false;
    }
    if (off + size <= off) {
        return false;
    }
    *out_body = body;
    *out_next = off + size;
    return true;
}

static bool jce_mp4_find_box(const unsigned char *d, uint64_t start,
                            uint64_t end, uint32_t wanted,
                            uint64_t *body, uint64_t *next)
{
    uint32_t type;
    while (start < end && jce_mp4_box_at(d, start, end, &type, body, next)) {
        if (type == wanted) return true;
        start = *next;
    }
    return false;
}

/* Extend pristine minimp4 through the JCE wrapper, never through vendor
 * patches. Sample tables remain upstream-owned. Codec records are borrowed
 * from the same immutable input blob as the compressed samples. */
static bool jce_mp4_apply_sample_entry(const unsigned char *d, uint64_t body,
                                       uint64_t end, uint32_t type,
                                       MP4D_track_t *track, bool *borrowed)
{
    uint32_t config, object;
    uint64_t header = 78u, cb, ce;
    unsigned min_config;
    switch (type) {
        case JCE_MP4_BOX('a','v','c','1'):
        case JCE_MP4_BOX('a','v','c','3'):
            config = JCE_MP4_BOX('a','v','c','C');
            object = MP4_OBJECT_TYPE_AVC; min_config = 7u; break;
        case JCE_MP4_BOX('h','v','c','1'):
        case JCE_MP4_BOX('h','e','v','1'):
            config = JCE_MP4_BOX('h','v','c','C');
            object = MP4_OBJECT_TYPE_HEVC; min_config = 23u; break;
        case JCE_MP4_BOX('a','v','0','1'):
            config = JCE_MP4_BOX('a','v','1','C');
            object = JCE_MP4_OBJECT_AV1; min_config = 4u; break;
        case JCE_MP4_BOX('v','p','0','9'):
            config = JCE_MP4_BOX('v','p','c','C');
            object = JCE_MP4_OBJECT_VP9; min_config = 12u; break;
        case JCE_MP4_BOX('O','p','u','s'):
            config = JCE_MP4_BOX('d','O','p','s'); header = 28u;
            object = JCE_MP4_OBJECT_OPUS; min_config = 11u; break;
        default: return true;
    }
    if (end - body < header ||
        !jce_mp4_find_box(d, body + header, end, config, &cb, &ce) ||
        ce - cb < min_config || ce - cb > UINT32_MAX || track->dsi)
        return false;
    if (header == 78u) {
        track->SampleDescription.video.width =
            ((unsigned)d[body + 24u] << 8) | d[body + 25u];
        track->SampleDescription.video.height =
            ((unsigned)d[body + 26u] << 8) | d[body + 27u];
    } else {
        track->SampleDescription.audio.channelcount = d[cb + 1u];
        /* Opus always decodes at 48 kHz; dOps InputSampleRate is advisory. */
        track->SampleDescription.audio.samplerate_hz = 48000u;
    }
    track->object_type_indication = object;
    track->dsi = (unsigned char *)(d + cb);
    track->dsi_bytes = (unsigned)(ce - cb);
    *borrowed = true;
    return true;
}

static bool jce_mp4_apply_codec_records(JceMp4Parser *parser)
{
    static const uint32_t path[] = {
        JCE_MP4_BOX('m','d','i','a'), JCE_MP4_BOX('m','i','n','f'),
        JCE_MP4_BOX('s','t','b','l'), JCE_MP4_BOX('s','t','s','d')
    };
    const unsigned char *d = parser->blob.metadata;
    uint64_t mb, me, off, body, next;
    uint32_t type, index = 0u;
    if (!jce_mp4_find_box(d, 0u, parser->blob.metadata_size,
                          JCE_MP4_BOX('m','o','o','v'), &mb, &me)) return false;
    parser->borrowed_config = (bool *)JCE_CALLOC(parser->mp4.track_count,
                                                sizeof(bool));
    if (!parser->borrowed_config) return false;
    off = mb;
    while (off < me && jce_mp4_box_at(d, off, me, &type, &body, &next)) {
        if (type == JCE_MP4_BOX('t','r','a','k')) {
            uint64_t b = body, e = next, sb, se;
            unsigned level;
            if (index >= parser->mp4.track_count) return false;
            for (level = 0u; level < sizeof(path) / sizeof(path[0]); ++level) {
                if (!jce_mp4_find_box(d, b, e, path[level], &sb, &se)) break;
                b = sb; e = se;
            }
            if (level == sizeof(path) / sizeof(path[0]) && e - b >= 8u &&
                jce_mp4_rd_u32(d + b + 4u) > 0u) {
                if (!jce_mp4_box_at(d, b + 8u, e, &type, &sb, &se) ||
                    !jce_mp4_apply_sample_entry(d, sb, se, type,
                        &parser->mp4.track[index], &parser->borrowed_config[index]))
                    return false;
            }
            ++index;
        }
        off = next;
    }
    return index == parser->mp4.track_count;
}

/* CTTS is decode-order indexed; decoded pictures return their own PTS.
 * Keep DTS for seek indexing and normalize the presentation origin once. */
static bool jce_mp4_build_video_pts(JceMp4Parser *parser)
{
    static const uint32_t path[] = {
        JCE_MP4_BOX('m','d','i','a'), JCE_MP4_BOX('m','i','n','f'),
        JCE_MP4_BOX('s','t','b','l')
    };
    const unsigned char *d = parser->blob.metadata;
    uint64_t mb, me, pos, body, next;
    uint32_t type, index = 0u, count;
    if (parser->video_track_idx < 0) return true;
    count = parser->mp4.track[parser->video_track_idx].sample_count;
    if (!count) return true;
    if (!parser->video_dts64) return false;
    parser->video_pts64 = (uint64_t *)JCE_MALLOC((size_t)count * sizeof(uint64_t));
    if (!parser->video_pts64) return false;
    memcpy(parser->video_pts64, parser->video_dts64, (size_t)count * sizeof(uint64_t));
    if (!jce_mp4_find_box(d, 0u, parser->blob.metadata_size,
        JCE_MP4_BOX('m','o','o','v'), &mb, &me)) return false;
    pos = mb;
    while (pos < me && jce_mp4_box_at(d, pos, me, &type, &body, &next)) {
        if (type == JCE_MP4_BOX('t','r','a','k') && index++ == (uint32_t)parser->video_track_idx) {
            uint64_t b = body, e = next, cb, ce;
            unsigned level;
            for (level = 0u; level < 3u; ++level) {
                if (!jce_mp4_find_box(d, b, e, path[level], &cb, &ce)) return false;
                b = cb; e = ce;
            }
            if (jce_mp4_find_box(d, b, e, JCE_MP4_BOX('c','t','t','s'), &cb, &ce)) {
                if (!jce_mp4_composition_index(parser->video_dts64, count,
                    d + cb, (size_t)(ce - cb), parser->video_pts64)) return false;
            }
            return true;
        }
        pos = next;
    }
    return false;
}

static void jce_mp4_cache_sync(JceMp4Parser *parser)
{
    static const uint32_t path[] = {
        JCE_MP4_BOX('m','d','i','a'), JCE_MP4_BOX('m','i','n','f'),
        JCE_MP4_BOX('s','t','b','l'), JCE_MP4_BOX('s','t','s','s')
    };
    const unsigned char *d = parser->blob.metadata;
    uint64_t mb, me, off, b, e, next;
    uint32_t type, index = 0u;
    if (parser->video_track_idx < 0 || !jce_mp4_find_box(d,0u,
        parser->blob.metadata_size,JCE_MP4_BOX('m','o','o','v'),&mb,&me)) return;
    off=mb;
    while (off < me && jce_mp4_box_at(d,off,me,&type,&b,&next)) {
        if (type == JCE_MP4_BOX('t','r','a','k') && index++ == (uint32_t)parser->video_track_idx) {
            unsigned level;
            e=next;
            for (level=0u; level<4u; ++level) {
                uint64_t sb,se;
                if (!jce_mp4_find_box(d,b,e,path[level],&sb,&se)) return;
                b=sb; e=se;
            }
            if (e-b < 8u) return;
            parser->sync_count=jce_mp4_rd_u32(d+b+4u);
            if (parser->sync_count > (e-b-8u)/4u) return;
            parser->sync_samples=d+b+8u;
            parser->sync_present=true;
            return;
        }
        off=next;
    }
}

static void jce_mp4_close_demux(JceMp4Parser *parser)
{
    unsigned i;
    if (parser->borrowed_config) {
        for (i = 0u; i < parser->mp4.track_count; ++i) {
            if (parser->borrowed_config[i]) parser->mp4.track[i].dsi = NULL;
        }
        JCE_FREE(parser->borrowed_config);
        parser->borrowed_config = NULL;
    }
    MP4D_close(&parser->mp4);
}

/* Collect track_IDs in moov order, so index i in mp4.track[] maps to
 * ids[i]. minimp4 fills its track array in trak order, and there is no
 * track_id in MP4D_track_t to read back. */
static uint32_t jce_mp4_frag_track_ids(const unsigned char *d, uint64_t size,
                                       uint32_t *ids, uint32_t max_ids)
{
    uint64_t off = 0u, body = 0u, next = 0u;
    uint32_t type = 0u;
    uint32_t n = 0u;

    while (off < size && jce_mp4_box_at(d, off, size, &type, &body, &next)) {
        if (type == JCE_MP4_BOX('m', 'o', 'o', 'v')) {
            uint64_t t = body;
            uint64_t tb = 0u, tn = 0u;
            uint32_t tt = 0u;
            while (t < next && jce_mp4_box_at(d, t, next, &tt, &tb, &tn)) {
                if (tt == JCE_MP4_BOX('t', 'r', 'a', 'k')) {
                    uint64_t k = tb;
                    uint64_t kb = 0u, kn = 0u;
                    uint32_t kt = 0u;
                    while (k < tn && jce_mp4_box_at(d, k, tn, &kt, &kb, &kn)) {
                        if (kt == JCE_MP4_BOX('t', 'k', 'h', 'd')
                            && kb + 4u <= kn) {
                            unsigned ver = d[kb];
                            /* fullbox(4) + creation/modification, then ID */
                            uint64_t idp = kb + 4u + (ver == 1u ? 16u : 8u);
                            if (idp + 4u <= kn && n < max_ids) {
                                ids[n++] = jce_mp4_rd_u32(d + idp);
                            }
                            break;
                        }
                        k = kn;
                    }
                }
                t = tn;
            }
            return n;
        }
        off = next;
    }
    return n;
}

static JceMp4FragTrack *jce_mp4_frag_find(JceMp4FragTrack *tracks,
                                          uint32_t count, uint32_t track_id)
{
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        if (tracks[i].track_id == track_id) {
            return &tracks[i];
        }
    }
    return NULL;
}

/* mvex/trex per-track defaults (ISO/IEC 14496-12 §8.8.3). */
static void jce_mp4_frag_read_trex(const unsigned char *d, uint64_t size,
                                   JceMp4FragTrack *tracks, uint32_t count)
{
    uint64_t off = 0u, body = 0u, next = 0u;
    uint32_t type = 0u;

    while (off < size && jce_mp4_box_at(d, off, size, &type, &body, &next)) {
        if (type == JCE_MP4_BOX('m', 'o', 'o', 'v')) {
            uint64_t t = body, tb = 0u, tn = 0u;
            uint32_t tt = 0u;
            while (t < next && jce_mp4_box_at(d, t, next, &tt, &tb, &tn)) {
                if (tt == JCE_MP4_BOX('m', 'v', 'e', 'x')) {
                    uint64_t e = tb, eb = 0u, en = 0u;
                    uint32_t et = 0u;
                    while (e < tn && jce_mp4_box_at(d, e, tn, &et, &eb, &en)) {
                        if (et == JCE_MP4_BOX('t', 'r', 'e', 'x')
                            && eb + 24u <= en) {
                            JceMp4FragTrack *tr = jce_mp4_frag_find(
                                tracks, count, jce_mp4_rd_u32(d + eb + 4u));
                            if (tr) {
                                tr->trex_duration = jce_mp4_rd_u32(d + eb + 12u);
                                tr->trex_size     = jce_mp4_rd_u32(d + eb + 16u);
                                tr->trex_flags    = jce_mp4_rd_u32(d + eb + 20u);
                            }
                        }
                        e = en;
                    }
                }
                t = tn;
            }
            return;
        }
        off = next;
    }
}

static bool jce_mp4_frag_push(JceMp4FragTrack *tr, const JceMp4FragSample *s)
{
    if (tr->count == tr->cap) {
        uint32_t cap = tr->cap ? (tr->cap * 2u) : 256u;
        JceMp4FragSample *grown;
        if (cap > JCE_MP4_FRAG_MAX_SAMPLES) {
            cap = JCE_MP4_FRAG_MAX_SAMPLES;
        }
        if (cap == tr->cap) {
            return false;                  /* backstop reached */
        }
        grown = (JceMp4FragSample *)JCE_REALLOC(tr->samples,
                                                (size_t)cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        tr->samples = grown;
        tr->cap = cap;
    }
    tr->samples[tr->count++] = *s;
    return true;
}

/* One traf: tfhd (per-fragment defaults) + optional tfdt (base DTS) +
 * one or more trun (the sample records themselves). */
static void jce_mp4_frag_read_traf(const unsigned char *d,
                                   uint64_t traf_body, uint64_t traf_end,
                                   uint64_t moof_off, uint64_t file_size,
                                   JceMp4FragTrack *tracks, uint32_t count,
                                   uint64_t *moof_data_end,
                                   uint32_t *total, uint32_t total_cap)
{
    uint64_t off = traf_body, body = 0u, next = 0u;
    uint32_t type = 0u;
    JceMp4FragTrack *tr = NULL;
    uint32_t tf_flags = 0u;
    uint64_t base_offset = moof_off;
    uint32_t def_duration = 0u, def_size = 0u, def_flags = 0u;
    bool have_def_duration = false, have_def_size = false, have_def_flags = false;
    uint64_t dts = 0u;
    bool have_tfdt = false;
    uint64_t run_cursor = 0u;              /* next byte after previous trun */
    bool have_cursor = false;

    /* Pass 1: tfhd and tfdt must be known before any trun is expanded. */
    while (off < traf_end && jce_mp4_box_at(d, off, traf_end, &type, &body,
                                            &next)) {
        if (type == JCE_MP4_BOX('t', 'f', 'h', 'd') && body + 8u <= next) {
            uint64_t p = body + 4u;
            tf_flags = jce_mp4_rd_u32(d + body) & 0x00FFFFFFu;
            tr = jce_mp4_frag_find(tracks, count, jce_mp4_rd_u32(d + p));
            p += 4u;

            /* The optional fields are positional: each present flag consumes
             * its bytes and shifts the next one. Skipping a field that does
             * not fit (rather than bailing) would make every LATER field read
             * from the truncated one's bytes — a short base_data_offset would
             * silently become the default_sample_size. A tfhd that lies about
             * its own length is not recoverable; drop the whole traf. */
            if (tf_flags & 0x000001u) {                       /* base-data-offset */
                if (p + 8u > next) { return; }
                base_offset = jce_mp4_rd_u64(d + p);
                p += 8u;
            } else if (tf_flags & 0x020000u) {                /* default-base-is-moof */
                base_offset = moof_off;
            } else {
                /* ISO/IEC 14496-12 §8.8.7: absent both flags, the base is the
                 * first byte of the enclosing moof for the FIRST track
                 * fragment, and the end of the previous track fragment's data
                 * for any later one. Using moof_off for both aliased track N
                 * onto track N-1's bytes in a multi-track fragment. */
                base_offset = (*moof_data_end != 0u) ? *moof_data_end : moof_off;
            }
            if (tf_flags & 0x000002u) {                       /* sample-desc-index */
                if (p + 4u > next) { return; }
                p += 4u;
            }
            if (tf_flags & 0x000008u) {
                if (p + 4u > next) { return; }
                def_duration = jce_mp4_rd_u32(d + p); p += 4u;
                have_def_duration = true;
            }
            if (tf_flags & 0x000010u) {
                if (p + 4u > next) { return; }
                def_size = jce_mp4_rd_u32(d + p); p += 4u;
                have_def_size = true;
            }
            if (tf_flags & 0x000020u) {
                if (p + 4u > next) { return; }
                def_flags = jce_mp4_rd_u32(d + p); p += 4u;
                have_def_flags = true;
            }
        } else if (type == JCE_MP4_BOX('t', 'f', 'd', 't') && body + 8u <= next) {
            unsigned ver = d[body];
            if (ver == 1u && body + 12u <= next) {
                dts = jce_mp4_rd_u64(d + body + 4u);
                have_tfdt = true;
            } else if (ver == 0u) {
                dts = (uint64_t)jce_mp4_rd_u32(d + body + 4u);
                have_tfdt = true;
            }
        }
        off = next;
    }

    if (!tr) {
        return;
    }
    /* Fall back to the trex defaults on ABSENCE, not on a zero value: a tfhd
     * may legitimately signal default_sample_duration == 0 (an empty-duration
     * fragment), and testing the value would silently override it. */
    if (!have_def_duration) def_duration = tr->trex_duration;
    if (!have_def_size)     def_size     = tr->trex_size;
    if (!have_def_flags)    def_flags    = tr->trex_flags;
    if (!have_tfdt) {
        dts = tr->next_dts;
    }

    /* Pass 2: expand every trun. */
    off = traf_body;
    while (off < traf_end && jce_mp4_box_at(d, off, traf_end, &type, &body,
                                            &next)) {
        if (type == JCE_MP4_BOX('t', 'r', 'u', 'n') && body + 8u <= next) {
            uint32_t tr_flags = jce_mp4_rd_u32(d + body) & 0x00FFFFFFu;
            uint32_t n = jce_mp4_rd_u32(d + body + 4u);
            uint64_t p = body + 8u;
            uint64_t data = have_cursor ? run_cursor : base_offset;
            uint32_t first_flags = 0u;
            bool have_first = false;
            uint32_t i;

            if (tr_flags & 0x000001u) {                       /* data-offset */
                if (p + 4u > next) { off = next; continue; }
                data = (uint64_t)((int64_t)base_offset
                                  + (int32_t)jce_mp4_rd_u32(d + p));
                p += 4u;
            }
            if (tr_flags & 0x000004u) {                       /* first-sample-flags */
                if (p + 4u > next) { off = next; continue; }
                first_flags = jce_mp4_rd_u32(d + p);
                have_first = true;
                p += 4u;
            }

            /* sample_count is a raw attacker-controlled uint32. When the trun
             * carries per-sample fields the box size is the real bound, so
             * clamp to what actually fits. When it carries NONE, the loop body
             * reads nothing and cannot run off the end of the box — a 16-byte
             * trun declaring 2^32-1 samples would push zero-length records
             * until the per-track backstop, ~128 MB per track and ~8 GB with
             * 64 tracks. The shared budget below bounds that case. */
            {
                uint32_t stride = 0u;
                if (tr_flags & 0x000100u) stride += 4u;
                if (tr_flags & 0x000200u) stride += 4u;
                if (tr_flags & 0x000400u) stride += 4u;
                if (tr_flags & 0x000800u) stride += 4u;
                if (stride > 0u) {
                    uint64_t room = (next > p) ? (next - p) : 0u;
                    uint64_t fit  = room / stride;
                    if ((uint64_t)n > fit) {
                        n = (uint32_t)fit;
                    }
                }
            }

            for (i = 0u; i < n; ++i) {
                if (*total >= total_cap) {
                    tr->next_dts = dts;
                    return;
                }
                JceMp4FragSample s;
                uint32_t dur = def_duration;
                uint32_t sz  = def_size;
                uint32_t fl  = def_flags;
                int64_t cts_offset = 0;

                if (tr_flags & 0x000100u) {
                    if (p + 4u > next) break;
                    dur = jce_mp4_rd_u32(d + p); p += 4u;
                }
                if (tr_flags & 0x000200u) {
                    if (p + 4u > next) break;
                    sz = jce_mp4_rd_u32(d + p); p += 4u;
                }
                if (tr_flags & 0x000400u) {
                    if (p + 4u > next) break;
                    fl = jce_mp4_rd_u32(d + p); p += 4u;
                }
                if (tr_flags & 0x000800u) {                   /* cts offset */
                    uint32_t raw;
                    if (p + 4u > next) break;
                    raw = jce_mp4_rd_u32(d + p);
                    cts_offset = d[body] ? (int64_t)(int32_t)raw : (int64_t)raw;
                    p += 4u;
                }
                if (i == 0u && have_first) {
                    fl = first_flags;
                }

                if (data > file_size || (uint64_t)sz > file_size - data) {
                    break;                 /* truncated / lying fragment */
                }

                if (dts > (uint64_t)INT64_MAX - UINT32_MAX) break;
                s.offset   = data;
                s.size     = sz;
                s.dts      = dts;
                s.cts_offset = cts_offset;
                s.duration = dur;
                /* §8.8.3.1: bit 16 is sample_is_non_sync_sample, and
                 * sample_depends_on == 2 means "depends on nothing" (an
                 * I-frame). Either one makes this a seek point. */
                s.sync = ((fl & 0x00010000u) == 0u)
                       || (((fl >> 24) & 0x03u) == 2u);

                if (!jce_mp4_frag_push(tr, &s)) {
                    tr->next_dts = dts;
                    return;
                }
                ++(*total);
                data += sz;
                dts  += dur;
            }
            run_cursor = data;
            have_cursor = true;
            if (data > *moof_data_end) {
                *moof_data_end = data;
            }
        }
        off = next;
    }
    tr->next_dts = dts;
}

/* Walk every top-level moof and build the per-track sample index.
 * Returns true when the file is fragmented AND at least one sample was
 * recovered. */
static bool jce_mp4_frag_build(JceMp4Parser *parser)
{
    const unsigned char *d = parser->blob.metadata;
    uint64_t size = (uint64_t)parser->blob.size;
    uint64_t off = 0u, body = 0u, next = 0u;
    uint32_t type = 0u;
    uint32_t ids[64];
    uint32_t n_ids;
    uint32_t i;
    uint32_t total = 0u;
    bool saw_moof = false;
    /* Shared allocation budget across ALL tracks, tied to the input size: a
     * real sample costs bytes in the file, so a few KB of boxes must not be
     * able to demand hundreds of MB of index. The honest 4.3 MB measured file
     * needs 3240 entries and gets a ceiling of ~542k. */
    uint32_t total_cap;
    uint64_t budget = (size / 8u) + 1024u;
    if (budget > (uint64_t)JCE_MP4_FRAG_MAX_SAMPLES) {
        budget = (uint64_t)JCE_MP4_FRAG_MAX_SAMPLES;
    }
    total_cap = (uint32_t)budget;

    /* Cheap pre-scan: no moof means nothing to do, and this must be decided
     * before any allocation so progressive files pay nothing. */
    while (off < size && jce_mp4_source_box(parser->blob.source, off, &type, &body, &next)) {
        if (type == JCE_MP4_BOX('m', 'o', 'o', 'f')) {
            saw_moof = true;
            break;
        }
        off = next;
    }
    if (!saw_moof) {
        return false;
    }
    off = 0u;

    n_ids = jce_mp4_frag_track_ids(d, parser->blob.metadata_size, ids, 64u);
    if (n_ids == 0u) {
        return false;
    }
    /* minimp4's track array and the moov trak order must line up for the
     * id mapping to mean anything. If they do not, index nothing rather
     * than index the wrong track. */
    if (n_ids != parser->mp4.track_count) {
        return false;
    }

    parser->frag = (JceMp4FragTrack *)JCE_CALLOC(n_ids, sizeof(*parser->frag));
    if (!parser->frag) {
        return false;
    }
    parser->frag_count = n_ids;
    for (i = 0u; i < n_ids; ++i) {
        parser->frag[i].track_id = ids[i];
    }

    jce_mp4_frag_read_trex(d, parser->blob.metadata_size, parser->frag, n_ids);

    /* Hybrid layout (ffmpeg -movflags +frag_keyframe WITHOUT +empty_moov):
     * moov/stbl describes the first run of samples and the moofs describe the
     * rest. Seed from stbl so the fragment records append after them; taking
     * only one of the two sources ends playback at the first seam. Pure
     * fragmented files have sample_count == 0 here and skip this entirely. */
    for (i = 0u; i < n_ids; ++i) {
        const MP4D_track_t *mt = &parser->mp4.track[i];
        unsigned s;
        for (s = 0u; s < mt->sample_count; ++s) {
            JceMp4FragSample smp;
            unsigned fb = 0u, ts = 0u, du = 0u;
            MP4D_file_offset_t o;
            if (total >= total_cap) {
                break;
            }
            o = MP4D_frame_offset(&parser->mp4, i, s, &fb, &ts, &du);
            if ((uint64_t)o > size || (uint64_t)fb > size - (uint64_t)o) {
                break;
            }
            smp.offset   = (uint64_t)o;
            smp.size     = (uint32_t)fb;
            smp.dts      = (uint64_t)ts;
            smp.cts_offset = (int)i == parser->video_track_idx && parser->video_pts64
                ? (int64_t)parser->video_pts64[s] - (int64_t)ts : 0;
            smp.duration = du;
            /* stbl sync flags are not exposed by minimp4; keyframe_count is
             * the only consumer of this bit and it is advisory. */
            smp.sync     = true;
            if (!jce_mp4_frag_push(&parser->frag[i], &smp)) {
                break;
            }
            ++total;
            parser->frag[i].next_dts = (uint64_t)ts + du;
        }
    }

    while (off < size && jce_mp4_source_box(parser->blob.source, off, &type, &body, &next)) {
        if (type == JCE_MP4_BOX('m', 'o', 'o', 'f')) {
            const size_t header = (size_t)(body-off);
            size_t length;
            unsigned char *fragment;
            uint64_t t, tb = 0u, tn = 0u;
            uint32_t tt = 0u;
            uint64_t moof_data_end = 0u;
            if (next-off > 4u*1024u*1024u) break;
            length = (size_t)(next-off);
            fragment = JCE_MALLOC(length);
            if (!fragment) break;
            if (jce_read_source_read_at(parser->blob.source,off,fragment,length) != length) {
                JCE_FREE(fragment);
                break;
            }
            t = header;
            while (t < length && jce_mp4_box_at(fragment,t,length,&tt,&tb,&tn)) {
                if (tt == JCE_MP4_BOX('t','r','a','f'))
                    jce_mp4_frag_read_traf(fragment,tb,tn,off,size,
                                           parser->frag,n_ids,&moof_data_end,
                                           &total,total_cap);
                t = tn;
            }
            JCE_FREE(fragment);
        }
        off = next;
    }

    /* Rebase each track's DTS to zero. tfdt carries baseMediaDecodeTime on the
     * MEDIA timeline, so a segment taken from the middle of a stream starts at
     * a large value; a progressive file's stts always starts at 0. Consumers
     * compare sample timestamps against a playback clock that starts at 0, so
     * leaving the raw value in freezes on frame 1 and makes every seek land at
     * sample 0. */
    for (i = 0u; i < n_ids; ++i) {
        JceMp4FragTrack *ft = &parser->frag[i];
        uint64_t base;
        uint32_t k;
        if (ft->count == 0u) {
            continue;
        }
        base = ft->samples[0].dts;
        ft->pts_base = INT64_MAX;
        for (k = 0u; k < ft->count; ++k) {
            ft->samples[k].dts = (ft->samples[k].dts >= base)
                                     ? (ft->samples[k].dts - base) : 0u;
            {
                int64_t pts = (int64_t)ft->samples[k].dts + ft->samples[k].cts_offset;
                if (pts < ft->pts_base) ft->pts_base = pts;
            }
        }
    }

    total = 0u;
    for (i = 0u; i < n_ids; ++i) {
        total += parser->frag[i].count;
    }
    if (total == 0u) {
        for (i = 0u; i < n_ids; ++i) {
            JCE_FREE(parser->frag[i].samples);
        }
        JCE_FREE(parser->frag);
        parser->frag = NULL;
        parser->frag_count = 0u;
        return false;
    }
    return true;
}

static const JceMp4FragTrack *jce_mp4_frag_track(const JceMp4Parser *parser,
                                                 int track_idx)
{
    if (!parser->fragmented || !parser->frag || track_idx < 0
        || (uint32_t)track_idx >= parser->frag_count) {
        return NULL;
    }
    /* A track that appears in moov but in no moof gets an EMPTY entry here.
     * Reporting it as "fragment-backed with 0 samples" made every request for
     * it fail and, worse, suppressed the minimp4 path that could still serve
     * it from its own stbl. An empty index is not an index. */
    if (parser->frag[track_idx].count == 0u) {
        return NULL;
    }
    return &parser->frag[track_idx];
}

/* Serve one sample out of the fragment index. Returns false when this is
 * not a fragmented file (caller falls back to minimp4). */
static bool jce_mp4_frag_sample(const JceMp4Parser *parser, int track_idx,
                                uint32_t sample_index,
                                JceMp4SampleInfo *out, bool *out_valid)
{
    const JceMp4FragTrack *tr = jce_mp4_frag_track(parser, track_idx);
    const JceMp4FragSample *s;

    *out_valid = false;
    if (!tr) {
        return false;
    }
    if (sample_index >= tr->count) {
        return true;                       /* handled, but out of range */
    }
    s = &tr->samples[sample_index];
    out->offset = s->offset;
    out->size_bytes = s->size;
    out->timestamp = s->dts;
    out->duration = s->duration;
    *out_valid = true;
    return true;
}

static void jce_mp4_set_error(JceMp4Info *out, const char *msg)
{
    if (!out || !msg) {
        return;
    }
    snprintf(out->error, sizeof(out->error), "%s", msg);
}

static size_t jce_mp4_blob_read(const JceMp4Blob *blob, uint64_t offset,
                                void *buffer, size_t bytes)
{
    if (offset >= blob->metadata_offset &&
        offset-blob->metadata_offset <= blob->metadata_size &&
        bytes <= blob->metadata_size-(size_t)(offset-blob->metadata_offset)) {
        memcpy(buffer,blob->metadata+(size_t)(offset-blob->metadata_offset),bytes);
        return bytes;
    }
    return jce_read_source_read_at(blob->source,offset,buffer,bytes);
}

static int jce_mp4_read_cb(int64_t offset, void *buffer, size_t bytes, void *token)
{
    const JceMp4Blob *blob = token;
    unsigned char type[4];
    uint64_t off;
    if (!blob || !buffer || offset < 0) return -1;
    off = (uint64_t)offset;
    if (off > blob->size || bytes > blob->size-off ||
        jce_mp4_blob_read(blob,off,buffer,bytes) != bytes) return -1;
    /* Normalize only the unsupported ctts version byte in JCE's callback. */
    if (bytes == 1u && off >= 4u && *(unsigned char *)buffer == 1u &&
        jce_mp4_blob_read(blob,off-4u,type,4u) == 4u &&
        memcmp(type,"ctts",4u) == 0) *(unsigned char *)buffer = 0u;
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
        case JCE_MP4_OBJECT_AV1:
            snprintf(out_codec, 5u, "av01");
            break;
        case JCE_MP4_OBJECT_VP9:
            snprintf(out_codec, 5u, "vp09");
            break;
        case JCE_MP4_OBJECT_OPUS:
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

JceMp4Parser *jce_mp4_parser_open_source(JceReadSource *source,
                                         JceMp4Info *out_info)
{
    JceMp4Parser *parser;
    uint64_t size = jce_read_source_size(source);
    if (out_info) memset(out_info,0,sizeof(*out_info));
    if (!source || size < 8u) {
        jce_mp4_set_error(out_info,"input is empty or too small");
        return NULL;
    }
    parser = JCE_CALLOC(1u,sizeof(*parser));
    if (!parser) return NULL;
    parser->blob.source = jce_read_source_acquire(source);
    parser->blob.size = size;
    parser->blob.metadata = jce_mp4_source_moov(source,
        &parser->blob.metadata_offset,&parser->blob.metadata_size);
    if (!parser->blob.source || !parser->blob.metadata) {
        jce_mp4_set_error(out_info,"invalid or over-budget MP4 metadata");
        jce_mp4_parser_close(parser);
        return NULL;
    }
    parser->video_track_idx = -1;
    parser->audio_track_idx = -1;

    if (!MP4D_open(&parser->mp4,
                   jce_mp4_read_cb,
                   &parser->blob,
                   (int64_t)size)) {
        if (out_info) {
            jce_mp4_set_error(out_info, "invalid MP4 structure");
        }
        jce_mp4_parser_close(parser);
        return NULL;
    }

    if (!jce_mp4_apply_codec_records(parser)) {
        if (out_info) jce_mp4_set_error(out_info, "invalid MP4 codec configuration");
        jce_mp4_parser_close(parser);
        return NULL;
    }

    if (!jce_mp4_fill_info(&parser->mp4, &parser->info,
                           &parser->video_track_idx,
                           &parser->audio_track_idx)) {
        if (out_info) {
            *out_info = parser->info;
        }
        jce_mp4_parser_close(parser);
        return NULL;
    }

    jce_mp4_cache_sync(parser);
    if (parser->video_track_idx >= 0)
        parser->video_dts64 = jce_mp4_build_dts64(
            &parser->mp4.track[parser->video_track_idx]);
    if (parser->audio_track_idx >= 0)
        parser->audio_dts64 = jce_mp4_build_dts64(
            &parser->mp4.track[parser->audio_track_idx]);

    if (!jce_mp4_build_video_pts(parser)) {
        if (out_info) jce_mp4_set_error(out_info, "invalid MP4 composition times");
        jce_mp4_parser_close(parser);
        return NULL;
    }

    /* Fragmented files carry an empty stbl, so minimp4 reports zero samples
     * for every track. Recover the real index from the moof chain before
     * anyone asks for sample 0. Attempted unconditionally, not just when the
     * video track's stbl is empty: an audio-only or hybrid file has a
     * non-zero count there and still needs the fragments. frag_build
     * pre-scans for a moof and costs a progressive file one box walk. */
    parser->fragmented = jce_mp4_frag_build(parser);
    if (parser->fragmented) {
        const JceMp4FragTrack *vt = jce_mp4_frag_track(parser,
                                                       parser->video_track_idx);
        const JceMp4FragTrack *at = jce_mp4_frag_track(parser,
                                                       parser->audio_track_idx);
        parser->info.fragmented = true;
        if (vt) {
            uint32_t i;
            uint32_t keys = 0u;
            uint64_t ticks = 0u;
            unsigned ts = parser->mp4.track[parser->video_track_idx].timescale;

            for (i = 0u; i < vt->count; ++i) {
                if (vt->samples[i].sync) {
                    ++keys;
                }
                ticks += vt->samples[i].duration;
            }
            parser->info.sample_count = vt->count;
            parser->info.keyframe_count = keys ? keys : 1u;
            /* mvhd/tkhd duration is often 0 in an init segment; the summed
             * sample durations are the honest answer. */
            if (ts > 0u && ticks > 0u) {
                double secs = (double)ticks / (double)ts;
                if (parser->info.duration_seconds <= 0.0) {
                    parser->info.duration_seconds = secs;
                }
                if (secs > 0.0) {
                    parser->info.framerate = (double)vt->count / secs;
                }
            }
        }
        if (at && parser->info.duration_seconds <= 0.0) {
            uint32_t i;
            uint64_t ticks = 0u;
            unsigned ts = parser->mp4.track[parser->audio_track_idx].timescale;
            for (i = 0u; i < at->count; ++i) {
                ticks += at->samples[i].duration;
            }
            if (ts > 0u && ticks > 0u) {
                parser->info.duration_seconds = (double)ticks / (double)ts;
            }
        }
    }

    if (out_info) {
        *out_info = parser->info;
    }
    return parser;
}

JceMp4Parser *jce_mp4_parser_open_memory(const void *data, size_t size,
                                         JceMp4Info *out_info)
{
    JceReadSource *source = jce_read_source_open_memory(data,size,false);
    JceMp4Parser *parser = jce_mp4_parser_open_source(source,out_info);
    jce_read_source_close(source);
    return parser;
}

void jce_mp4_parser_close(JceMp4Parser *parser)
{
    if (!parser) {
        return;
    }
    JCE_FREE(parser->video_dts64);
    JCE_FREE(parser->audio_dts64);
    JCE_FREE(parser->video_pts64);
    if (parser->frag) {
        uint32_t i;
        for (i = 0u; i < parser->frag_count; ++i) {
            JCE_FREE(parser->frag[i].samples);
        }
        JCE_FREE(parser->frag);
    }
    jce_mp4_close_demux(parser);
    JCE_FREE(parser->blob.metadata);
    jce_read_source_close(parser->blob.source);
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
    if (!jce_mp4_get_audio_track_info_internal(audio_track, out_info)) {
        return false;
    }
    {   /* fragmented: stbl is empty, the count lives in the moof chain */
        const JceMp4FragTrack *ft = jce_mp4_frag_track(parser,
                                                       parser->audio_track_idx);
        if (ft) {
            out_info->sample_count = ft->count;
        }
    }
    return true;
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

    {
        bool valid = false;
        if (jce_mp4_frag_sample(parser, parser->audio_track_idx, sample_index,
                                out_sample, &valid)) {
            return valid;
        }
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
    out_sample->timestamp = parser->audio_dts64
        ? parser->audio_dts64[sample_index] : (uint64_t)timestamp;
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

    return jce_read_source_read_at(parser->blob.source,sample.offset,dst,
                                    sample.size_bytes) == sample.size_bytes;
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
        case JCE_MP4_OBJECT_OPUS: /* Opus in MP4 */
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
    {   /* fragmented: stbl is empty, the count lives in the moof chain */
        const JceMp4FragTrack *ft = jce_mp4_frag_track(parser,
                                                       parser->video_track_idx);
        if (ft) {
            out_info->sample_count = ft->count;
        }
    }
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

    {
        bool valid = false;
        if (jce_mp4_frag_sample(parser, parser->video_track_idx, sample_index,
                                out_sample, &valid)) {
            return valid;
        }
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
    out_sample->timestamp = parser->video_dts64
        ? parser->video_dts64[sample_index] : (uint64_t)timestamp;
    out_sample->duration = duration;
    return true;
}

bool jce_mp4_parser_video_presentation_time(const JceMp4Parser *parser,
                                            uint32_t index, uint64_t *out)
{
    const JceMp4FragTrack *track;
    if (!parser || !out || parser->video_track_idx < 0) return false;
    track = jce_mp4_frag_track(parser, parser->video_track_idx);
    if (parser->fragmented && track) {
        const JceMp4FragSample *sample;
        if (index >= track->count) return false;
        sample = &track->samples[index];
        *out = (uint64_t)((int64_t)sample->dts + sample->cts_offset - track->pts_base);
        return true;
    }
    if (!parser->video_pts64 || index >= parser->mp4.track[parser->video_track_idx].sample_count)
        return false;
    *out = parser->video_pts64[index];
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

    return jce_read_source_read_at(parser->blob.source,sample.offset,dst,
                                    sample.size_bytes) == sample.size_bytes;
}

bool jce_mp4_parser_video_sample_sync(const JceMp4Parser *parser,
                                       uint32_t index, bool *out_sync)
{
    JceMp4SampleInfo info;
    const JceMp4FragTrack *track;
    uint32_t lo=0u, hi;
    if (!out_sync || !jce_mp4_parser_get_video_sample(parser,index,&info)) return false;
    track=jce_mp4_frag_track(parser,parser->video_track_idx);
    if (parser->fragmented && track) { *out_sync=track->samples[index].sync; return true; }
    if (!parser->sync_present) { *out_sync=true; return true; }
    hi=parser->sync_count;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2u;
        uint32_t sample=jce_mp4_rd_u32(parser->sync_samples+(size_t)mid*4u);
        if (sample < index+1u) lo=mid+1u; else hi=mid;
    }
    *out_sync=lo<parser->sync_count &&
        jce_mp4_rd_u32(parser->sync_samples+(size_t)lo*4u)==index+1u;
    return true;
}
