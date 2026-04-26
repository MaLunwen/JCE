/*
 * jce_audio_stream.cpp  Generic streaming audio source impl.
 *
 * Architecture:
 *
 *      [worker thread]                       [audio device thread]
 *           │                                         │
 *           │  desc.decode_next(out, max)             │  pull(out, n)
 *           │  ──────────►  ring  ──────────►         │
 *           │             (mutex)                     │
 *           │                                         │
 *           ▼                                         ▼
 *      block on `wake` semaphore                read up to `n` frames
 *      until ring < lo-water (or              update frames_pulled
 *      seek/quit signaled)
 *
 * The ring stores interleaved s16 frames in a contiguous int16_t array
 * sized `ring_capacity_frames * channels * sizeof(int16_t)`.
 *
 * Seek:
 *   - main thread calls jce_audio_stream_seek(t)
 *   - we set seek_target_sec, raise seek_pending, wake the worker
 *   - worker drops pre-buffered audio, calls desc.seek(t), resumes
 *
 * Destroy:
 *   - main thread sets quit flag, wakes worker
 *   - waits on jce_thread_join
 *   - calls desc.destroy(ud) and frees ring
 */

#include "jce_audio_stream.h"

#include "os/core/jce_memory.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>

#include <string.h>

#define LOG_TAG "jce_audio_stream"

static const uint32_t DEFAULT_RING_FRAMES_PER_SEC = 48000u;
static const uint32_t DEFAULT_RING_SECONDS        = 2u;
static const uint32_t DEFAULT_LOW_WATER_FRAMES    = 24000u; /* ~0.5s @ 48k */

struct JceAudioStream {
    JceAudioStreamDesc desc;

    /* Ring buffer (interleaved s16 frames). */
    int16_t       *ring;
    uint32_t       ring_capacity_frames;   /* power-of-two not required */
    uint32_t       lo_water_frames;
    JceMutex      *ring_lock;
    uint32_t       ring_read_frame;        /* mod ring_capacity_frames */
    uint32_t       ring_write_frame;
    uint32_t       ring_count_frames;      /* available frames */

    /* Worker. */
    JceThread     *worker;
    JceSemaphore  *wake;
    JceAtomicI32  *quit;          /* 0/1 */
    JceAtomicI32  *eof;           /* 0/1: worker has hit decode_next == 0 */
    JceAtomicI32  *primed;        /* 0 until first real sample arrives post-seek */
    JceAtomicI32  *seek_pending;  /* 0/1 */
    double         seek_target_sec;
    JceMutex      *seek_lock;    /* protects seek_target_sec */

    /* Stats. */
    JceAtomicU64  *frames_pulled; /* monotonic; reset on seek */
    JceAtomicU64  *base_frame;    /* offset added by seek (in frames) */
};

/* ── Internal helpers ────────────────────────────────────────────── */

static int worker_main(void *arg);
static void worker_main_jce(void *arg) { (void)worker_main(arg); }

static void ring_clear_locked(JceAudioStream *s)
{
    s->ring_read_frame  = 0;
    s->ring_write_frame = 0;
    s->ring_count_frames = 0;
}

static uint32_t ring_free_locked(JceAudioStream *s)
{
    return s->ring_capacity_frames - s->ring_count_frames;
}

/* Append frames to the ring. Returns frames actually written. */
static uint32_t ring_write(JceAudioStream *s,
                           const int16_t *src, uint32_t frames)
{
    if (frames == 0) return 0;
    jce_mutex_lock(s->ring_lock);
    uint32_t can = ring_free_locked(s);
    if (can > frames) can = frames;
    uint32_t channels = s->desc.channels;
    for (uint32_t i = 0; i < can; ++i) {
        uint32_t idx = (s->ring_write_frame + i) % s->ring_capacity_frames;
        memcpy(&s->ring[idx * channels],
               &src[i * channels],
               channels * sizeof(int16_t));
    }
    s->ring_write_frame = (s->ring_write_frame + can) % s->ring_capacity_frames;
    s->ring_count_frames += can;
    jce_mutex_unlock(s->ring_lock);
    return can;
}

/* Pull frames from the ring. Returns frames actually read. */
static uint32_t ring_read(JceAudioStream *s,
                          int16_t *dst, uint32_t frames)
{
    if (frames == 0) return 0;
    jce_mutex_lock(s->ring_lock);
    uint32_t can = s->ring_count_frames;
    if (can > frames) can = frames;
    uint32_t channels = s->desc.channels;
    for (uint32_t i = 0; i < can; ++i) {
        uint32_t idx = (s->ring_read_frame + i) % s->ring_capacity_frames;
        memcpy(&dst[i * channels],
               &s->ring[idx * channels],
               channels * sizeof(int16_t));
    }
    s->ring_read_frame = (s->ring_read_frame + can) % s->ring_capacity_frames;
    s->ring_count_frames -= can;
    jce_mutex_unlock(s->ring_lock);
    return can;
}

/* ── Public API ──────────────────────────────────────────────────── */

JceAudioStream *jce_audio_stream_create(const JceAudioStreamDesc *desc)
{
    if (!desc || !desc->decode_next || desc->channels == 0
        || desc->channels > 8 || desc->samplerate == 0) {
        return NULL;
    }

    JceAudioStream *s = (JceAudioStream *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->quit         = jce_atomic_i32_create(0);
    s->eof          = jce_atomic_i32_create(0);
    s->primed       = jce_atomic_i32_create(0);
    s->seek_pending = jce_atomic_i32_create(0);
    s->frames_pulled = jce_atomic_u64_create(0);
    s->base_frame    = jce_atomic_u64_create(0);
    if (!s->quit || !s->eof || !s->primed || !s->seek_pending
        || !s->frames_pulled || !s->base_frame) {
        jce_audio_stream_destroy(s);
        return NULL;
    }

    s->desc = *desc;
    s->ring_capacity_frames = desc->ring_capacity_frames
        ? desc->ring_capacity_frames
        : (DEFAULT_RING_SECONDS * (desc->samplerate ? desc->samplerate
                                                    : DEFAULT_RING_FRAMES_PER_SEC));
    s->lo_water_frames = desc->lo_water_frames
        ? desc->lo_water_frames
        : DEFAULT_LOW_WATER_FRAMES;
    if (s->lo_water_frames >= s->ring_capacity_frames)
        s->lo_water_frames = s->ring_capacity_frames / 2u;

    size_t ring_bytes = (size_t)s->ring_capacity_frames
                      * desc->channels * sizeof(int16_t);
    s->ring = (int16_t *)JCE_MALLOC(ring_bytes);
    s->ring_lock = jce_mutex_create();
    s->seek_lock = jce_mutex_create();
    s->wake = jce_semaphore_create(0);
    if (!s->ring || !s->ring_lock || !s->seek_lock || !s->wake) {
        jce_audio_stream_destroy(s);
        return NULL;
    }
    memset(s->ring, 0, ring_bytes);

    s->worker = jce_thread_create(worker_main_jce, s, "jce_audio_stream");
    if (!s->worker) {
        LOG_ERROR(LOG_TAG, "failed to create worker thread");
        jce_audio_stream_destroy(s);
        return NULL;
    }

    return s;
}

void jce_audio_stream_destroy(JceAudioStream *s)
{
    if (!s) return;
    if (s->worker) {
        if (s->quit) jce_atomic_i32_store(s->quit, 1);
        if (s->wake) jce_semaphore_signal(s->wake);
        jce_thread_join(s->worker);
        s->worker = NULL;
    }
    if (s->desc.destroy) s->desc.destroy(s->desc.ud);
    if (s->wake) jce_semaphore_destroy(s->wake);
    if (s->ring_lock) jce_mutex_destroy(s->ring_lock);
    if (s->seek_lock) jce_mutex_destroy(s->seek_lock);
    if (s->ring) JCE_FREE(s->ring);
    jce_atomic_i32_destroy(s->quit);
    jce_atomic_i32_destroy(s->eof);
    jce_atomic_i32_destroy(s->primed);
    jce_atomic_i32_destroy(s->seek_pending);
    jce_atomic_u64_destroy(s->frames_pulled);
    jce_atomic_u64_destroy(s->base_frame);
    JCE_FREE(s);
}

uint32_t jce_audio_stream_pull(JceAudioStream *s,
                                int16_t *out, uint32_t frames)
{
    if (!s || !out || frames == 0) return 0;
    uint32_t got = ring_read(s, out, frames);
    /* Pad under-runs with silence so the consumer always gets a
     * continuous block. This keeps the audio clock advancing during
     * brief decode stalls AND past EOF, which is critical for the
     * viewer's A/V sync (otherwise audio time stalls and the viewer
     * loops trying to catch up). */
    if (got < frames) {
        uint32_t channels = s->desc.channels;
        memset(out + (size_t)got * channels, 0,
               (size_t)(frames - got) * channels * sizeof(int16_t));
    }
    /* Audio-clock policy:
     *  - Cold start / post-seek pre-roll: clock pinned to base_frame
     *    until the first real sample arrives (otherwise the viewer
     *    drives the video forward while audio is still spinning up).
     *  - Normal play: advance only by `got` (REAL samples produced),
     *    never by silence-padding count. This prevents cumulative
     *    drift when the worker briefly under-runs.
     *  - At true EOF (decoder exhausted): advance by full request so
     *    the clock walks to duration and `has_ended` triggers cleanly. */
    if (got > 0) {
        jce_atomic_i32_store(s->primed, 1);
        jce_atomic_u64_add(s->frames_pulled, (uint64_t)got);
    } else if (jce_atomic_i32_load(s->eof)
               && jce_atomic_i32_load(s->primed)) {
        jce_atomic_u64_add(s->frames_pulled, (uint64_t)frames);
    }

    /* Wake worker if we just consumed below the low-water mark. */
    jce_mutex_lock(s->ring_lock);
    bool need_refill = s->ring_count_frames < s->lo_water_frames;
    jce_mutex_unlock(s->ring_lock);
    if (need_refill) jce_semaphore_signal(s->wake);

    return frames;
}

void jce_audio_stream_seek(JceAudioStream *s, double time_sec)
{
    if (!s) return;
    if (time_sec < 0.0) time_sec = 0.0;
    if (s->desc.duration_sec > 0.0 && time_sec > s->desc.duration_sec)
        time_sec = s->desc.duration_sec;

    jce_mutex_lock(s->seek_lock);
    s->seek_target_sec = time_sec;
    jce_atomic_i32_store(s->seek_pending, 1);
    jce_mutex_unlock(s->seek_lock);

    /* Pre-emptively flush the ring so the audio thread immediately
     * starts under-running silence rather than playing stale audio. */
    jce_mutex_lock(s->ring_lock);
    ring_clear_locked(s);
    jce_mutex_unlock(s->ring_lock);

    /* Reset the audio clock to the seek target. The worker will catch
     * up by producing fresh samples. */
    uint64_t base = (uint64_t)(time_sec * (double)s->desc.samplerate);
    jce_atomic_u64_store(s->base_frame, base);
    jce_atomic_u64_store(s->frames_pulled, 0);
    jce_atomic_i32_store(s->eof, 0);
    jce_atomic_i32_store(s->primed, 0);

    jce_semaphore_signal(s->wake);
}

double jce_audio_stream_get_time(const JceAudioStream *s)
{
    if (!s || s->desc.samplerate == 0) return 0.0;
    uint64_t base = jce_atomic_u64_load(s->base_frame);
    uint64_t pulled = jce_atomic_u64_load(s->frames_pulled);
    double t = (double)(base + pulled) / (double)s->desc.samplerate;
    /* Cap at duration: the pull callback advances frames_pulled even
     * past EOF (to keep the clock alive during under-runs). Without
     * this cap, the viewer's A/V sync chases an ever-growing audio_t
     * and re-seeks the video back to near-end every tick, causing the
     * "tape stuck" loop. */
    if (s->desc.duration_sec > 0.0 && t > s->desc.duration_sec)
        t = s->desc.duration_sec;
    return t;
}

bool jce_audio_stream_eof(const JceAudioStream *s)
{
    if (!s) return true;
    if (!jce_atomic_i32_load(s->eof)) return false;
    jce_mutex_lock(s->ring_lock);
    bool drained = (s->ring_count_frames == 0);
    jce_mutex_unlock(s->ring_lock);
    return drained;
}

bool jce_audio_stream_is_primed(const JceAudioStream *s)
{
    return s ? (jce_atomic_i32_load(s->primed) != 0) : false;
}

uint32_t jce_audio_stream_channels(const JceAudioStream *s)
{
    return s ? s->desc.channels : 0u;
}

uint32_t jce_audio_stream_samplerate(const JceAudioStream *s)
{
    return s ? s->desc.samplerate : 0u;
}

/* ── Worker thread ───────────────────────────────────────────────── */

static int worker_main(void *arg)
{
    JceAudioStream *s = (JceAudioStream *)arg;

    /* Per-iteration scratch buffer sized for one decode batch. We size
     * it to a typical Opus packet output (120ms @ 48k = 5760 frames).
     * Multi-channel safe up to 8 channels. */
    enum { BATCH_FRAMES = 5760 };
    int16_t scratch[BATCH_FRAMES * 8];

    while (!jce_atomic_i32_load(s->quit)) {
        /* Service pending seek before producing new audio. */
        if (jce_atomic_i32_exchange(s->seek_pending, 0)) {
            jce_mutex_lock(s->seek_lock);
            double tgt = s->seek_target_sec;
            jce_mutex_unlock(s->seek_lock);
            if (s->desc.seek) s->desc.seek(s->desc.ud, tgt);
            jce_mutex_lock(s->ring_lock);
            ring_clear_locked(s);
            jce_mutex_unlock(s->ring_lock);
        }

        if (jce_atomic_i32_load(s->eof)) {
            /* Sleep until woken by seek or destroy. */
            jce_semaphore_wait(s->wake);
            continue;
        }

        /* Check ring level. If full enough, sleep until consumer drains. */
        jce_mutex_lock(s->ring_lock);
        uint32_t avail = s->ring_count_frames;
        uint32_t freef = ring_free_locked(s);
        jce_mutex_unlock(s->ring_lock);

        if (avail >= s->ring_capacity_frames - BATCH_FRAMES) {
            /* Ring is nearly full — wait for a pull to free space. We
             * use a short timeout so we periodically re-check quit/seek. */
            jce_semaphore_wait_timeout(s->wake, 50);
            continue;
        }

        /* Decode one batch into scratch. */
        uint32_t want = freef < BATCH_FRAMES ? freef : BATCH_FRAMES;
        if (want == 0) {
            jce_semaphore_wait_timeout(s->wake, 50);
            continue;
        }
        uint32_t produced = s->desc.decode_next(s->desc.ud, scratch, want);
        if (produced == 0) {
            jce_atomic_i32_store(s->eof, 1);
            continue;
        }
        /* Write into ring; if it can't all fit (race), remainder is dropped. */
        ring_write(s, scratch, produced);
    }

    return 0;
}
