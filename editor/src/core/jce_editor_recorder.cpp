/*
 * jce_editor_recorder.cpp  F9 screen recorder (VP9 video + Opus system audio -> .mkv).
 *
 * Video: renderer capture sink (screenshot path) -> bounded queue.
 * Audio: WASAPI loopback (system output) -> bounded queue.
 * A worker thread drains both and feeds the encoder, which reorders by
 * timestamp before muxing. Audio is Windows-only (loopback); on failure the
 * recording is video-only.
 */

#include "core/jce_editor_recorder.h"
#include "core/jce_editor_alloc.h"

extern "C" {
#include <jce/renderer/jce_renderer.h>
#include <jce/ui/jce_imgui_renderer.h>
#include <jce/middleware/video/jce_webm_encoder.h>
#include <jce/middleware/audio/jce_audio_loopback.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
}

#include <cstdlib>
#include <cstring>
#include <cstdio>

#define LOG_TAG       "jce_editor_rec"
#define VID_QUEUE_MAX 12
/* Audio chunks are tiny (~4 KB each, ~100/s from WASAPI loopback); a deep
   queue costs little and absorbs any encoder stall (a heavy VP9 keyframe)
   without dropping ~5 s of audio. */
#define AUD_QUEUE_MAX 512
/* Cap on audio held before the encoder exists (see rec_worker): bounds the
   pre-first-video-frame hold so a video stream that never starts can't grow
   it without bound.  ~5 s at 100 chunks/s. */
#define AUD_PREROLL_MAX 512

namespace {

struct VidFrame { void *bgra; uint64_t ts_ms; VidFrame *next; };
struct AudChunk { float *pcm; uint32_t frames; uint64_t ts_ms; AudChunk *next; };

struct RecState {
    bool         active;
    JceRenderer *renderer;
    char         path[512];

    uint32_t     width, height, pitch;
    int          yflip;
    uint32_t     aud_ch;
    bool         have_audio;

    JceMutex    *mtx;
    JceCondVar  *cond;
    VidFrame    *vhead, *vtail;  uint32_t vdepth;
    AudChunk    *ahead, *atail;  uint32_t adepth;
    bool         worker_run;
    JceThread   *worker;
    uint64_t     start_ms;

    uint32_t     vcaptured, vwritten, vdropped, adropped, awritten;
};

RecState g;

uint64_t now_rel_ms() {
    const uint64_t now = (uint64_t)jce_time_ticks_ms();
    return now >= g.start_ms ? now - g.start_ms : 0;
}

/* ── Video capture sink (render thread) ───────────────────────────── */
void rec_begin(void *ud, uint32_t w, uint32_t h, uint32_t pitch, int yflip) {
    (void)ud; g.width = w; g.height = h; g.pitch = pitch; g.yflip = yflip;
}
void rec_frame(void *ud, const void *data, uint32_t size) {
    (void)ud;
    if (!data || !size) return;
    jce_mutex_lock(g.mtx);
    const bool full = g.vdepth >= VID_QUEUE_MAX;
    if (full) g.vdropped++;
    jce_mutex_unlock(g.mtx);
    if (full) return;

    VidFrame *f = (VidFrame *)ED_MALLOC(sizeof(VidFrame));
    if (!f) return;
    f->bgra = ED_MALLOC(size);
    if (!f->bgra) { ED_FREE(f); return; }
    memcpy(f->bgra, data, size);
    f->ts_ms = now_rel_ms();
    f->next = nullptr;
    jce_mutex_lock(g.mtx);
    if (g.vtail) g.vtail->next = f; else g.vhead = f;
    g.vtail = f; g.vdepth++; g.vcaptured++;
    jce_cond_signal(g.cond);
    jce_mutex_unlock(g.mtx);
}
void rec_end(void *ud) { (void)ud; }

/* ── Audio loopback callback (audio thread) ───────────────────────── */
void aud_cb(void *ud, const float *pcm, uint32_t frames, uint32_t rate, uint32_t ch) {
    (void)ud; (void)rate;
    if (!pcm || !frames) return;
    jce_mutex_lock(g.mtx);
    const bool full = g.adepth >= AUD_QUEUE_MAX;
    if (full) g.adropped++;
    jce_mutex_unlock(g.mtx);
    if (full) return;

    AudChunk *c = (AudChunk *)ED_MALLOC(sizeof(AudChunk));
    if (!c) return;
    const size_t bytes = (size_t)frames * ch * sizeof(float);
    c->pcm = (float *)ED_MALLOC(bytes);
    if (!c->pcm) { ED_FREE(c); return; }
    memcpy(c->pcm, pcm, bytes);
    c->frames = frames; c->ts_ms = now_rel_ms(); c->next = nullptr;
    jce_mutex_lock(g.mtx);
    if (g.atail) g.atail->next = c; else g.ahead = c;
    g.atail = c; g.adepth++;
    jce_cond_signal(g.cond);
    jce_mutex_unlock(g.mtx);
}

/* Default keyframe-interval seed used only when the real capture rate cannot
   be measured (e.g. a recording of a single frame). */
#define REC_FALLBACK_FPS 30u

/* Derive fps from the measured gap between the first two captured frames.
   `fps` only seeds the encoder's keyframe interval, so a coarse integer
   estimate (clamped to a sane range) is sufficient. */
uint32_t measure_fps(uint64_t first_ts_ms, uint64_t second_ts_ms) {
    if (second_ts_ms <= first_ts_ms) return REC_FALLBACK_FPS;
    const uint64_t dt = second_ts_ms - first_ts_ms;
    uint32_t fps = (uint32_t)((1000u + dt / 2u) / dt);  /* round(1000/dt) */
    if (fps < 1u)   fps = 1u;
    if (fps > 240u) fps = 240u;
    return fps;
}

/* ── Worker thread — encode + mux ─────────────────────────────────── */
void rec_worker(void *arg) {
    (void)arg;
    JceWebmEncoder *enc = nullptr;
    /* The first captured frame is held back until a second frame arrives so we
       can measure the real capture fps from the timestamp delta before the
       encoder (which bakes fps into its keyframe interval) is created. */
    VidFrame *pending = nullptr;
    /* Audio that arrives before the encoder exists (the encoder is created
       lazily on the 2nd video frame so its fps is known) is HELD here, not
       dropped, then flushed once the encoder is up — the muxer reorders by
       timestamp, so pre-first-frame audio still lands correctly.  This is
       what eliminates the "N audio dropped" at the start of every clip. */
    AudChunk *pa_head = nullptr, *pa_tail = nullptr; uint32_t pa_n = 0;
    auto flush_preroll_audio = [&](JceWebmEncoder *e) {
        while (pa_head) {
            AudChunk *c = pa_head; pa_head = c->next;
            if (e) { jce_webm_encoder_push_audio(e, c->pcm, c->frames, c->ts_ms); g.awritten++; }
            ED_FREE(c->pcm); ED_FREE(c);
        }
        pa_tail = nullptr; pa_n = 0;
    };
    for (;;) {
        jce_mutex_lock(g.mtx);
        while (g.worker_run && !g.vhead && !g.ahead)
            jce_cond_wait(g.cond, g.mtx);
        /* Detach BOTH whole queues so neither stream starves the other —
           the encoder reorders by timestamp anyway. */
        VidFrame *vlist = g.vhead; g.vhead = g.vtail = nullptr; g.vdepth = 0;
        AudChunk *alist = g.ahead; g.ahead = g.atail = nullptr; g.adepth = 0;
        const bool run = g.worker_run;
        jce_mutex_unlock(g.mtx);

        /* Video first (the first two frames lazily create the encoder once the
           capture rate is known). */
        while (vlist) {
            VidFrame *vf = vlist; vlist = vf->next;
            if (!enc && g.width && g.height) {
                if (!pending) { pending = vf; continue; }  /* hold frame #1 */
                /* Frame #2: measure fps, create encoder, flush both. */
                const uint32_t fps = measure_fps(pending->ts_ms, vf->ts_ms);
                enc = jce_webm_encoder_create(g.path, g.width, g.height, fps, 8000,
                                              g.have_audio ? 48000u : 0u,
                                              g.have_audio ? g.aud_ch : 0u);
                if (enc && jce_webm_encoder_push_bgra(enc, pending->bgra, g.pitch,
                                                      g.yflip, pending->ts_ms))
                    g.vwritten++;
                ED_FREE(pending->bgra); ED_FREE(pending); pending = nullptr;
                flush_preroll_audio(enc);   /* release audio held before enc */
            }
            if (enc && jce_webm_encoder_push_bgra(enc, vf->bgra, g.pitch, g.yflip, vf->ts_ms))
                g.vwritten++;
            ED_FREE(vf->bgra); ED_FREE(vf);
        }
        /* Audio: push once the encoder exists; before that, HOLD (not drop)
           so the leading audio isn't lost — flushed by flush_preroll_audio
           the moment the encoder is created above. */
        while (alist) {
            AudChunk *ac = alist; alist = ac->next;
            if (enc) {
                jce_webm_encoder_push_audio(enc, ac->pcm, ac->frames, ac->ts_ms);
                g.awritten++;
                ED_FREE(ac->pcm); ED_FREE(ac);
            } else if (pa_n < AUD_PREROLL_MAX) {
                ac->next = nullptr;
                if (pa_tail) pa_tail->next = ac; else pa_head = ac;
                pa_tail = ac; pa_n++;
            } else {
                g.adropped++;   /* video never started: bounded safety drop */
                ED_FREE(ac->pcm); ED_FREE(ac);
            }
        }

        if (!run) {
            jce_mutex_lock(g.mtx);
            const bool empty = !g.vhead && !g.ahead;
            jce_mutex_unlock(g.mtx);
            if (empty) break;
        }
    }
    /* A single-frame recording never produced a second timestamp to measure
       from: emit the held frame at the fallback rate so it isn't lost. */
    if (pending) {
        if (!enc && g.width && g.height)
            enc = jce_webm_encoder_create(g.path, g.width, g.height,
                                          REC_FALLBACK_FPS, 8000,
                                          g.have_audio ? 48000u : 0u,
                                          g.have_audio ? g.aud_ch : 0u);
        if (enc && jce_webm_encoder_push_bgra(enc, pending->bgra, g.pitch,
                                              g.yflip, pending->ts_ms))
            g.vwritten++;
        ED_FREE(pending->bgra); ED_FREE(pending); pending = nullptr;
    }
    /* Flush any audio still held (single-frame clip, or a stream that ended
       before its 2nd video frame).  Drops only when the encoder truly never
       came up (no video at all). */
    flush_preroll_audio(enc);
    if (enc) jce_webm_encoder_finish(enc);
}

} // namespace

/* ── Public API — main thread ─────────────────────────────────────── */
extern "C" bool jce_editor_recorder_start(JceRenderer *r, const char *out_path) {
    if (g.active || !r || !out_path || !*out_path) return false;
    memset(&g, 0, sizeof(g));
    g.renderer = r;
    snprintf(g.path, sizeof(g.path), "%s", out_path);

    g.mtx = jce_mutex_create();
    g.cond = jce_cond_create();
    if (!g.mtx || !g.cond) {
        if (g.mtx) jce_mutex_destroy(g.mtx);
        if (g.cond) jce_cond_destroy(g.cond);
        g.mtx = nullptr; g.cond = nullptr;
        LOG_ERROR(LOG_TAG, "sync alloc failed");
        return false;
    }

    g.worker_run = true;
    g.start_ms = (uint64_t)jce_time_ticks_ms();
    g.worker = jce_thread_create(rec_worker, nullptr, "jce_editor_rec");
    if (!g.worker) {
        jce_cond_destroy(g.cond); jce_mutex_destroy(g.mtx);
        g.cond = nullptr; g.mtx = nullptr;
        LOG_ERROR(LOG_TAG, "worker spawn failed");
        return false;
    }

    /* Audio first (so the encoder is created with the right track count). */
    g.aud_ch = 2;
    g.have_audio = jce_audio_loopback_start(aud_cb, nullptr);
    if (!g.have_audio)
        LOG_WARN(LOG_TAG, "no system loopback — recording video only");

    jce_renderer_set_capture_sink(rec_begin, rec_frame, rec_end, nullptr);
    jce_renderer_set_backbuffer_capture(r, true);
    /* Whole-window capture: the ImGui renderer re-renders the UI into an offscreen
       FBO and reads it back into the sink each frame.  The backbuffer screen_shot
       path is black on D3D flip-model swap chains, so route around it. */
    jce_renderer_set_capture_imgui_mode(true);
    jce_imgui_renderer_set_recording(true);
    g.active = true;
    LOG_SUCCESS(LOG_TAG, "recording -> %s (audio=%d)", g.path, (int)g.have_audio);
    return true;
}

extern "C" void jce_editor_recorder_stop(void) {
    if (!g.active) return;

    jce_imgui_renderer_set_recording(false);
    jce_renderer_set_capture_imgui_mode(false);
    jce_renderer_set_backbuffer_capture(g.renderer, false);
    jce_renderer_set_capture_sink(nullptr, nullptr, nullptr, nullptr);
    if (g.have_audio) jce_audio_loopback_stop();

    jce_mutex_lock(g.mtx);
    g.worker_run = false;
    jce_cond_signal(g.cond);
    jce_mutex_unlock(g.mtx);
    jce_thread_join(g.worker);

    jce_cond_destroy(g.cond);
    jce_mutex_destroy(g.mtx);
    LOG_SUCCESS(LOG_TAG, "stopped: %u video (%u dropped), %u audio (%u dropped) -> %s",
                g.vwritten, g.vdropped, g.awritten, g.adropped, g.path);

    g.active = false;
    g.worker = nullptr; g.mtx = nullptr; g.cond = nullptr;
}

extern "C" bool     jce_editor_recorder_is_active(void)   { return g.active; }
extern "C" uint32_t jce_editor_recorder_frame_count(void) { return g.vwritten; }
extern "C" uint32_t jce_editor_recorder_dropped(void)     { return g.vdropped; }
