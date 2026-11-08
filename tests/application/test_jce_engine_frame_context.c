/* Exact Engine context helpers, render-boundary wires and release owner.
 * Typed public ports observe CPU state only; no window, renderer or GPU. */
#include "unity.h"
#include "application/jce_engine_kpi_capture.h"
#include "application/jce_engine_perf_hitch.h"
#include "application/jce_engine_kpi_clock.h"
#include "application/jce_engine_trace_scope.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include "application/jce_engine_callbacks.h"
#include <jce/os/platform/jce_window.h>
#include <SDL3/SDL_atomic.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

struct JceEngine {
    JceEngineKpiCapture frame_kpi;
    JceWindow *window;
    JceRenderer *renderer;
    JceEnginePerfHitch perf_hitch;
    JceEngineKpiClock kpi_clock;
    uint64_t frame_counter_prev, perf_freq;
    JceServices svc;
    bool ready;
};
static JceEngine *g_engine_owner;
static JceAppDesc g_app_desc;
static bool engine_owner_ready(const JceEngine *e) { return e && e->ready; }
typedef struct {
    unsigned point_calls, drawable_calls, flag_calls, native_calls, backend_calls;
    unsigned render_begin_calls, render_end_calls, export_calls, clock_reads;
    uint64_t clock_boundary;
    uint32_t flags[2], backend[2];
    int native_w[2], native_h[2];
    bool flag_valid[2], native_valid[2], admission;
    bool context_before_begin, end_before_observation;
    unsigned clock_logs;
    bool reported_before_clock_log, reenter_clock_log;
    char clock_message[512];
} ContextPorts;
typedef struct { char bytes[8192]; size_t size; unsigned closes; } ContextSink;
typedef struct { JceEngine engine; ContextSink sink; JceAsyncExecutor *executor; } ContextFixture;
static ContextPorts ports;
static JceEngine *active_engine;
static SDL_AtomicInt s_in_render_frame;
static unsigned char window_token, renderer_token;

void jce_window_get_point_size(const JceWindow *window, uint32_t *w, uint32_t *h)
{
    (void)window; ++ports.point_calls; *w = 640; *h = 360;
}
void jce_window_get_size(JceWindow *window, uint32_t *w, uint32_t *h)
{
    (void)window; ++ports.drawable_calls; *w = 1280; *h = 720;
}
bool jce_window_get_flags(const JceWindow *window, uint32_t *flags)
{
    (void)window; unsigned i = ports.flag_calls++;
    if (i >= 2) return false;
    if (i == 1) ports.end_before_observation = ports.render_end_calls == 1;
    if (!ports.flag_valid[i]) return false; /* Failed getter preserves output. */
    *flags = ports.flags[i]; return true;
}
static JceWindow *test_native_window(JceWindow *window) { return window; }
static bool test_native_size(JceWindow *window, int *w, int *h)
{
    (void)window; unsigned i = ports.native_calls++;
    if (i >= 2 || !ports.native_valid[i]) return false;
    *w = ports.native_w[i]; *h = ports.native_h[i]; return true;
}
uint32_t jce_renderer_get_frame_index(const JceRenderer *renderer)
{
    (void)renderer; unsigned i = ports.backend_calls++;
    return i < 2 ? ports.backend[i] : 999u;
}
void jce_renderer_begin_frame(const JceRenderer *renderer, JceWindow *window)
{
    (void)renderer; (void)window; ++ports.render_begin_calls;
    ports.context_before_begin = active_engine->frame_kpi.context_begun &&
        !active_engine->frame_kpi.context_ended && !active_engine->frame_kpi.context_sealed;
}
void jce_renderer_end_frame(const JceRenderer *renderer)
{
    (void)renderer; ++ports.render_end_calls;
}
bool jce_renderer_get_gpu_stats(JceGpuStats *out)
{
    ++ports.export_calls;
    if (out) memset(out, 0, sizeof *out);
    return false;
}
static bool jce_renderer_frame_admission_current_internal(const JceRenderer *renderer)
{
    (void)renderer; return ports.admission;
}
static uint64_t test_context_perf_counter(void)
{
    ++ports.clock_reads; return ports.clock_boundary;
}
static JceAppResult engine_application_result(JceEngine *engine, JceAppResult result)
{
    return jce_engine_kpi_capture_result(&engine->frame_kpi, result);
}
static void engine_report_kpi_clock(JceEngine *engine, uint64_t records);
static void counted_clock_log(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO,level);
    TEST_ASSERT_EQUAL_STRING("engine",tag);TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(line>0);TEST_ASSERT_NOT_NULL(active_engine);
    ++ports.clock_logs;
    ports.reported_before_clock_log=active_engine->kpi_clock.reported;
    va_list args;va_start(args,format);
    int size=vsnprintf(ports.clock_message,sizeof ports.clock_message,format,args);
    va_end(args);TEST_ASSERT_TRUE(size>0 && (size_t)size<sizeof ports.clock_message);
    if(ports.reenter_clock_log) {
        ports.reenter_clock_log=false;
        engine_report_kpi_clock(active_engine,2);
    }
}
static bool engine_cleanup_defer(JceEngine *engine, JceEngineTraceScope scope,
    int reason, const char *message)
{
    (void)engine;(void)scope;TEST_ASSERT_EQUAL_INT(4,reason);TEST_ASSERT_NOT_NULL(message);
    return false;
}
#define JCE_DEFAULT_FRAME_DT (1.0 / 60.0)
#define jce_time_perf_counter test_context_perf_counter
#define jce_window_sdl test_native_window
#define SDL_GetWindowSizeInPixels test_native_size
#undef JCE_PROFILE_ZONE_END
#define JCE_PROFILE_ZONE_END ((void)0)
#define LOG_TAG "engine"
#define jce_log_write counted_clock_log
#include JCE_ENGINE_FRAME_CONTEXT_OWNER
#undef jce_log_write
#undef LOG_TAG
#undef JCE_PROFILE_ZONE_END
#undef SDL_GetWindowSizeInPixels
#undef jce_window_sdl
#undef jce_time_perf_counter
#undef JCE_DEFAULT_FRAME_DT

static size_t SDLCALL context_write(void *user, const void *bytes, size_t size, SDL_IOStatus *status)
{
    ContextSink *sink = user;
    if (size >= sizeof sink->bytes - sink->size) { *status = SDL_IO_STATUS_ERROR; return 0; }
    memcpy(sink->bytes + sink->size, bytes, size); sink->size += size;
    sink->bytes[sink->size] = 0; *status = SDL_IO_STATUS_READY; return size;
}
static bool SDLCALL context_close(void *user)
{
    ++((ContextSink *)user)->closes; return true;
}
static bool SDLCALL context_flush(void *user, SDL_IOStatus *status)
{
    (void)user; *status = SDL_IO_STATUS_READY; return true;
}
static void attach_context(ContextFixture *fixture, bool enabled)
{
    JceAsyncExecutorConfig async; jce_async_executor_config_init(&async);
    async.mode = JCE_ASYNC_EXECUTION_COOPERATIVE; async.worker_count = 0; async.max_tasks = 8;
    fixture->executor = jce_async_executor_create(&async);
    TEST_ASSERT_NOT_NULL(fixture->executor);
    SDL_IOStreamInterface io; SDL_INIT_INTERFACE(&io);
    io.write = context_write; io.close = context_close; io.flush = context_flush;
    JceFrameKpiWriterConfig config; jce_frame_kpi_writer_config_init(&config, true);
    config.context_json = enabled; config.stream = SDL_OpenIO(&io, &fixture->sink);
    config.executor = fixture->executor; config.byte_capacity = 4096;
    config.record_capacity = 4; config.drain_bytes = 4096; config.drain_records = 1;
    TEST_ASSERT_NOT_NULL(config.stream);
    fixture->engine.frame_kpi.writer = jce_frame_kpi_writer_create(&config);
    TEST_ASSERT_NOT_NULL(fixture->engine.frame_kpi.writer);
    fixture->engine.frame_kpi.context_enabled = enabled;
    fixture->engine.window = (JceWindow *)&window_token;
    fixture->engine.renderer = (JceRenderer *)&renderer_token;
    active_engine = g_engine_owner = &fixture->engine;
    fixture->engine.ready = true;
    fixture->engine.perf_freq = 1000;
    jce_engine_perf_hitch_init(&fixture->engine.perf_hitch, false, 4.0);
}
static void dispose_context(ContextFixture *fixture)
{
    jce_engine_kpi_capture_destroy(&fixture->engine.frame_kpi);
    jce_async_executor_shutdown(fixture->executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    jce_async_executor_destroy(fixture->executor); fixture->executor = NULL; active_engine = g_engine_owner = NULL;
}
void setUp(void)
{
    memset(&ports, 0, sizeof ports);
    memset(&g_app_desc, 0, sizeof g_app_desc);
    jce_thread_mark_main();
    ports.clock_boundary = 7;
    ports.flag_valid[0] = ports.flag_valid[1] = true;
    ports.native_valid[0] = ports.native_valid[1] = ports.admission = true;
    ports.flags[1] = UINT32_MAX; ports.backend[1] = UINT32_MAX;
    ports.native_w[0] = 1280; ports.native_h[0] = 720;
    ports.native_w[1] = 1920; ports.native_h[1] = 1080;
    jce_perf_phase_set_enabled(0); SDL_SetAtomicInt(&s_in_render_frame, 1);
}
void tearDown(void) { jce_perf_phase_set_enabled(0); }

static void test_actual_render_boundaries_seal_and_terminal_drain_same_sample(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&fixture.engine.perf_hitch));
    test_context_render_begin(&fixture.engine);
    JceAppResult ended = test_context_render_end(&fixture.engine);
    bool begun = fixture.engine.frame_kpi.context_begun;
    bool end_sampled = fixture.engine.frame_kpi.context_ended;
    JceAppResult result = engine_frame_release(&fixture.engine, JCE_APP_SUCCESS, true);
    bool consumed = !fixture.engine.frame_kpi.context_sealed;
    uint64_t count = fixture.engine.frame_kpi.index;
    dispose_context(&fixture);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, ended); TEST_ASSERT_TRUE(begun); TEST_ASSERT_TRUE(end_sampled);
    TEST_ASSERT_TRUE(ports.context_before_begin); TEST_ASSERT_TRUE(ports.end_before_observation);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result); TEST_ASSERT_TRUE(consumed);
    TEST_ASSERT_EQUAL_UINT64(1, count); TEST_ASSERT_EQUAL_UINT(1, ports.export_calls);
    TEST_ASSERT_EQUAL_UINT(1, ports.point_calls); TEST_ASSERT_EQUAL_UINT(1, ports.drawable_calls);
    TEST_ASSERT_EQUAL_UINT(2, ports.flag_calls); TEST_ASSERT_EQUAL_UINT(2, ports.native_calls);
    TEST_ASSERT_EQUAL_UINT(2, ports.backend_calls);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"flags_begin\"\":0,\"\"flags_end\"\":4294967295"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"point_size\"\":[640,360],\"\"drawable_size\"\":[1280,720]"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"native_size_begin\"\":[1280,720],\"\"native_size_end\"\":[1920,1080]"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"backend_frame_begin\"\":0,\"\"backend_frame_end\"\":4294967295"));
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
}

static void test_delayed_export_reuses_sealed_context_without_getter_resampling(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
    engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    bool sealed = fixture.engine.frame_kpi.context_sealed;
    ports.flags[0] = ports.flags[1] = 123; ports.native_w[0] = ports.native_w[1] = 77;
    bool accepted = jce_engine_kpi_capture_export(&fixture.engine.frame_kpi, 9.0, false);
    unsigned flag_reads = ports.flag_calls, native_reads = ports.native_calls;
    JceAppResult terminal = jce_engine_kpi_capture_result(&fixture.engine.frame_kpi, JCE_APP_SUCCESS);
    dispose_context(&fixture);
    TEST_ASSERT_TRUE(sealed); TEST_ASSERT_TRUE(accepted); TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, terminal);
    TEST_ASSERT_EQUAL_UINT(2, flag_reads); TEST_ASSERT_EQUAL_UINT(2, native_reads);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"native_size_end\"\":[1920,1080]"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, ":123"));
}

static void test_missing_window_and_failed_observations_preserve_nullable_meaning(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    fixture.engine.window = NULL;
    test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
    engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.engine.frame_kpi, 1.0, false));
    jce_engine_kpi_capture_result(&fixture.engine.frame_kpi, JCE_APP_SUCCESS);
    dispose_context(&fixture);
    TEST_ASSERT_EQUAL_UINT(0, ports.point_calls + ports.drawable_calls + ports.flag_calls + ports.native_calls);
    TEST_ASSERT_EQUAL_UINT(2, ports.backend_calls);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"window_present\"\":false,\"\"flags_begin\"\":null"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"backend_frame_begin\"\":0"));

    setUp(); memset(&fixture, 0, sizeof fixture); attach_context(&fixture, true);
    ports.flag_valid[0] = ports.flag_valid[1] = false;
    ports.native_valid[0] = false; ports.native_h[1] = 0;
    fixture.engine.renderer = NULL;
    test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
    engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.engine.frame_kpi, 2.0, false));
    jce_engine_kpi_capture_result(&fixture.engine.frame_kpi, JCE_APP_SUCCESS);
    dispose_context(&fixture);
    TEST_ASSERT_EQUAL_UINT(0, ports.backend_calls);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"flags_begin\"\":null,\"\"flags_end\"\":null"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"native_size_begin\"\":null,\"\"native_size_end\"\":null"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"backend_frame_begin\"\":null,\"\"backend_frame_end\"\":null"));
}

static void test_disabled_limited_and_failed_render_paths_do_not_fabricate_context(void)
{
    for (unsigned inactive = 0; inactive < 4; ++inactive) {
        ContextFixture fixture = {0}; attach_context(&fixture, inactive != 0);
        if (inactive == 1) fixture.engine.frame_kpi.limit = fixture.engine.frame_kpi.index = 1;
        if (inactive == 2) fixture.engine.frame_kpi.failed = true;
        if (inactive == 3) fixture.engine.frame_kpi.reported = true;
        test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
        bool begun = fixture.engine.frame_kpi.context_begun;
        bool ended = fixture.engine.frame_kpi.context_ended;
        /* This control does not pretend a record was admitted at the limit. */
        fixture.engine.frame_kpi.index = 0;
        dispose_context(&fixture);
        TEST_ASSERT_FALSE(begun); TEST_ASSERT_FALSE(ended);
        TEST_ASSERT_EQUAL_UINT(0, ports.point_calls + ports.drawable_calls + ports.flag_calls + ports.native_calls + ports.backend_calls);
    }
    setUp(); ContextFixture fixture = {0}; attach_context(&fixture, true);
    test_context_render_begin(&fixture.engine); ports.admission = false;
    JceAppResult result = test_context_render_end(&fixture.engine);
    bool ended = fixture.engine.frame_kpi.context_ended, sealed = fixture.engine.frame_kpi.context_sealed;
    uint64_t records = fixture.engine.frame_kpi.index;
    dispose_context(&fixture);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result); TEST_ASSERT_FALSE(ended); TEST_ASSERT_FALSE(sealed);
    TEST_ASSERT_EQUAL_UINT64(0, records); TEST_ASSERT_EQUAL_UINT(1, ports.flag_calls);
    TEST_ASSERT_EQUAL_UINT(0, ports.export_calls);
}
static void test_direct_close_reports_saved_nonzero_writer_ordinal_once_after_reset(void)
{
    for(unsigned enabled=0;enabled<2;++enabled) {
        setUp();ContextFixture fixture={0};attach_context(&fixture,false);
        TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.engine.frame_kpi,1.0,false));
        TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.engine.frame_kpi,2.0,false));
        TEST_ASSERT_EQUAL_UINT64(2,fixture.engine.frame_kpi.index);
        jce_engine_kpi_clock_init(&fixture.engine.kpi_clock,enabled!=0,1000,0);
        if(enabled) {
            TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&fixture.engine.kpi_clock,1000,0,10,0));
            TEST_ASSERT_TRUE(jce_engine_kpi_clock_record(&fixture.engine.kpi_clock,1000,10,20,1));
            TEST_ASSERT_TRUE(jce_engine_kpi_clock_check(&fixture.engine.kpi_clock,1000,20));
            ports.reenter_clock_log=true;
        }
        TEST_ASSERT_TRUE(test_context_direct_capture_close(&fixture.engine,20));
        TEST_ASSERT_EQUAL_UINT64(0,fixture.engine.frame_kpi.index); /* Real destroy memset ran. */
        TEST_ASSERT_EQUAL_UINT(1,fixture.sink.closes);
        TEST_ASSERT_TRUE(test_context_direct_capture_close(&fixture.engine,99));
        dispose_context(&fixture);
        TEST_ASSERT_EQUAL_UINT(enabled,ports.clock_logs);
        TEST_ASSERT_EQUAL_UINT(0,fixture.engine.kpi_clock.errors);
        if(enabled) {
            TEST_ASSERT_TRUE(ports.reported_before_clock_log);
            TEST_ASSERT_NOT_NULL(strstr(ports.clock_message,"render_intervals=2 "));
            TEST_ASSERT_NOT_NULL(strstr(ports.clock_message,"valid=1 errors=0"));
        } else TEST_ASSERT_FALSE(fixture.engine.kpi_clock.reported);
    }
}

typedef struct {
    bool enabled, accepted, wrong_thread_disabled, wrong_thread_rejected;
    bool invalids_rejected, copied, duplicate_rejected;
    bool empty, suppress;
} ViewportProbe;

static void wrong_thread_observation(void *user)
{
    ViewportProbe *probe = user;
    probe->wrong_thread_disabled = !jce_engine_frame_observation_enabled();
    probe->wrong_thread_rejected = !jce_engine_report_frame_observations(NULL, 0, 0);
}

static void observation_callback(const JceServices *svc, void *user)
{
    (void)svc;
    ViewportProbe *probe = user;
    probe->enabled = jce_engine_frame_observation_enabled();
    if (probe->suppress) return;
    if (probe->empty) {
        probe->accepted = jce_engine_report_frame_observations(NULL, 0, 0);
        return;
    }
    JceFrameViewportObservation v = {0};
    v.struct_size = sizeof v; v.source_id = 77;
    v.cpu_outcome = JCE_FRAME_VIEWPORT_CPU_SKIPPED; v.refusal_reason = 9;
    bool rejected = !jce_engine_report_frame_observations(&v, 5, sizeof v) &&
        !jce_engine_report_frame_observations(NULL, 1, sizeof v) &&
        !jce_engine_report_frame_observations(&v, 0, 0) &&
        !jce_engine_report_frame_observations(&v, 1, sizeof v - 1) &&
        !jce_engine_report_frame_observations(&v, 1, 4097) &&
        !jce_engine_report_frame_observations((const JceFrameViewportObservation *)
            ((const unsigned char *)&v + 1), 1, sizeof v);
    JceFrameViewportObservation invalid = v;
    invalid.struct_size = sizeof v - 4;
    rejected = rejected && !jce_engine_report_frame_observations(&invalid, 1, sizeof v);
    invalid = v; invalid.struct_size = sizeof v + 4;
    rejected = rejected && !jce_engine_report_frame_observations(&invalid, 1, sizeof v);
    invalid = v; invalid.flags = 256;
    rejected = rejected && !jce_engine_report_frame_observations(&invalid, 1, sizeof v);
    invalid = v; invalid.cpu_outcome = 4;
    rejected = rejected && !jce_engine_report_frame_observations(&invalid, 1, sizeof v);
    invalid = v; invalid.requested_width = 100;
    rejected = rejected && !jce_engine_report_frame_observations(&invalid, 1, sizeof v);
    JceFrameViewportObservation pair[2] = {v, v};
    rejected = rejected && !jce_engine_report_frame_observations(pair, 2, sizeof v);
    probe->invalids_rejected = rejected && !active_engine->frame_kpi.context.viewport_reported;
    JceThread *thread = jce_thread_create(wrong_thread_observation, probe, "observation-control");
    if (thread) jce_thread_join(thread);
    probe->accepted = jce_engine_report_frame_observations(&v, 1, sizeof v);
    v.source_id = 999;
    probe->copied = active_engine->frame_kpi.context.viewport_count == 1 &&
        active_engine->frame_kpi.context.viewports[0].source_id == 77;
    JceFrameKpiContext saved = active_engine->frame_kpi.context;
    probe->duplicate_rejected = !jce_engine_report_frame_observations(NULL, 0, 0) &&
        memcmp(&saved, &active_engine->frame_kpi.context, sizeof saved) == 0;
}

static void test_public_v2_observations_are_draw_scoped_copied_and_transactional(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    fixture.engine.frame_kpi.context_version = 2;
    ViewportProbe probe = {0}; g_app_desc.draw = observation_callback; g_app_desc.user_data = &probe;
    bool before = jce_engine_report_frame_observations(NULL, 0, 0);
    test_context_render_begin(&fixture.engine);
    bool outside = jce_engine_frame_observation_enabled();
    test_context_app_draw(&fixture.engine);
    bool after = jce_engine_frame_observation_enabled();
    test_context_render_end(&fixture.engine);
    engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    bool exported = jce_engine_kpi_capture_export(&fixture.engine.frame_kpi, 9.0, false);
    dispose_context(&fixture);
    TEST_ASSERT_FALSE(before); TEST_ASSERT_FALSE(outside); TEST_ASSERT_FALSE(after);
    TEST_ASSERT_TRUE(probe.enabled); TEST_ASSERT_TRUE(probe.invalids_rejected);
    TEST_ASSERT_TRUE(probe.wrong_thread_disabled); TEST_ASSERT_TRUE(probe.wrong_thread_rejected);
    TEST_ASSERT_TRUE(probe.accepted); TEST_ASSERT_TRUE(probe.copied); TEST_ASSERT_TRUE(probe.duplicate_rejected);
    TEST_ASSERT_TRUE(exported);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"v\"\":2"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"source_id\"\":77"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"requested_size\"\":null"));
}

static void test_v2_empty_unreported_and_next_interval_remain_distinct(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    fixture.engine.frame_kpi.context_version = 2;
    ViewportProbe probe = {0}; probe.empty = true;
    g_app_desc.draw = observation_callback; g_app_desc.user_data = &probe;
    test_context_render_begin(&fixture.engine); test_context_app_draw(&fixture.engine);
    test_context_render_end(&fixture.engine); engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    bool first = jce_engine_kpi_capture_export(&fixture.engine.frame_kpi, 1.0, false);
    bool explicit_empty = fixture.engine.frame_kpi.context.viewport_reported &&
        fixture.engine.frame_kpi.context.viewport_count == 0;
    probe.suppress = true;
    test_context_render_begin(&fixture.engine); test_context_app_draw(&fixture.engine);
    bool reset = !fixture.engine.frame_kpi.context.viewport_reported;
    test_context_render_end(&fixture.engine); engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    bool second = jce_engine_kpi_capture_export(&fixture.engine.frame_kpi, 2.0, false);
    dispose_context(&fixture);
    TEST_ASSERT_TRUE(probe.accepted); TEST_ASSERT_TRUE(explicit_empty); TEST_ASSERT_TRUE(reset);
    TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(second);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"viewport_reported\"\":true,\"\"viewports\"\":[]"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"viewport_reported\"\":false,\"\"viewports\"\":[]"));
}

static void test_off_v1_and_inactive_owner_never_admit_viewport_reports(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        setUp(); ContextFixture fixture = {0}; attach_context(&fixture, mode != 0);
        fixture.engine.frame_kpi.context_version = mode;
        if (mode == 2) fixture.engine.ready = false;
        ViewportProbe probe = {0}; probe.empty = true;
        g_app_desc.draw = observation_callback; g_app_desc.user_data = &probe;
        test_context_render_begin(&fixture.engine); test_context_app_draw(&fixture.engine);
        bool recorded = fixture.engine.frame_kpi.context.viewport_reported;
        dispose_context(&fixture);
        TEST_ASSERT_FALSE(probe.enabled); TEST_ASSERT_FALSE(probe.accepted); TEST_ASSERT_FALSE(recorded);
    }
    TEST_ASSERT_FALSE(jce_engine_frame_observation_enabled());
}


static void test_v3_actual_completed_pending_and_terminal_intervals_use_borrowed_clock(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    fixture.engine.frame_kpi.context_version = 3;
    fixture.engine.frame_counter_prev = UINT64_MAX - 9;
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&fixture.engine.perf_hitch));
    ViewportProbe probe = {0};
    g_app_desc.draw = observation_callback; g_app_desc.user_data = &probe;
    test_context_render_begin(&fixture.engine); test_context_app_draw(&fixture.engine);
    test_context_render_end(&fixture.engine);
    engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
    unsigned render_end_reads = ports.clock_reads;
    bool first = engine_export_pending_interval(&fixture.engine, UINT64_MAX);
    uint64_t records = fixture.engine.frame_kpi.index;
    bool captured = fixture.engine.frame_kpi.context.clock_valid &&
        fixture.engine.frame_kpi.context.clock_begin == UINT64_MAX - 9 &&
        fixture.engine.frame_kpi.context.clock_end == UINT64_MAX;
    unsigned pending_reads = ports.clock_reads;
    probe.suppress = true;
    fixture.engine.frame_counter_prev = ports.clock_boundary = UINT64_MAX;
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&fixture.engine.perf_hitch));
    test_context_render_begin(&fixture.engine); test_context_app_draw(&fixture.engine);
    bool reset = !fixture.engine.frame_kpi.context.clock_valid &&
        !fixture.engine.frame_kpi.context.viewport_reported;
    test_context_render_end(&fixture.engine);
    unsigned before_terminal_reads = ports.clock_reads;
    JceAppResult terminal = engine_frame_release(&fixture.engine, JCE_APP_SUCCESS, true);
    uint64_t total = fixture.engine.frame_kpi.index;
    unsigned clock_reads = ports.clock_reads;
    dispose_context(&fixture);
    TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(captured); TEST_ASSERT_EQUAL_UINT64(1, records);
    TEST_ASSERT_TRUE(probe.enabled); TEST_ASSERT_TRUE(probe.accepted); TEST_ASSERT_TRUE(probe.copied);
    TEST_ASSERT_TRUE(reset); TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, terminal);
    TEST_ASSERT_EQUAL_UINT64(2, total);
    /* The actual end_frame wire already brackets its call with two reads. */
    TEST_ASSERT_EQUAL_UINT(2, render_end_reads);
    TEST_ASSERT_EQUAL_UINT(render_end_reads, pending_reads);
    TEST_ASSERT_EQUAL_UINT(4, before_terminal_reads);
    TEST_ASSERT_EQUAL_UINT(before_terminal_reads + 1, clock_reads);
    TEST_ASSERT_EQUAL_UINT(2, ports.point_calls); TEST_ASSERT_EQUAL_UINT(2, ports.drawable_calls);
    TEST_ASSERT_EQUAL_UINT(4, ports.native_calls); TEST_ASSERT_EQUAL_UINT(4, ports.flag_calls);
    TEST_ASSERT_EQUAL_UINT(4, ports.backend_calls);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_wall,9.0000,"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "1,frame_wall,0.0000,"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"v\"\":3"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes,
        "\"\"frequency_hz\"\":\"\"1000\"\",\"\"begin\"\":\"\"18446744073709551606\"\","
        "\"\"end\"\":\"\"18446744073709551615\"\""));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes,
        "\"\"viewport_reported\"\":false,\"\"viewports\"\":[],\"\"clock\"\""));
}

static void test_v3_actual_terminal_failure_keeps_its_completed_interval(void)
{
    ContextFixture fixture = {0}; attach_context(&fixture, true);
    fixture.engine.frame_kpi.context_version = 3;
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&fixture.engine.perf_hitch));
    test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
    unsigned before_terminal_reads = ports.clock_reads;
    JceAppResult result = engine_frame_release(&fixture.engine, JCE_APP_FAILURE, true);
    uint64_t records = fixture.engine.frame_kpi.index;
    bool failed = fixture.engine.frame_kpi.failed;
    dispose_context(&fixture);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result); TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_EQUAL_UINT64(1, records); TEST_ASSERT_EQUAL_UINT(2, before_terminal_reads);
    TEST_ASSERT_EQUAL_UINT(before_terminal_reads + 1, ports.clock_reads);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_wall,7.0000,"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes,
        "\"\"frequency_hz\"\":\"\"1000\"\",\"\"begin\"\":\"\"0\"\",\"\"end\"\":\"\"7\"\""));
}

static void test_v3_actual_export_refuses_invalid_clock_and_wall_mismatch(void)
{
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        setUp(); ContextFixture fixture = {0}; attach_context(&fixture, true);
        fixture.engine.frame_kpi.context_version = 3;
        if (invalid == 0) fixture.engine.perf_freq = 0;
        fixture.engine.frame_counter_prev = invalid == 1 ? 10 : 0;
        test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
        engine_frame_release(&fixture.engine, JCE_APP_CONTINUE, true);
        fixture.engine.perf_hitch.interval_completed = true;
        unsigned before_export_reads = ports.clock_reads;
        bool exported = engine_export_completed_interval(&fixture.engine,
            invalid == 2 ? 99.0 : 7.0, fixture.engine.frame_counter_prev, 7, NULL);
        bool failed = fixture.engine.frame_kpi.failed;
        uint64_t records = fixture.engine.frame_kpi.index;
        JceAppResult result = jce_engine_kpi_capture_result(&fixture.engine.frame_kpi, JCE_APP_SUCCESS);
        dispose_context(&fixture);
        TEST_ASSERT_FALSE(exported); TEST_ASSERT_TRUE(failed);
        TEST_ASSERT_EQUAL_UINT64(0, records); TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result);
        TEST_ASSERT_EQUAL_UINT(2, before_export_reads);
        TEST_ASSERT_EQUAL_UINT(before_export_reads, ports.clock_reads);
        TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "frame_wall,"));
    }
}

static void test_legacy_completed_exports_retain_query_counts_and_omit_clock(void)
{
    for (unsigned version = 0; version < 3; ++version) {
        setUp(); ContextFixture fixture = {0}; attach_context(&fixture, version != 0);
        fixture.engine.frame_kpi.context_version = version;
        TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&fixture.engine.perf_hitch));
        test_context_render_begin(&fixture.engine); test_context_render_end(&fixture.engine);
        unsigned before_terminal_reads = ports.clock_reads;
        JceAppResult result = engine_frame_release(&fixture.engine, JCE_APP_SUCCESS, true);
        uint64_t records = fixture.engine.frame_kpi.index;
        dispose_context(&fixture);
        TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result); TEST_ASSERT_EQUAL_UINT64(1, records);
        TEST_ASSERT_EQUAL_UINT(2, before_terminal_reads);
        TEST_ASSERT_EQUAL_UINT(before_terminal_reads + 1, ports.clock_reads);
        TEST_ASSERT_EQUAL_UINT(1, ports.export_calls);
        TEST_ASSERT_EQUAL_UINT(version ? 2 : 0, ports.native_calls);
        TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "\"\"clock\"\""));
        if (!version) TEST_ASSERT_EQUAL_STRING("frame_index,phase,ms\n0,frame_wall,7.0000\n", fixture.sink.bytes);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_render_boundaries_seal_and_terminal_drain_same_sample);
    RUN_TEST(test_delayed_export_reuses_sealed_context_without_getter_resampling);
    RUN_TEST(test_missing_window_and_failed_observations_preserve_nullable_meaning);
    RUN_TEST(test_disabled_limited_and_failed_render_paths_do_not_fabricate_context);
    RUN_TEST(test_direct_close_reports_saved_nonzero_writer_ordinal_once_after_reset);
    RUN_TEST(test_public_v2_observations_are_draw_scoped_copied_and_transactional);
    RUN_TEST(test_v2_empty_unreported_and_next_interval_remain_distinct);
    RUN_TEST(test_off_v1_and_inactive_owner_never_admit_viewport_reports);
    RUN_TEST(test_v3_actual_completed_pending_and_terminal_intervals_use_borrowed_clock);
    RUN_TEST(test_v3_actual_terminal_failure_keeps_its_completed_interval);
    RUN_TEST(test_v3_actual_export_refuses_invalid_clock_and_wall_mismatch);
    RUN_TEST(test_legacy_completed_exports_retain_query_counts_and_omit_clock);
    return UNITY_END();
}
