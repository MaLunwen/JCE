/*
 * jce_webm_parser.cpp  Source-backed WebM/Matroska demuxer using libwebm.
 *
 * Implements the C ABI declared in jce_webm_parser.h on top of libwebm's
 * mkvparser (BSD-3, royalty-free). One concrete IMkvReader subclass wraps
 * bounded random-access input; everything else just walks Cluster→Block→Frame.
 */

#include <jce/middleware/video/jce_webm_parser.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
}

#include <mkvparser/mkvparser.h>

#include <cstring>
#include <new>
#include <string>

#define LOG_TAG "jce_webm"

namespace {

// ── Source-backed IMkvReader ──────────────────────────────────────────
class SourceReader : public mkvparser::IMkvReader {
public:
    explicit SourceReader(JceReadSource *source)
        : m_source(jce_read_source_acquire(source)),
          m_size((long long)jce_read_source_size(source)) {}
    ~SourceReader() override { jce_read_source_close(m_source); }

    int Read(long long pos, long len, unsigned char *buf) override {
        if (pos < 0 || len < 0 || pos > m_size || len > m_size-pos || len > 32*1024*1024) return -1;
        return jce_read_source_read_at(m_source,(uint64_t)pos,buf,(size_t)len)
                   == (size_t)len ? 0 : -1;
    }
    int Length(long long *total, long long *available) override {
        if (total)     *total     = m_size;
        if (available) *available = m_size;
        return 0;
    }
private:
    JceReadSource *m_source;
    long long      m_size;
};

struct TrackCursor {
    long          track_number = 0; // 1-based Matroska track number
    const mkvparser::Cluster *cluster = nullptr;
    const mkvparser::BlockEntry *entry = nullptr;
    int           frame_idx = 0;     // index into Block::GetFrameCount()
    bool          reached_eof = false;
    long long     last_cluster_time = -1; // monotonic guard across calls (ns or tc units)
};

JceWebmVideoCodec map_video_codec(const std::string &id) {
    if (id == "V_VP8")           return JCE_WEBM_VIDEO_VP8;
    if (id == "V_VP9")           return JCE_WEBM_VIDEO_VP9;
    if (id == "V_AV1")           return JCE_WEBM_VIDEO_AV1;
    return JCE_WEBM_VIDEO_NONE;
}
JceWebmAudioCodec map_audio_codec(const std::string &id) {
    if (id == "A_OPUS")          return JCE_WEBM_AUDIO_OPUS;
    if (id == "A_VORBIS")        return JCE_WEBM_AUDIO_VORBIS;
    return JCE_WEBM_AUDIO_NONE;
}

} // namespace

struct JceWebmParser {
    SourceReader            *reader = nullptr;
    mkvparser::Segment   *segment = nullptr;
    long long             timecode_scale_ns = 1000000;// default 1ms
    JceWebmVideoCodec     vcodec = JCE_WEBM_VIDEO_NONE;
    JceWebmAudioCodec     acodec = JCE_WEBM_AUDIO_NONE;

    // Per-track cursors.
    TrackCursor video;
    TrackCursor audio;

    // Audio codec-private data (held by libwebm; we just cache pointer/size).
    const unsigned char *acodec_private      = nullptr;
    size_t               acodec_private_size = 0;

    // Output frame staging (we copy small frames into this buffer so the
    // returned pointer outlives libwebm's internal state).
    unsigned char *frame_buf = nullptr;
    size_t         frame_cap = 0;
    uint32_t accounted[65536] = {};
    uint64_t retained_entries = 0;
    bool metadata_failed = false;
};

bool jce_webm_is_webm(const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    if (!p || size < 4u) return false;
    // EBML header magic: 0x1A 0x45 0xDF 0xA3
    return p[0] == 0x1A && p[1] == 0x45 && p[2] == 0xDF && p[3] == 0xA3;
}

/* libwebm retains parsed block entries. Cap their aggregate as well as clusters. */
static bool account_cluster(JceWebmParser *p,const mkvparser::Cluster *cluster)
{
    const long index=cluster->GetIndex(), entries=cluster->GetEntryCount();
    if (p->metadata_failed || index<0 || index>=65536 || entries<0) return false;
    const uint32_t count=(uint32_t)entries;
    if (count>p->accounted[index]) {
        p->retained_entries+=count-p->accounted[index];
        p->accounted[index]=count;
    }
    if (p->retained_entries>1000000u) {
        p->metadata_failed=true;
        LOG_ERROR(LOG_TAG,"WebM retained block index exceeds budget");
        return false;
    }
    return true;
}

static const mkvparser::Cluster *next_cluster(JceWebmParser *p,
                                              const mkvparser::Cluster *current)
{
    if (!p || !p->segment || !current) return nullptr;
    if (current == p->segment->GetLast()) {
        /* A fixed cluster budget also bounds libwebm's retained index. */
        if (p->segment->GetCount() >= 65536 || p->segment->LoadCluster() < 0)
            return nullptr;
    }
    return p->segment->GetNext(current);
}

static bool advance_to_next_block(JceWebmParser *p, TrackCursor &cur)
{
    if (!p || !p->segment || cur.track_number == 0) return false;
    auto *seg = p->segment;
    const long total_clusters = seg->GetCount();
    long step_budget = total_clusters > 0 ? total_clusters + 4 : 1024;

    auto step_to_next_cluster = [&](void) -> bool {
        const mkvparser::Cluster *nxt = next_cluster(p,cur.cluster);
        if (!nxt || nxt == cur.cluster || nxt->EOS()) return false;
        long long nt = nxt->GetTime();
        /* Cluster timecodes must be monotonically non-decreasing.
         * If libwebm hands us a cluster whose timecode is <= the highest
         * we've already passed, we're cycling — treat as EOF. */
        if (cur.last_cluster_time >= 0 && nt <= cur.last_cluster_time)
            return false;
        cur.cluster = nxt;
        cur.last_cluster_time = nt;
        cur.entry = nullptr;
        cur.frame_idx = 0;
        return true;
    };

    if (cur.cluster == nullptr) {
        if (cur.reached_eof) return false;
        cur.cluster = seg->GetFirst();
        if (!cur.cluster || cur.cluster->EOS()) { cur.reached_eof = true; return false; }
        cur.last_cluster_time = cur.cluster->GetTime();
        cur.entry = nullptr;
        cur.frame_idx = 0;
    } else if (cur.cluster->EOS()) {
        cur.reached_eof = true;
        return false;
    }

    while (cur.cluster && !cur.cluster->EOS()) {
        if (cur.entry == nullptr) {
            long st = cur.cluster->GetFirst(cur.entry);
            if (!account_cluster(p,cur.cluster)) return false;
            if (st < 0 || cur.entry == nullptr) {
                if (--step_budget <= 0) { cur.reached_eof = true; return false; }
                if (!step_to_next_cluster()) { cur.reached_eof = true; return false; }
                continue;
            }
        }
        while (cur.entry && !cur.entry->EOS()) {
            const mkvparser::Block *blk = cur.entry->GetBlock();
            if (blk && blk->GetTrackNumber() == cur.track_number) {
                if (cur.frame_idx < blk->GetFrameCount()) {
                    return true; // ready to emit
                }
            }
            const mkvparser::BlockEntry *next = nullptr;
            long st = cur.cluster->GetNext(cur.entry, next);
            if (!account_cluster(p,cur.cluster)) return false;
            if (st < 0) { cur.entry = nullptr; break; }
            cur.entry = next;
            cur.frame_idx = 0;
        }
        if (--step_budget <= 0) { cur.reached_eof = true; return false; }
        if (!step_to_next_cluster()) { cur.reached_eof = true; return false; }
    }
    cur.reached_eof = true;
    return false;
}

static bool emit_frame(JceWebmParser *p, TrackCursor &cur,
                       const uint8_t **out_data, size_t *out_size,
                       uint64_t *out_pts_ns, bool *out_keyframe)
{
    const mkvparser::Block *blk = cur.entry->GetBlock();
    const mkvparser::Block::Frame &f = blk->GetFrame(cur.frame_idx);
    if (f.len <= 0 || f.len > 16*1024*1024) return false;

    if ((size_t)f.len > p->frame_cap) {
        size_t new_cap = (size_t)f.len;
        unsigned char *nb = (unsigned char *)JCE_REALLOC(p->frame_buf, new_cap);
        if (!nb) return false;
        p->frame_buf = nb;
        p->frame_cap = new_cap;
    }
    if (f.Read(p->reader, p->frame_buf) < 0) return false;

    if (out_data)     *out_data     = p->frame_buf;
    if (out_size)     *out_size     = (size_t)f.len;
    if (out_pts_ns)   *out_pts_ns   = (uint64_t)blk->GetTime(cur.cluster);
    if (out_keyframe) *out_keyframe = blk->IsKey();

    // Advance for next call.
    cur.frame_idx++;
    if (cur.frame_idx >= blk->GetFrameCount()) {
        const mkvparser::BlockEntry *next = nullptr;
        long st = cur.cluster->GetNext(cur.entry, next);
        if (!account_cluster(p,cur.cluster)) return false;
        if (st < 0 || next == nullptr || next->EOS()) {
            // End of this cluster — eagerly step to the next cluster
            // (with monotonic-time guard) instead of leaving entry=null
            // on the same cluster, which would make the next
            // advance_to_next_block call GetFirst() and re-walk this
            // cluster's entries forever.
            const mkvparser::Cluster *nxt =
                next_cluster(p,cur.cluster);
            if (!nxt || nxt == cur.cluster || nxt->EOS()) {
                cur.cluster = nullptr;
                cur.reached_eof = true;
            } else {
                long long nt = nxt->GetTime();
                if (cur.last_cluster_time >= 0 && nt <= cur.last_cluster_time) {
                    cur.cluster = nullptr;
                    cur.reached_eof = true;
                } else {
                    cur.cluster = nxt;
                    cur.last_cluster_time = nt;
                }
            }
            cur.entry = nullptr;
            cur.frame_idx = 0;
        } else {
            cur.entry = next;
            cur.frame_idx = 0;
        }
    }
    return true;
}

JceWebmParser *jce_webm_open_source(JceReadSource *source,
                                    JceWebmInfo *out_info)
{
    uint8_t magic[4];
    if (out_info) memset(out_info,0,sizeof(*out_info));
    if (jce_read_source_read_at(source,0u,magic,4u) != 4u ||
        !jce_webm_is_webm(magic,4u)) return nullptr;

    void *raw = jce_malloc(sizeof(JceWebmParser));
    if (!raw) return nullptr;
    JceWebmParser *p = new (raw) JceWebmParser();

    p->reader = new (std::nothrow) SourceReader(source);
    if (!p->reader) { p->~JceWebmParser(); jce_free(p); return nullptr; }

    long long pos = 0;
    mkvparser::EBMLHeader hdr;
    if (hdr.Parse(p->reader, pos) < 0) {
        LOG_WARN(LOG_TAG, "EBML header parse failed");
        jce_webm_close(p); return nullptr;
    }

    if (mkvparser::Segment::CreateInstance(p->reader, pos, p->segment) < 0
        || !p->segment) {
        LOG_WARN(LOG_TAG, "Segment::CreateInstance failed");
        jce_webm_close(p); return nullptr;
    }
    if (p->segment->ParseHeaders() < 0 || p->segment->LoadCluster() < 0) {
        LOG_WARN(LOG_TAG, "Segment::Load failed");
        jce_webm_close(p); return nullptr;
    }

    const mkvparser::SegmentInfo *si = p->segment->GetInfo();
    if (si && si->GetTimeCodeScale() > 0)
        p->timecode_scale_ns = si->GetTimeCodeScale();

    JceWebmInfo info{};
    info.duration_ns = si ? (uint64_t)si->GetDuration() : 0;

    const mkvparser::Tracks *tracks = p->segment->GetTracks();
    if (tracks) {
        for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
            const mkvparser::Track *t = tracks->GetTrackByIndex(i);
            if (!t) continue;
            const char *codec_id = t->GetCodecId();
            std::string cid = codec_id ? codec_id : "";

            if (t->GetType() == mkvparser::Track::kVideo
                && info.video_codec == JCE_WEBM_VIDEO_NONE)
            {
                JceWebmVideoCodec vc = map_video_codec(cid);
                if (vc != JCE_WEBM_VIDEO_NONE) {
                    info.video_codec = vc;
                    p->vcodec = vc;
                    p->video.track_number = t->GetNumber();
                    auto *vt = static_cast<const mkvparser::VideoTrack *>(t);
                    info.width  = (uint32_t)vt->GetWidth();
                    info.height = (uint32_t)vt->GetHeight();
                }
            } else if (t->GetType() == mkvparser::Track::kAudio
                && info.audio_codec == JCE_WEBM_AUDIO_NONE)
            {
                JceWebmAudioCodec ac = map_audio_codec(cid);
                if (ac != JCE_WEBM_AUDIO_NONE) {
                    info.audio_codec = ac;
                    p->acodec = ac;
                    p->audio.track_number = t->GetNumber();
                    auto *at = static_cast<const mkvparser::AudioTrack *>(t);
                    info.audio_channels   = (uint32_t)at->GetChannels();
                    info.audio_samplerate = (uint32_t)at->GetSamplingRate();

                    size_t cp_size = 0;
                    const unsigned char *cp = t->GetCodecPrivate(cp_size);
                    p->acodec_private      = cp;
                    p->acodec_private_size = cp_size;
                }
            }
        }
    }

    if (out_info) *out_info = info;

    LOG_SUCCESS(LOG_TAG, "WebM opened: vcodec=%d %ux%u | acodec=%d %uch %uHz | dur=%llu ns",
                (int)info.video_codec, info.width, info.height,
                (int)info.audio_codec, info.audio_channels, info.audio_samplerate,
                (unsigned long long)info.duration_ns);
    return p;
}

JceWebmParser *jce_webm_open_memory(const void *data, size_t size,
                                    JceWebmInfo *out_info)
{
    JceReadSource *source = jce_read_source_open_memory(data,size,false);
    JceWebmParser *parser = jce_webm_open_source(source,out_info);
    jce_read_source_close(source);
    return parser;
}

bool jce_webm_get_audio_codec_private(JceWebmParser *p,
                                      const uint8_t **out_data, size_t *out_size)
{
    if (!p || !p->acodec_private || p->acodec_private_size == 0) return false;
    if (out_data) *out_data = p->acodec_private;
    if (out_size) *out_size = p->acodec_private_size;
    return true;
}

bool jce_webm_read_video_packet(JceWebmParser *p,
                                const uint8_t **out_data, size_t *out_size,
                                uint64_t *out_pts_ns, bool *out_keyframe)
{
    if (!p || p->vcodec == JCE_WEBM_VIDEO_NONE) return false;
    if (!advance_to_next_block(p, p->video)) return false;
    return emit_frame(p, p->video, out_data, out_size, out_pts_ns, out_keyframe);
}

bool jce_webm_read_audio_packet(JceWebmParser *p,
                                const uint8_t **out_data, size_t *out_size,
                                uint64_t *out_pts_ns)
{
    if (!p || p->acodec == JCE_WEBM_AUDIO_NONE) return false;
    if (!advance_to_next_block(p, p->audio)) return false;
    bool kf = false;
    return emit_frame(p, p->audio, out_data, out_size, out_pts_ns, &kf);
}

bool jce_webm_seek(JceWebmParser *p, uint64_t time_ns)
{
    if (!p || !p->segment) return false;
    while (p->segment->GetLast() &&
           (uint64_t)p->segment->GetLast()->GetTime() < time_ns) {
        const auto *last = p->segment->GetLast();
        const auto *next = next_cluster(p,last);
        if (!next || next->EOS() || next == last) break;
    }
    auto *cl = p->segment->FindCluster((long long)time_ns);
    if (!cl) return false;
    p->video.cluster = cl;  p->video.entry = nullptr; p->video.frame_idx = 0;
    p->video.reached_eof = false;
    p->video.last_cluster_time = cl->GetTime();
    p->audio.cluster = cl;  p->audio.entry = nullptr; p->audio.frame_idx = 0;
    p->audio.reached_eof = false;
    p->audio.last_cluster_time = cl->GetTime();
    return true;
}

void jce_webm_close(JceWebmParser *p)
{
    if (!p) return;
    delete p->segment;
    delete p->reader;
    if (p->frame_buf) JCE_FREE(p->frame_buf);
    p->~JceWebmParser();
    jce_free(p);
}
