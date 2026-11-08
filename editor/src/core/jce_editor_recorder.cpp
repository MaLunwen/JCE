/*
 * F9 recorder: VP9 video + Opus engine audio -> .mkv.
 * The worker owns encoding. The video budget includes reservations, queued,
 * detached, first-frame seed and the frame currently being encoded.
 */
#include "core/jce_editor_recorder.h"
#include "core/jce_editor_alloc.h"

extern "C" {
#include <jce/renderer/jce_renderer.h>
#include <jce/ui/jce_imgui_renderer.h>
#include <jce/middleware/video/jce_webm_encoder.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
}
#include <cstdlib>
#include <cstring>
#include <cstdio>

#define LOG_TAG "jce_editor_rec"
#define VID_INFLIGHT_MAX 12u
/* Preserve the existing small PCM queue and pre-video hold budgets. */
#define AUD_QUEUE_MAX 512u
#define AUD_PREROLL_MAX 512u
#define REC_FALLBACK_FPS 30u

namespace {
struct CaptureFormat { uint32_t width, height, pitch; int yflip; };
enum class VideoReject { none, invalid_format, payload_extent, format_change };
struct VidFrame {
    void *bgra;
    uint64_t ts_ms;
    CaptureFormat format;
    uint32_t bytes;
    VidFrame *next;
};
struct AudChunk { float *pcm; uint32_t frames; uint64_t ts_ms; AudChunk *next; };
struct RecState {
    bool active, stopping;
    JceRenderer *renderer;
    char path[512];
    uint32_t width, height, pitch;
    int yflip;
    uint32_t aud_ch;
    bool have_audio;
    bool format_locked, format_error_reported;
    CaptureFormat record_format, rejected_format;
    uint32_t rejected_bytes;
    VideoReject reject_reason;
    JceMutex *mtx;
    JceCondVar *cond;
    VidFrame *vhead, *vtail;
    uint32_t vdepth, vinflight, vpeak;
    AudChunk *ahead, *atail;
    uint32_t adepth;
    bool worker_run;
    JceThread *worker;
    uint64_t start_ms;
    uint32_t vcaptured, vwritten, vdropped, adropped, awritten;
};
RecState g;

/* The renderer sink has no unregister-and-join contract. Keep this one small
 * fence for the process lifetime, independently of every session mutex. A late
 * callback may inspect it after Stop; it must never inspect destroyed g.mtx.
 * These two public I32 objects and one public TLS slot are initialized once,
 * before publication, and deliberately retained (not one allocation per Start).
 * This fences actual Recorder borrowers, NOT the renderer/GPU source. Without
 * a source generation API a late old screenshot after a new Start is unknown.
 */
struct CaptureAdmission {
    JceAtomicI32 *closed;
    JceAtomicI32 *borrowers;
    JceTLS *inside;
};
CaptureAdmission admission;

bool prepare_admission() {
    if (admission.closed) return true;
    JceAtomicI32 *closed = jce_atomic_i32_create(1);
    JceAtomicI32 *borrowers = jce_atomic_i32_create(0);
    JceTLS *inside = closed && borrowers ? jce_tls_create(nullptr) : nullptr;
    if (!closed || !borrowers || !inside) {
        if (closed) jce_atomic_i32_destroy(closed);
        if (borrowers) jce_atomic_i32_destroy(borrowers);
        return false;
    }
    admission = {closed, borrowers, inside};
    return true;
}

struct CallbackBorrow {
    bool held = false;
    void *previous = nullptr;
    CallbackBorrow() {
        if (!admission.closed || jce_atomic_i32_load(admission.closed)) return;
        jce_atomic_i32_add(admission.borrowers, 1);
        /* Stop can win between the first load and the increment. It is safe
         * even if it already destroyed g: this second load touches only the
         * process-stable fence, and a refused borrower never touches g. */
        if (jce_atomic_i32_load(admission.closed)) {
            jce_atomic_i32_add(admission.borrowers, -1);
            return;
        }
        previous = jce_tls_get(admission.inside);
        jce_tls_set(admission.inside, &admission);
        if (jce_tls_get(admission.inside) != &admission) {
            jce_atomic_i32_add(admission.borrowers, -1);
            return; /* TLS admission failed: never risk callback self-wait. */
        }
        held = true;
    }
    ~CallbackBorrow() {
        if (!held) return;
        jce_tls_set(admission.inside, previous);
        jce_atomic_i32_add(admission.borrowers, -1);
    }
    CallbackBorrow(const CallbackBorrow &) = delete;
    CallbackBorrow &operator=(const CallbackBorrow &) = delete;
};

bool control_allowed() {
    return jce_thread_is_main() &&
        (!admission.inside || !jce_tls_get(admission.inside));
}
void close_admission() { jce_atomic_i32_store(admission.closed, 1); }
void wait_borrowers() {
    /* No deadline may turn an unfinished copy into permission to free its
     * mutex/buffer. Never hold g.mtx while waiting for a borrower to return. */
    while (jce_atomic_i32_load(admission.borrowers)) jce_thread_sleep_ms(1);
}
/* Bound both the visible last row and the declared complete storage without
 * overflowing width*4, pitch*height or a size_t on 32-bit consumers. The encoder
 * rounds odd dimensions down; dimensions below two cannot form an I420 frame. */
VideoReject capture_extent(const CaptureFormat &format, uint32_t bytes) {
    if (format.width < 2 || format.height < 2 || format.width > UINT32_MAX / 4u ||
        (format.yflip != 0 && format.yflip != 1)) return VideoReject::invalid_format;
    const uint32_t row = format.width * 4u;
    if (format.pitch < row || format.height > UINT32_MAX / format.pitch)
        return VideoReject::invalid_format;
    const uint32_t storage = format.pitch * format.height;
    if (bytes < row || bytes > storage ||
        format.height - 1u > (bytes - row) / format.pitch) return VideoReject::payload_extent;
    return VideoReject::none;
}
bool same_format(const CaptureFormat &a, const CaptureFormat &b) {
    return a.width == b.width && a.height == b.height && a.pitch == b.pitch && a.yflip == b.yflip;
}
void reject_video_locked(VideoReject why, const CaptureFormat &format, uint32_t bytes) {
    ++g.vdropped;
    if (g.reject_reason == VideoReject::none) {
        g.reject_reason = why; g.rejected_format = format; g.rejected_bytes = bytes;
    }
}
bool owned_frame_valid(const VidFrame *frame) {
    VideoReject why = capture_extent(frame->format, frame->bytes);
    if (!frame->bgra) why = VideoReject::payload_extent;
    jce_mutex_lock(g.mtx);
    if (why == VideoReject::none && !same_format(frame->format, g.record_format))
        why = VideoReject::format_change;
    if (why != VideoReject::none) reject_video_locked(why, frame->format, frame->bytes);
    jce_mutex_unlock(g.mtx);
    return why == VideoReject::none;
}
CaptureFormat frame_format_for_encode(const VidFrame *frame) {
    return frame->format;
}
bool push_video(JceWebmEncoder *enc, const VidFrame *frame) {
    if (!owned_frame_valid(frame)) return false;
    const CaptureFormat format = frame_format_for_encode(frame);
    return jce_webm_encoder_push_bgra(enc, frame->bgra, format.pitch, format.yflip, frame->ts_ms);
}
/* Called only by the stopping main owner, after closing admission and before
 * destroying session state. The stopping latch makes log-sink reentry safe. */
void report_format_rejection() {
    jce_mutex_lock(g.mtx);
    const bool report = g.reject_reason != VideoReject::none && !g.format_error_reported;
    const VideoReject reason = g.reject_reason;
    const CaptureFormat format = g.rejected_format;
    const uint32_t bytes = g.rejected_bytes;
    if (report) g.format_error_reported = true;
    jce_mutex_unlock(g.mtx);
    if (report) {
        const char *why = reason == VideoReject::format_change ? "format changed" :
            reason == VideoReject::payload_extent ? "payload extent" : "invalid/overflow format";
        LOG_ERROR(LOG_TAG, "capture format rejected (%s): %ux%u pitch=%u bytes=%u; stopping; restart F9 at the new size",
            why, format.width, format.height, format.pitch, bytes);
    }
}
uint64_t now_rel_ms() {
    const uint64_t now = (uint64_t)jce_time_ticks_ms();
    return now >= g.start_ms ? now - g.start_ms : 0;
}
void return_video_credit() {
    jce_mutex_lock(g.mtx);
    --g.vinflight;
    jce_mutex_unlock(g.mtx);
}
void free_video(VidFrame *f) {
    if (!f) return;
    /* Credit is returned only AFTER the actual payload and node are freed.
     * This helper is never called under the queue lock. */
    ED_FREE(f->bgra);
    ED_FREE(f);
    return_video_credit();
}
void free_audio(AudChunk *c) {
    if (!c) return;
    ED_FREE(c->pcm);
    ED_FREE(c);
}
void discard_video(VidFrame *&head) {
    while (head) { VidFrame *f = head; head = f->next; free_video(f); }
}
void discard_audio(AudChunk *&head) {
    while (head) { AudChunk *c = head; head = c->next; free_audio(c); }
}
/* One non-movable reservation. Before publication it owns both the credit and
 * any partially allocated node; after publication the linked-list node owns
 * that credit until free_video. OOM/exception cannot abandon the reservation. */
struct VideoReservation {
    VidFrame *frame = nullptr;
    bool reserved = true;
    ~VideoReservation() {
        if (!reserved) return;
        if (frame) free_video(frame); else return_video_credit();
    }
    void published() { reserved = false; frame = nullptr; }
    VideoReservation() = default;
    VideoReservation(const VideoReservation &) = delete;
    VideoReservation &operator=(const VideoReservation &) = delete;
};

void rec_begin(void *ud, uint32_t w, uint32_t h, uint32_t pitch, int yflip) {
    (void)ud;
    CallbackBorrow borrow;
    if (!borrow.held) return;
    jce_mutex_lock(g.mtx);
    g.width = w; g.height = h; g.pitch = pitch; g.yflip = yflip;
    jce_mutex_unlock(g.mtx);
}
void rec_frame(void *ud, const void *data, uint32_t size) {
    (void)ud;
    CallbackBorrow borrow;
    if (!borrow.held) return;
    jce_mutex_lock(g.mtx);
    const CaptureFormat format = {g.width, g.height, g.pitch, g.yflip};
    VideoReject why = capture_extent(format, size);
    if (!data) why = VideoReject::payload_extent;
    if (why == VideoReject::none && g.format_locked && !same_format(format, g.record_format))
        why = VideoReject::format_change;
    if (why != VideoReject::none) {
        reject_video_locked(why, format, size);
        jce_mutex_unlock(g.mtx);
        return;
    }
    if (g.reject_reason != VideoReject::none) {
        ++g.vdropped; /* Stop already requested; only queued valid frames drain. */
        jce_mutex_unlock(g.mtx);
        return;
    }
    const bool full = g.vinflight >= VID_INFLIGHT_MAX;
    if (full) ++g.vdropped;
    else {
        if (!g.format_locked) { g.record_format = format; g.format_locked = true; }
        ++g.vinflight;
        if (g.vinflight > g.vpeak) g.vpeak = g.vinflight;
    }
    jce_mutex_unlock(g.mtx);
    if (full) return; /* no allocation or copy when the whole budget is busy */

    try {
        VideoReservation reservation;
        reservation.frame = (VidFrame *)ED_MALLOC(sizeof(VidFrame));
        VidFrame *f = reservation.frame;
        if (!f) return;
        f->bgra = nullptr; f->next = nullptr;
        f->format = format; f->bytes = size;
        f->bgra = ED_MALLOC(size);
        if (!f->bgra) return;
        memcpy(f->bgra, data, size);
        f->ts_ms = now_rel_ms();
        jce_mutex_lock(g.mtx);
        if (g.vtail) g.vtail->next = f; else g.vhead = f;
        g.vtail = f; ++g.vdepth; ++g.vcaptured;
        reservation.published();
        jce_cond_signal(g.cond);
        jce_mutex_unlock(g.mtx);
    } catch (...) {
        /* The public allocator ordinarily returns NULL. A throwing custom
         * provider must still release the reservation and callback borrow. */
        LOG_ERROR(LOG_TAG, "video capture provider threw; frame discarded");
    }
}
void rec_end(void *ud) { (void)ud; }
void aud_cb(void *ud, const float *pcm, uint32_t frames, uint32_t rate, uint32_t ch) {
    (void)ud; (void)rate;
    CallbackBorrow borrow;
    if (!borrow.held || !pcm || !frames) return;
    jce_mutex_lock(g.mtx);
    const bool full = g.adepth >= AUD_QUEUE_MAX;
    if (full) ++g.adropped;
    jce_mutex_unlock(g.mtx);
    if (full) return;
    AudChunk *c = nullptr;
    try {
        c = (AudChunk *)ED_MALLOC(sizeof(AudChunk));
        if (!c) return;
        c->pcm = nullptr; c->next = nullptr;
        const size_t bytes = (size_t)frames * ch * sizeof(float);
        c->pcm = (float *)ED_MALLOC(bytes);
        if (!c->pcm) { free_audio(c); return; }
        memcpy(c->pcm, pcm, bytes);
        c->frames = frames; c->ts_ms = now_rel_ms();
        jce_mutex_lock(g.mtx);
        if (g.atail) g.atail->next = c; else g.ahead = c;
        g.atail = c; ++g.adepth;
        jce_cond_signal(g.cond);
        jce_mutex_unlock(g.mtx);
    } catch (...) {
        free_audio(c);
        LOG_ERROR(LOG_TAG, "audio capture provider threw; chunk discarded");
    }
}
uint32_t measure_fps(uint64_t first, uint64_t second) {
    if (second <= first) return REC_FALLBACK_FPS;
    const uint64_t dt = second - first;
    uint32_t fps = (uint32_t)((1000u + dt / 2u) / dt);
    if (fps < 1u) fps = 1u;
    if (fps > 240u) fps = 240u;
    return fps;
}
void note_video_written() {
    jce_mutex_lock(g.mtx); ++g.vwritten; jce_mutex_unlock(g.mtx);
}

void rec_worker(void *arg) {
    (void)arg;
    JceWebmEncoder *enc = nullptr;
    /* All local lists/active nodes remain named owners across provider calls,
     * so the exception path can release every Recorder-owned buffer. */
    VidFrame *pending = nullptr, *vlist = nullptr, *vf = nullptr;
    AudChunk *alist = nullptr, *ac = nullptr, *pa_head = nullptr, *pa_tail = nullptr;
    uint32_t pa_n = 0;
    auto flush_audio = [&](JceWebmEncoder *e) {
        while (pa_head) {
            /* Keep the node linked until the potentially throwing call returns. */
            AudChunk *c = pa_head;
            if (e) { jce_webm_encoder_push_audio(e, c->pcm, c->frames, c->ts_ms); ++g.awritten; }
            pa_head = c->next;
            free_audio(c);
        }
        pa_tail = nullptr; pa_n = 0;
    };
    try {
        for (;;) {
            jce_mutex_lock(g.mtx);
            while (g.worker_run && !g.vhead && !g.ahead) jce_cond_wait(g.cond, g.mtx);
            vlist = g.vhead; g.vhead = g.vtail = nullptr; g.vdepth = 0;
            alist = g.ahead; g.ahead = g.atail = nullptr; g.adepth = 0;
            const bool run = g.worker_run;
            jce_mutex_unlock(g.mtx);
            /* Detaching a queue does NOT release any video credits. */
            while (vlist) {
                vf = vlist; vlist = vf->next;
                if (!owned_frame_valid(vf)) { free_video(vf); vf = nullptr; continue; }
                if (!enc) {
                    if (!pending) { pending = vf; vf = nullptr; continue; }
                    const uint32_t fps = measure_fps(pending->ts_ms, vf->ts_ms);
                    const CaptureFormat format = frame_format_for_encode(pending);
                    enc = jce_webm_encoder_create(g.path, format.width, format.height, fps, 8000,
                        g.have_audio ? 48000u : 0u, g.have_audio ? g.aud_ch : 0u);
                    if (enc && push_video(enc, pending))
                        note_video_written();
                    free_video(pending); pending = nullptr;
                    flush_audio(enc);
                }
                if (enc && push_video(enc, vf))
                    note_video_written();
                free_video(vf); vf = nullptr;
            }
            while (alist) {
                ac = alist; alist = ac->next;
                if (enc) {
                    jce_webm_encoder_push_audio(enc, ac->pcm, ac->frames, ac->ts_ms);
                    ++g.awritten;
                    free_audio(ac); ac = nullptr;
                } else if (pa_n < AUD_PREROLL_MAX) {
                    ac->next = nullptr;
                    if (pa_tail) pa_tail->next = ac; else pa_head = ac;
                    pa_tail = ac; ++pa_n; ac = nullptr;
                } else {
                    jce_mutex_lock(g.mtx); ++g.adropped; jce_mutex_unlock(g.mtx);
                    free_audio(ac); ac = nullptr;
                }
            }
            if (!run) break; /* Stop joined actual producers before closing worker_run. */
        }
        if (pending) {
            if (owned_frame_valid(pending)) {
                const CaptureFormat format = frame_format_for_encode(pending);
                if (!enc)
                    enc = jce_webm_encoder_create(g.path, format.width, format.height, REC_FALLBACK_FPS, 8000,
                        g.have_audio ? 48000u : 0u, g.have_audio ? g.aud_ch : 0u);
                if (enc && push_video(enc, pending)) note_video_written();
            }
            free_video(pending); pending = nullptr;
        }
        flush_audio(enc);
    } catch (...) {
        /* Stop remains the session owner. Close admission before dropping
         * failed work; wait actual producers, never a timeout-and-free. */
        close_admission();
        wait_borrowers();
        jce_mutex_lock(g.mtx);
        g.worker_run = false;
        jce_mutex_unlock(g.mtx);
        LOG_ERROR(LOG_TAG, "encoder provider threw; recording failed, pending work discarded");
    }
    free_video(vf); free_video(pending); discard_video(vlist);
    free_audio(ac); discard_audio(alist); discard_audio(pa_head);
    /* Failure may leave producer-published work; ordinary Stop leaves none. */
    jce_mutex_lock(g.mtx);
    vlist = g.vhead; g.vhead = g.vtail = nullptr; g.vdepth = 0;
    alist = g.ahead; g.ahead = g.atail = nullptr; g.adepth = 0;
    jce_mutex_unlock(g.mtx);
    discard_video(vlist); discard_audio(alist);
    if (enc) {
        try { jce_webm_encoder_finish(enc); }
        catch (...) {
            /* The encoder has no failed-finish/destroy ABI. Do not retry an
             * unknown partially destroyed owner or claim native retirement. */
            LOG_ERROR(LOG_TAG, "encoder finish threw; encoder retirement unknown");
        }
    }
}
} // namespace

/* Lifecycle calls are serialized on the Editor main thread. */
extern "C" bool jce_editor_recorder_start(JceRenderer *r, const char *path) {
    if (!control_allowed()) return false;
    if (g.active || g.stopping || !r || !path || !*path) return false;
    if (!prepare_admission()) {
        LOG_ERROR(LOG_TAG, "callback admission alloc failed");
        return false;
    }
    (void)ed_alloc_default_ptr(); /* initialize this TU allocator cache before callbacks */
    memset(&g, 0, sizeof(g));
    g.renderer = r;
    snprintf(g.path, sizeof(g.path), "%s", path);
    g.mtx = jce_mutex_create(); g.cond = jce_cond_create();
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
    /* Any immediate audio callback is refused while admission stays closed. */
    g.aud_ch = 2;
    g.have_audio = jce_audio_master_tap_set(aud_cb, nullptr);
    if (!g.have_audio) LOG_WARN(LOG_TAG, "no master-mix device — recording video only");
    g.active = true;
    jce_atomic_i32_store(admission.closed, 0); /* fully initialized session */
    jce_renderer_set_capture_sink(rec_begin, rec_frame, rec_end, nullptr);
    jce_renderer_set_backbuffer_capture(r, true);
    jce_renderer_set_capture_imgui_mode(true);
    jce_imgui_renderer_set_recording(true);
    LOG_SUCCESS(LOG_TAG, "recording -> %s (audio=%d)", g.path, (int)g.have_audio);
    return true;
}
extern "C" void jce_editor_recorder_stop(void) {
    if (!control_allowed()) {
        LOG_ERROR(LOG_TAG, "Stop requires Editor main thread outside capture callbacks; owner retained");
        return;
    }
    if (!g.active || g.stopping) return;
    g.stopping = true; /* Retain the main owner across destroy/log reentry. */
    close_admission();
    jce_imgui_renderer_set_recording(false);
    jce_renderer_set_capture_imgui_mode(false);
    jce_renderer_set_backbuffer_capture(g.renderer, false);
    jce_renderer_set_capture_sink(nullptr, nullptr, nullptr, nullptr);
    if (g.have_audio) jce_audio_master_tap_set(nullptr, nullptr);
    wait_borrowers(); /* clear sink is not a join; this is the actual borrower fence */
    jce_mutex_lock(g.mtx);
    g.worker_run = false;
    jce_cond_signal(g.cond);
    jce_mutex_unlock(g.mtx);
    jce_thread_join(g.worker);
    g.worker = nullptr;
    /* The last accepted callback or drained frame can still reject format. */
    report_format_rejection();
    /* Completed counters no longer need a session lock. A synchronous
     * destroy/log sink may query them; a new Start must still refuse. */
    g.active = false;
    jce_cond_destroy(g.cond); jce_mutex_destroy(g.mtx);
    LOG_SUCCESS(LOG_TAG, "stopped: %u video (%u dropped, inflight peak %u/%u), %u audio (%u dropped) -> %s",
        g.vwritten, g.vdropped, g.vpeak, VID_INFLIGHT_MAX, g.awritten, g.adropped, g.path);
    g.mtx = nullptr; g.cond = nullptr;
    g.stopping = false;
}
extern "C" void jce_editor_recorder_update(void) {
    if (!control_allowed() || !g.active || g.stopping) return;
    jce_mutex_lock(g.mtx);
    const bool stop_requested = g.reject_reason != VideoReject::none;
    jce_mutex_unlock(g.mtx);
    if (stop_requested) jce_editor_recorder_stop();
}
extern "C" bool jce_editor_recorder_is_active(void) { return g.active; }
extern "C" uint32_t jce_editor_recorder_frame_count(void) {
    if (!g.active) return g.vwritten;
    jce_mutex_lock(g.mtx); const uint32_t n = g.vwritten; jce_mutex_unlock(g.mtx); return n;
}
extern "C" uint32_t jce_editor_recorder_dropped(void) {
    if (!g.active) return g.vdropped;
    jce_mutex_lock(g.mtx); const uint32_t n = g.vdropped; jce_mutex_unlock(g.mtx); return n;
}
