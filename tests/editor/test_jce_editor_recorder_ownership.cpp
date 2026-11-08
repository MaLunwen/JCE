/* Complete Recorder owner; typed capture/codec/allocator ports, real SDK
 * mutexes, I32 atomics, TLS, conditions, dedicated threads and joins. No GPU. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/ui/jce_imgui_renderer.h>
#include <jce/middleware/video/jce_webm_encoder.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_log.h>
#include "core/jce_editor_alloc.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include <new>
#include <cstdarg>

namespace {
constexpr uint32_t PIXELS = 16u * 16u * 4u;
constexpr uint32_t LARGE_PIXELS = 32u * 32u * 4u;
void JCE_CALL recorder_log(JceLogLevel, const char *, const char *, int, const char *, ...);
void *recorder_alloc(size_t);
void recorder_free(void *);
void *recorder_copy(void *, const void *, size_t);
void recorder_lock(JceMutex *);
void recorder_destroy_mutex(JceMutex *);
JceThread *recorder_spawn(JceThreadFn, void *, const char *);
void recorder_sink(JceCaptureBeginFn, JceCaptureFrameFn, JceCaptureEndFn, void *);
void recorder_backbuffer(JceRenderer *, bool);
void recorder_imgui_mode(bool);
void recorder_recording(bool);
bool recorder_audio_tap(JceAudioMasterTapFn, void *);
JceWebmEncoder *recorder_encoder_create(const char *, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t);
bool recorder_encoder_video(JceWebmEncoder *, const void *, uint32_t, int, uint64_t);
bool recorder_encoder_audio(JceWebmEncoder *, const float *, uint32_t, uint64_t);
void recorder_encoder_finish(JceWebmEncoder *);
}
#undef ED_MALLOC
#undef ED_FREE
#define ED_MALLOC recorder_alloc
#define ED_FREE recorder_free
#define jce_log_write recorder_log
#define memcpy recorder_copy
#define jce_mutex_lock recorder_lock
#define jce_mutex_destroy recorder_destroy_mutex
#define jce_thread_create recorder_spawn
#define jce_renderer_set_capture_sink recorder_sink
#define jce_renderer_set_backbuffer_capture recorder_backbuffer
#define jce_renderer_set_capture_imgui_mode recorder_imgui_mode
#define jce_imgui_renderer_set_recording recorder_recording
#define jce_audio_master_tap_set recorder_audio_tap
#define jce_webm_encoder_create recorder_encoder_create
#define jce_webm_encoder_push_bgra recorder_encoder_video
#define jce_webm_encoder_push_audio recorder_encoder_audio
#define jce_webm_encoder_finish recorder_encoder_finish
#include JCE_TEST_RECORDER_BODY
#undef jce_log_write
#undef memcpy
#undef jce_mutex_lock
#undef jce_mutex_destroy
#undef jce_thread_create
#undef jce_renderer_set_capture_sink
#undef jce_renderer_set_backbuffer_capture
#undef jce_renderer_set_capture_imgui_mode
#undef jce_imgui_renderer_set_recording
#undef jce_audio_master_tap_set
#undef jce_webm_encoder_create
#undef jce_webm_encoder_push_bgra
#undef jce_webm_encoder_push_audio
#undef jce_webm_encoder_finish

namespace {
enum Pause { NONE, NODE_ALLOC, PIXEL_COPY, QUEUE_PUBLISH, PAYLOAD_FREE };
struct Block { void *p; size_t size; };
struct Ports {
    JceMutex *lock = nullptr;
    JceTLS *role = nullptr;
    JceTLS *copied = nullptr;
    JceAtomicI32 *worker_release = nullptr, *relock_release = nullptr;
    JceAtomicI32 *relock_entered = nullptr, *encode_release = nullptr;
    JceAtomicI32 *encode_entered = nullptr, *worker_done = nullptr;
    JceAtomicI32 *pause_entered = nullptr, *pause_release = nullptr;
    JceAtomicI32 *closed_observed = nullptr;
    JceThreadFn worker_fn = nullptr; void *worker_arg = nullptr;
    JceCaptureBeginFn begin = nullptr; JceCaptureFrameFn frame = nullptr;
    JceAudioMasterTapFn tap = nullptr;
    Block blocks[128] = {};
    int alloc_calls = 0, copy_calls = 0, live_payloads = 0, peak_payloads = 0;
    int fail_alloc = 0, throw_alloc = 0, throw_copy = 0;
    int destroyed = 0, unsafe_destroy = 0, creates = 0, finishes = 0;
    bool destroy_inactive = false, destroy_getters = false, destroy_reentry = false;
    int encoded = 0, create_error = 0, push_error = 0;
    int extent_violations = 0, format_errors = 0;
    bool corrupt_seed_at_create = false, log_reentry = false, log_retained = false;
    int worker_locks = 0;
    bool pause_relock = false, recursive_stop = false, recursive_retained = false;
    bool audio_enabled = false, audio_producer = false;
    Pause pause = NONE;
    uint32_t width = 0, height = 0, fps = 0, bitrate = 0, rate = 0, channels = 0;
};
Ports p;
unsigned char pixels[PIXELS] = {};
unsigned char large_pixels[LARGE_PIXELS] = {};
int producer_role, worker_role;

void wait_flag(JceAtomicI32 *v) {
    while (!jce_atomic_i32_load(v)) jce_thread_sleep_ms(1);
}
bool observe(JceAtomicI32 *v) {
    const uint64_t end = (uint64_t)jce_time_ticks_ms() + 5000;
    while (!jce_atomic_i32_load(v) && (uint64_t)jce_time_ticks_ms() < end)
        jce_thread_sleep_ms(1);
    return jce_atomic_i32_load(v) != 0;
}
void pause_at(Pause at) {
    if (p.pause != at) return;
    jce_atomic_i32_store(p.pause_entered, 1);
    wait_flag(p.pause_release);
}
void *recorder_alloc(size_t n) {
    if (jce_tls_get(p.role) == &producer_role &&
        n == (p.audio_producer ? sizeof(AudChunk) : sizeof(VidFrame))) pause_at(NODE_ALLOC);
    jce_mutex_lock(p.lock);
    const int call = ++p.alloc_calls;
    const bool fail = call == p.fail_alloc, throws = call == p.throw_alloc;
    jce_mutex_unlock(p.lock);
    if (throws) throw std::bad_alloc();
    if (fail) return nullptr;
    const jce_allocator_t allocator = jce_allocator_default();
    void *result = allocator.alloc(n, allocator.ctx);
    if (!result) return nullptr;
    jce_mutex_lock(p.lock);
    for (auto &b : p.blocks) if (!b.p) { b = {result, n}; break; }
    if (n == PIXELS || n == LARGE_PIXELS) {
        ++p.live_payloads;
        if (p.live_payloads > p.peak_payloads) p.peak_payloads = p.live_payloads;
    }
    jce_mutex_unlock(p.lock);
    return result;
}
void recorder_free(void *ptr) {
    if (!ptr) return;
    jce_mutex_lock(p.lock);
    size_t size = 0;
    for (auto &b : p.blocks) if (b.p == ptr) { size = b.size; break; }
    jce_mutex_unlock(p.lock);
    if (size == PIXELS || size == LARGE_PIXELS) pause_at(PAYLOAD_FREE); /* BEFORE actual allocator free */
    const jce_allocator_t allocator = jce_allocator_default();
    allocator.free(ptr, allocator.ctx);
    jce_mutex_lock(p.lock);
    for (auto &b : p.blocks) if (b.p == ptr) { b = {}; break; }
    if (size == PIXELS || size == LARGE_PIXELS) --p.live_payloads;
    jce_mutex_unlock(p.lock);
}
void *recorder_copy(void *dest, const void *src, size_t n) {
    if (jce_tls_get(p.role) == &producer_role) pause_at(PIXEL_COPY);
    jce_mutex_lock(p.lock); ++p.copy_calls; const bool throws = p.throw_copy; jce_mutex_unlock(p.lock);
    if (throws) throw std::bad_alloc();
    void *result = std::memcpy(dest, src, n);
    if (jce_tls_get(p.role) == &producer_role) jce_tls_set(p.copied, &producer_role);
    if (p.recursive_stop) {
        jce_editor_recorder_update();
        jce_editor_recorder_stop();
        p.recursive_retained = g.active && g.mtx && g.worker;
    }
    return result;
}
void recorder_lock(JceMutex *m) {
    if (m == g.mtx && jce_tls_get(p.role) == &worker_role) {
        ++p.worker_locks;
        if (p.pause_relock && p.worker_locks == 2) {
            jce_atomic_i32_store(p.relock_entered, 1);
            wait_flag(p.relock_release);
        }
    }
    if (m == g.mtx && jce_tls_get(p.copied)) {
        jce_tls_set(p.copied, nullptr);
        pause_at(QUEUE_PUBLISH);
    }
    jce_mutex_lock(m);
}
void recorder_destroy_mutex(JceMutex *m) {
    if (m == g.mtx) {
        ++p.destroyed;
        if (jce_atomic_i32_load(admission.borrowers) || g.vinflight) ++p.unsafe_destroy;
        p.destroy_inactive = !jce_editor_recorder_is_active();
        p.destroy_getters = jce_editor_recorder_frame_count() == g.vwritten &&
            jce_editor_recorder_dropped() == g.vdropped;
        const bool started = jce_editor_recorder_start(
            reinterpret_cast<JceRenderer *>(&p), "reentrant.mkv");
        jce_editor_recorder_stop();
        p.destroy_reentry = !started && g.mtx == m && g.stopping && !g.active;
    }
    jce_mutex_destroy(m);
}
void worker_entry(void *) {
    jce_tls_set(p.role, &worker_role);
    wait_flag(p.worker_release);
    p.worker_fn(p.worker_arg);
    jce_atomic_i32_store(p.worker_done, 1);
}
JceThread *recorder_spawn(JceThreadFn fn, void *arg, const char *name) {
    p.worker_fn = fn; p.worker_arg = arg;
    return jce_thread_create(worker_entry, nullptr, name);
}
void recorder_sink(JceCaptureBeginFn begin, JceCaptureFrameFn frame, JceCaptureEndFn, void *) {
    p.begin = begin; p.frame = frame;
}
void recorder_backbuffer(JceRenderer *, bool) {}
void recorder_imgui_mode(bool) {}
void recorder_recording(bool) {}
bool recorder_audio_tap(JceAudioMasterTapFn tap, void *) {
    p.tap = tap;
    if (tap && p.audio_enabled) {
        float pcm[8] = {};
        tap(nullptr, pcm, 4, 48000, 2); /* immediate callback before admission opens */
    }
    return p.audio_enabled;
}
JceWebmEncoder *recorder_encoder_create(const char *, uint32_t w, uint32_t h, uint32_t fps,
    uint32_t bitrate, uint32_t rate, uint32_t channels) {
    ++p.creates; p.width = w; p.height = h; p.fps = fps;
    p.bitrate = bitrate; p.rate = rate; p.channels = channels;
    jce_atomic_i32_store(p.encode_entered, 1);
    wait_flag(p.encode_release);
    if (p.corrupt_seed_at_create) {
        /* A typed provider corrupts only the declared extent, not allocation.
         * The second production guard must refuse before its codec call. */
        for (auto &b : p.blocks) if (b.p && b.size == sizeof(VidFrame)) {
            auto *frame = static_cast<VidFrame *>(b.p);
            if (frame->format.width == w) { --frame->bytes; break; }
        }
    }
    if (p.create_error == 1) throw std::bad_alloc();
    if (p.create_error == 2) return nullptr;
    return reinterpret_cast<JceWebmEncoder *>(&p);
}
bool recorder_encoder_video(JceWebmEncoder *, const void *bgra, uint32_t pitch, int, uint64_t) {
    /* Model the real fixed-size I420 converter's last source byte, without
     * reading it. An old shared-format control fails normally, never OOB. */
    size_t available = 0;
    jce_mutex_lock(p.lock);
    for (const auto &b : p.blocks) if (b.p == bgra) { available = b.size; break; }
    jce_mutex_unlock(p.lock);
    const uint32_t width = p.width & ~1u, height = p.height & ~1u;
    const uint64_t row = uint64_t(width) * 4u;
    const uint64_t end = height ? uint64_t(height - 1u) * pitch + row : 0;
    if (!width || !height || pitch < row || !available || end > available) {
        ++p.extent_violations; return false;
    }
    if (p.push_error == 1) throw std::bad_alloc();
    if (p.push_error == 2) return false;
    ++p.encoded; return true;
}
bool recorder_encoder_audio(JceWebmEncoder *, const float *, uint32_t, uint64_t) { return true; }
void recorder_encoder_finish(JceWebmEncoder *) { ++p.finishes; }
void JCE_CALL recorder_log(JceLogLevel level, const char *, const char *, int, const char *fmt, ...) {
    char text[1024];
    va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    if (level != JCE_LOG_LEVEL_ERROR || !std::strstr(text, "capture format rejected")) return;
    ++p.format_errors;
    if (p.log_reentry) {
        const bool started = jce_editor_recorder_start(reinterpret_cast<JceRenderer *>(&p), "log-reentry.mkv");
        jce_editor_recorder_update(); jce_editor_recorder_stop();
        p.log_retained = !started && g.active && g.stopping && g.mtx && !g.worker;
    }
}
void produce(void *) {
    jce_tls_set(p.role, &producer_role);
    if (p.audio_producer) {
        float pcm[8] = {}; aud_cb(nullptr, pcm, 4, 48000, 2);
    } else rec_frame(nullptr, pixels, PIXELS);
    jce_tls_set(p.role, nullptr);
}
void release_all() {
    jce_atomic_i32_store(p.pause_release, 1);
    jce_atomic_i32_store(p.relock_release, 1);
    jce_atomic_i32_store(p.encode_release, 1);
    jce_atomic_i32_store(p.worker_release, 1);
}
struct Fixture {
    explicit Fixture(bool audio = false) {
        jce_thread_mark_main();
        JceTLS *role = p.role ? p.role : jce_tls_create(nullptr);
        JceTLS *copied = p.copied ? p.copied : jce_tls_create(nullptr);
        p = {};
        p.lock = jce_mutex_create();
        p.role = role; p.copied = copied; p.audio_enabled = audio;
        p.worker_release = jce_atomic_i32_create(0);
        p.relock_release = jce_atomic_i32_create(0); p.relock_entered = jce_atomic_i32_create(0);
        p.encode_release = jce_atomic_i32_create(0); p.encode_entered = jce_atomic_i32_create(0);
        p.worker_done = jce_atomic_i32_create(0);
        p.pause_entered = jce_atomic_i32_create(0); p.pause_release = jce_atomic_i32_create(0);
        p.closed_observed = jce_atomic_i32_create(0);
        REQUIRE(p.lock); REQUIRE(p.role); REQUIRE(p.copied);
        REQUIRE(p.worker_release); REQUIRE(p.relock_release); REQUIRE(p.relock_entered);
        REQUIRE(p.encode_release); REQUIRE(p.encode_entered); REQUIRE(p.worker_done);
        REQUIRE(p.pause_entered); REQUIRE(p.pause_release); REQUIRE(p.closed_observed);
        REQUIRE(jce_editor_recorder_start(reinterpret_cast<JceRenderer *>(&p), "recorder-port.mkv"));
        REQUIRE(p.begin); REQUIRE(p.frame);
        p.begin(nullptr, 16, 16, 64, 0);
    }
    ~Fixture() {
        release_all();
        jce_editor_recorder_stop();
        for (auto &b : p.blocks) CHECK(b.p == nullptr);
        CHECK(p.live_payloads == 0);
        CHECK(p.unsafe_destroy == 0);
        CHECK(p.extent_violations == 0);
        CHECK(p.destroy_inactive); CHECK(p.destroy_getters); CHECK(p.destroy_reentry);
        CHECK(g.vinflight == 0);
        jce_atomic_i32_destroy(p.worker_release);
        jce_atomic_i32_destroy(p.relock_release); jce_atomic_i32_destroy(p.relock_entered);
        jce_atomic_i32_destroy(p.encode_release); jce_atomic_i32_destroy(p.encode_entered);
        jce_atomic_i32_destroy(p.worker_done);
        jce_atomic_i32_destroy(p.pause_entered); jce_atomic_i32_destroy(p.pause_release);
        jce_atomic_i32_destroy(p.closed_observed);
        jce_mutex_destroy(p.lock);
        /* Public TLS has no destroy entry. Keep the finite test slots, just as
         * the SDK does; this fixture does not certify process heap extinction. */
    }
};
uint32_t inflight() {
    jce_mutex_lock(g.mtx); const auto n = g.vinflight; jce_mutex_unlock(g.mtx); return n;
}
void stop_release(void *) {
    wait_flag(admission.closed);
    const bool retained = g.active && g.mtx && inflight() == (p.audio_producer ? 0u : 1u) &&
        jce_atomic_i32_load(admission.borrowers) == 1 && p.destroyed == 0;
    jce_atomic_i32_store(p.closed_observed, retained ? 1 : -1);
    release_all();
}
}

TEST_CASE("Recorder counts seed detached queued and reserved buffers in one budget") {
    Fixture fixture;
    p.pause_relock = true;
    p.frame(nullptr, pixels, PIXELS); /* one seed before the next detachment */
    jce_atomic_i32_store(p.worker_release, 1);
    REQUIRE(observe(p.relock_entered));
    for (int n = 0; n < 12; ++n) p.frame(nullptr, pixels, PIXELS);
    jce_atomic_i32_store(p.relock_release, 1);
    REQUIRE(observe(p.encode_entered)); /* seed + whole next detached list alive */
    for (int n = 0; n < 12; ++n) p.frame(nullptr, pixels, PIXELS);
    CHECK(inflight() == 12);
    CHECK(p.live_payloads == 12);
    CHECK(p.peak_payloads <= 12);
    CHECK(g.vpeak <= 12);
    CHECK(p.alloc_calls == 24); /* two allocations for each admitted frame */
    CHECK(p.copy_calls == 12);
    CHECK(jce_editor_recorder_dropped() == 13);
    release_all();
    jce_editor_recorder_stop();
    CHECK(p.peak_payloads <= 12);
    CHECK(p.encoded == 12);
    CHECK(p.finishes == 1);
    CHECK(p.width == 16); CHECK(p.height == 16); CHECK(p.bitrate == 8000);
    CHECK(p.rate == 0); CHECK(p.channels == 0);
}
TEST_CASE("Recorder retains a credit until payload free actually returns") {
    Fixture fixture;
    for (int n = 0; n < 12; ++n) p.frame(nullptr, pixels, PIXELS);
    p.pause = PAYLOAD_FREE;
    jce_atomic_i32_store(p.encode_release, 1);
    jce_atomic_i32_store(p.worker_release, 1);
    REQUIRE(observe(p.pause_entered));
    const int allocations = p.alloc_calls, copies = p.copy_calls;
    CHECK(inflight() == 12);
    p.frame(nullptr, pixels, PIXELS);
    CHECK(p.alloc_calls == allocations);
    CHECK(p.copy_calls == copies);
    CHECK(p.live_payloads == 12);
    release_all();
    const uint64_t end = (uint64_t)jce_time_ticks_ms() + 5000;
    while (inflight() == 12 && (uint64_t)jce_time_ticks_ms() < end) jce_thread_sleep_ms(1);
    REQUIRE(inflight() < 12); /* actual free+credit return, not only release intent */
    p.frame(nullptr, pixels, PIXELS);
    CHECK(p.alloc_calls == allocations + 2);
    CHECK(p.copy_calls == copies + 1);
    CHECK(g.vcaptured == 13);
}
TEST_CASE("Recorder Stop waits actual producers at allocation copy and publication") {
    for (bool audio : {false, true}) for (Pause at : {NODE_ALLOC, PIXEL_COPY, QUEUE_PUBLISH}) {
        CAPTURE(audio); CAPTURE(at);
        Fixture fixture(audio);
        p.audio_producer = audio;
        p.pause = at;
        JceThread *producer = jce_thread_create(produce, nullptr, "rec-producer");
        REQUIRE(producer);
        if (!observe(p.pause_entered)) { release_all(); jce_thread_join(producer); FAIL("producer did not reach boundary"); }
        JceThread *release = jce_thread_create(stop_release, nullptr, "rec-stop-release");
        if (!release) { release_all(); jce_thread_join(producer); FAIL("release thread failed"); }
        jce_editor_recorder_stop();
        jce_thread_join(producer); jce_thread_join(release);
        CHECK(jce_atomic_i32_load(p.closed_observed) == 1);
        CHECK(p.destroyed == 1);
        CHECK(p.unsafe_destroy == 0);
        CHECK(p.destroy_inactive); CHECK(p.destroy_getters); CHECK(p.destroy_reentry);
        CHECK(g.vinflight == 0);
        /* A saved function pointer may still arrive after unregistration. */
        const int allocations = p.alloc_calls;
        rec_begin(nullptr, 99, 99, 396, 1);
        rec_frame(nullptr, pixels, PIXELS);
        float pcm[8] = {};
        aud_cb(nullptr, pcm, 4, 48000, 2);
        CHECK(p.alloc_calls == allocations);
        CHECK(g.width == 16);
    }
}
TEST_CASE("Recorder OOM and throwing capture providers return reserved credits") {
    for (int failure = 0; failure < 5; ++failure) {
        CAPTURE(failure);
        Fixture fixture;
        if (failure < 2) p.fail_alloc = failure + 1;
        else if (failure < 4) p.throw_alloc = failure - 1;
        else p.throw_copy = 1;
        p.frame(nullptr, pixels, PIXELS);
        CHECK(inflight() == 0);
        CHECK(p.live_payloads == 0);
        CHECK(jce_atomic_i32_load(admission.borrowers) == 0);
        p.fail_alloc = p.throw_alloc = p.throw_copy = 0;
        p.frame(nullptr, pixels, PIXELS);
        CHECK(inflight() == 1);
    }
}
TEST_CASE("Recorder refuses callback-recursive Stop and retains the live owner") {
    Fixture fixture;
    p.recursive_stop = true;
    p.frame(nullptr, pixels, PIXELS);
    CHECK(p.recursive_retained);
    CHECK(jce_editor_recorder_is_active());
    CHECK(jce_atomic_i32_load(admission.borrowers) == 0);
    CHECK(inflight() == 1);
    p.recursive_stop = false;
}
TEST_CASE("Recorder encoder failure releases current seed detached and queued ownership") {
    for (int failure = 0; failure < 4; ++failure) {
        CAPTURE(failure);
        Fixture fixture;
        p.create_error = failure < 2 ? failure + 1 : 0;
        p.push_error = failure >= 2 ? failure - 1 : 0;
        for (int n = 0; n < 12; ++n) p.frame(nullptr, pixels, PIXELS);
        release_all();
        if (p.create_error == 1 || p.push_error == 1) {
            REQUIRE(observe(p.worker_done));
            CHECK(jce_atomic_i32_load(admission.closed) == 1);
            CHECK(inflight() == 0);
        }
        jce_editor_recorder_stop();
        CHECK(g.vinflight == 0);
        CHECK(p.live_payloads == 0);
        CHECK(p.unsafe_destroy == 0);
    }
}
TEST_CASE("Recorder normal single frame retains fallback codec configuration") {
    Fixture fixture;
    p.frame(nullptr, pixels, PIXELS);
    release_all();
    jce_editor_recorder_stop();
    CHECK(p.encoded == 1); CHECK(p.creates == 1); CHECK(p.finishes == 1);
    CHECK(p.fps == 30); CHECK(p.bitrate == 8000);
    CHECK(g.vcaptured == 1); CHECK(g.vwritten == 1); CHECK(g.vdropped == 0);
    CHECK(measure_fps(10, 10) == 30);
    CHECK(measure_fps(10, 11) == 240);
    CHECK(measure_fps(10, 1010) == 1);
}

TEST_CASE("Recorder audio admission opens only after complete startup and retains track settings") {
    Fixture fixture(true);
    CHECK(p.alloc_calls == 0); /* typed tap invoked its immediate callback during Start */
    REQUIRE(p.tap);
    float pcm[8] = {};
    p.tap(nullptr, pcm, 4, 48000, 2);
    p.frame(nullptr, pixels, PIXELS);
    release_all();
    jce_editor_recorder_stop();
    CHECK(p.rate == 48000); CHECK(p.channels == 2);
    CHECK(p.encoded == 1); CHECK(g.awritten == 1);
    CHECK(p.finishes == 1);
}

TEST_CASE("Recorder locks the admitted seed format and resize retires on main update") {
    Fixture fixture;
    p.log_reentry = true;
    p.frame(nullptr, pixels, PIXELS); /* Worker has not touched this small seed. */
    p.begin(nullptr, 32, 32, 128, 0);
    p.frame(nullptr, large_pixels, LARGE_PIXELS);
    CHECK(jce_editor_recorder_is_active()); CHECK(p.destroyed == 0);
    CHECK(p.alloc_calls == 2); CHECK(p.copy_calls == 1); CHECK(inflight() == 1);
    release_all();
    jce_editor_recorder_update();
    const bool auto_stopped = !jce_editor_recorder_is_active();
    jce_editor_recorder_stop(); /* Safe normal retirement even for old control. */
    CHECK(auto_stopped); CHECK(p.width == 16); CHECK(p.height == 16);
    CHECK(p.encoded == 1); CHECK(p.finishes == 1);
    CHECK(g.vcaptured == 1); CHECK(g.vdropped == 1);
    CHECK(p.format_errors == 1); CHECK(p.log_retained);
    jce_editor_recorder_update(); jce_editor_recorder_stop();
    CHECK(p.format_errors == 1);
    /* Same public Start/Stop path used by the next F9, at the new dimensions. */
    REQUIRE(jce_editor_recorder_start(reinterpret_cast<JceRenderer *>(&p), "resized.mkv"));
    p.begin(nullptr, 32, 32, 128, 0); p.frame(nullptr, large_pixels, LARGE_PIXELS);
    jce_editor_recorder_stop();
    CHECK(p.width == 32); CHECK(p.height == 32);
    CHECK(p.encoded == 2); CHECK(p.finishes == 2); CHECK(p.format_errors == 1);
}
TEST_CASE("Recorder rejects a smaller frame after fixed encoder construction") {
    Fixture fixture;
    p.begin(nullptr, 32, 32, 128, 0);
    p.frame(nullptr, large_pixels, LARGE_PIXELS); p.frame(nullptr, large_pixels, LARGE_PIXELS);
    jce_atomic_i32_store(p.worker_release, 1);
    REQUIRE(observe(p.encode_entered)); /* Real owner is in typed codec create. */
    p.begin(nullptr, 16, 16, 64, 0); p.frame(nullptr, pixels, PIXELS);
    CHECK(jce_editor_recorder_is_active()); CHECK(p.destroyed == 0);
    CHECK(p.alloc_calls == 4); CHECK(p.copy_calls == 2);
    release_all(); jce_editor_recorder_update();
    const bool auto_stopped = !jce_editor_recorder_is_active();
    jce_editor_recorder_stop();
    CHECK(auto_stopped); CHECK(p.encoded == 2); CHECK(p.finishes == 1);
    CHECK(p.width == 32); CHECK(p.height == 32); CHECK(p.format_errors == 1);
    CHECK(g.vcaptured == 2); CHECK(g.vdropped == 1);
}
TEST_CASE("Recorder validates declared extent before allocation copy or encoding") {
    struct Invalid { CaptureFormat format; uint32_t bytes; bool null; };
    const Invalid invalid[] = {
        {{16, 16, 64, 0}, PIXELS - 1, false},
        {{16, 16, 64, 0}, PIXELS + 1, false},
        {{UINT32_MAX, 16, 64, 0}, PIXELS, false},
        {{2, UINT32_MAX, UINT32_MAX, 0}, PIXELS, false},
        {{16, 16, 63, 0}, PIXELS, false},
        {{1, 16, 64, 0}, PIXELS, false},
        {{16, 16, 64, 2}, PIXELS, false},
        {{16, 16, 64, 0}, PIXELS, true}
    };
    for (const auto &item : invalid) {
        CAPTURE(item.format.width); CAPTURE(item.format.height); CAPTURE(item.bytes);
        Fixture fixture;
        p.begin(nullptr, item.format.width, item.format.height, item.format.pitch, item.format.yflip);
        p.frame(nullptr, item.null ? nullptr : pixels, item.bytes);
        CHECK(p.alloc_calls == 0); CHECK(p.copy_calls == 0); CHECK(inflight() == 0);
        CHECK(jce_editor_recorder_is_active()); CHECK(p.destroyed == 0);
        release_all(); jce_editor_recorder_update();
        CHECK_FALSE(jce_editor_recorder_is_active());
        CHECK(p.creates == 0); CHECK(p.encoded == 0); CHECK(p.format_errors == 1);
    }
}
TEST_CASE("Recorder revalidates owned seed extent before codec push") {
    Fixture fixture;
    p.corrupt_seed_at_create = true;
    p.frame(nullptr, pixels, PIXELS);
    release_all(); jce_editor_recorder_stop();
    CHECK(p.creates == 1); CHECK(p.encoded == 0); CHECK(p.extent_violations == 0);
    CHECK(p.finishes == 1); CHECK(p.format_errors == 1); CHECK(g.vdropped == 1);
}
TEST_CASE("Recorder accepts padded odd capture and validates before seed creation") {
    {
        Fixture fixture;
        p.begin(nullptr, 15, 3, 64, 1);
        p.frame(nullptr, pixels, 188); /* Last visible byte; final row pad absent. */
        release_all(); jce_editor_recorder_stop();
        CHECK(p.width == 15); CHECK(p.height == 3); CHECK(p.encoded == 1);
        CHECK(p.format_errors == 0); CHECK(p.fps == 30); CHECK(p.bitrate == 8000);
    }
    {
        Fixture fixture;
        p.frame(nullptr, pixels, PIXELS);
        REQUIRE(g.vhead); --g.vhead->bytes; /* Owned allocation remains intact. */
        release_all(); jce_editor_recorder_stop();
        CHECK(p.creates == 0); CHECK(p.encoded == 0); CHECK(p.format_errors == 1);
    }
}

TEST_CASE("Recorder resize main update waits copied old-format borrower before draining") {
    Fixture fixture;
    p.pause = PIXEL_COPY; p.recursive_stop = true;
    JceThread *producer = jce_thread_create(produce, nullptr, "rec-resize-producer");
    REQUIRE(producer);
    if (!observe(p.pause_entered)) { release_all(); jce_thread_join(producer); FAIL("producer did not reach copy"); }
    p.begin(nullptr, 32, 32, 128, 0); p.frame(nullptr, large_pixels, LARGE_PIXELS);
    CHECK(jce_editor_recorder_is_active()); CHECK(p.destroyed == 0);
    JceThread *release = jce_thread_create(stop_release, nullptr, "rec-resize-release");
    if (!release) { release_all(); jce_thread_join(producer); FAIL("release thread failed"); }
    jce_editor_recorder_update();
    jce_editor_recorder_stop(); /* Explicit close before joins, including old control. */
    jce_thread_join(producer); jce_thread_join(release);
    CHECK_FALSE(jce_editor_recorder_is_active());
    CHECK(jce_atomic_i32_load(p.closed_observed) == 1);
    CHECK(p.recursive_retained); CHECK(p.unsafe_destroy == 0);
    CHECK(p.encoded == 1); CHECK(p.width == 16); CHECK(p.height == 16);
    CHECK(p.format_errors == 1); CHECK(g.vinflight == 0);
}
