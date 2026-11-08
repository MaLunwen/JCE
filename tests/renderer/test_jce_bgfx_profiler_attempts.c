/* Real callback owner plus complete actual backend-attempt function and exact
 * constructor/allocation prefixes. Counted bgfx/allocator ports enter the real
 * failed-init, API-refusal and allocation-cleanup branches without a device. */
#include "unity.h"
#include "os/core/jce_trace_scope_internal.h"
#include <string.h>
#include <jce/renderer/jce_render_readback.h>
bool jce_render_readback_format_supported(JceRenderFormat format)
{ (void)format; return false; }

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

uint32_t jce_rcb_host_frame_index(void) { return 0; }
void jce_renderer_caps_api_capture_trace(const char *line) { (void)line; }
#include "renderer/jce_renderer_bgfx_callback.c"
#include "renderer/jce_renderer_display_adapter_internal.h"
#include "renderer/jce_renderer_launch_policy_internal.h"

/* Only the allocation prefix needs the size of the opaque renderer. No
 * successful fake allocation is published or consumed as a real renderer. */
struct JceRenderer {
    bool is_fallback;
    uint32_t reset_flags;
    bool headless;
    JceRendererLaunchVsync launch_vsync;
};
static struct {
    bool init_ok, api_ok, web_ok, callbacks, retry_adapter;
    unsigned inits, shutdowns, allocations, releases, resets, flag_queries;
    unsigned display_queries, performance_queries;
    uint32_t window_flags, forwarded_flags;
    bool window_flags_valid;
    uint8_t ctor_latency;
    const char *flip_env, *latency_env;
    unsigned env_reads, diagnostic_logs;
    char diagnostic_log[512];
    bgfx_init_t forwarded_init;
    bgfx_caps_t caps;
    bool override_backend;
    bool missing_caps;
    bool software_device;
    JceRendererLaunchDiagnostics snapshot;
    bgfx_callback_interface_t *active;
} port;
static struct {
    int allocation_trace;
    uint32_t frame_index, offscreen_submission_index;
} s_device;

static void fixture_ctor(bgfx_init_t *init)
{
    memset(init, 0, sizeof(*init));
    init->resolution.maxFrameLatency = port.ctor_latency;
    init->fallback = true;
}
static const char *fixture_environment(const char *key)
{
    ++port.env_reads;
    if (!strcmp(key, "JCE_GFX_FLIP_AFTER_RENDER")) return port.flip_env;
    TEST_ASSERT_EQUAL_STRING("JCE_GFX_MAX_FRAME_LATENCY", key);
    return port.latency_env;
}
static void fixture_diagnostic_log(JceLogLevel level, const char *tag,
    const char *file, int line, const char *format, ...)
{
    (void)level; (void)tag; (void)file; (void)line;
    ++port.diagnostic_logs;
    va_list args; va_start(args, format);
    const int length = vsnprintf(port.diagnostic_log, sizeof port.diagnostic_log, format, args);
    va_end(args);
    TEST_ASSERT_TRUE(length >= 0 && (size_t)length < sizeof port.diagnostic_log);
}
static void fixture_observe_launch_snapshot(const JceRendererLaunchDiagnostics *request)
{ port.snapshot = *request; }

static bool fixture_init(const bgfx_init_t *init)
{
    ++port.inits;
    port.forwarded_flags = init->resolution.reset;
    port.forwarded_init = *init;
    if (!port.override_backend) port.caps.rendererType = init->type;
    TEST_ASSERT_NOT_NULL(init->callback);
    port.active = init->callback;
    if (port.callbacks) {
        port.active->vtbl->profiler_begin_literal(port.active, "fixture.init", 0, "fixture", 1);
        port.active->vtbl->profiler_end(port.active);
    }
    bool ok = port.init_ok;
    if (port.retry_adapter) {
        TEST_ASSERT_TRUE(port.inits <= 2);
        TEST_ASSERT_EQUAL_UINT(port.inits - 1, port.releases);
        TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_DIRECT3D11, init->type);
        const bool first = port.inits == 1;
        TEST_ASSERT_EQUAL_HEX16(first ? 0x10de : 0x8086, init->vendorId);
        TEST_ASSERT_EQUAL_HEX16(first ? 0x28e0 : 0x9a49, init->deviceId);
        ok = !first && ok;
    }
    if (!ok) port.active = NULL; /* The failed-init port is quiescent. */
    return ok;
}
static void fixture_shutdown(void)
{
    ++port.shutdowns;
    TEST_ASSERT_NOT_NULL(port.active);
    port.active->vtbl->profiler_begin_literal(port.active, "fixture.shutdown", 0, "fixture", 2);
    port.active->vtbl->profiler_end(port.active);
    port.active = NULL;
}
static void *fixture_allocate(void) { ++port.allocations; return NULL; }
static JceRendererBackend fixture_backend(bgfx_renderer_type_t type)
{ return type == BGFX_RENDERER_TYPE_OPENGLES ? JCE_BACKEND_OPENGLES : JCE_BACKEND_NOOP; }
static JceRendererApiInfo fixture_api(void) { JceRendererApiInfo api = {0}; return api; }
static void fixture_release(void) { ++port.releases; }
static void fixture_reset(void) { ++port.resets; }

static bool fixture_display_adapter(JceWindow *window, JceWindowAdapterObservation *out)
{
    if (!port.retry_adapter) return false;
    TEST_ASSERT_EQUAL_PTR(&port, window);
    ++port.display_queries;
    *out = (JceWindowAdapterObservation){.vendor_id = 0x8086, .device_id = 0x9a49,
        .adapter_index = 1, .reason = JCE_WINDOW_ADAPTER_MATCH};
    return true;
}

static bool fixture_performance_adapter(JceWindow *window, JceWindowAdapterObservation *out)
{
    if (!port.retry_adapter) return false;
    TEST_ASSERT_EQUAL_PTR(&port, window);
    ++port.performance_queries;
    *out = (JceWindowAdapterObservation){.vendor_id = 0x10de, .device_id = 0x28e0,
        .reason = JCE_WINDOW_ADAPTER_MATCH};
    return true;
}

static bool fixture_flags(const JceWindow *window, uint32_t *flags)
{
    (void)window; ++port.flag_queries; *flags = port.window_flags;
    return port.window_flags_valid;
}
static bgfx_renderer_type_t fixture_renderer_type(void) { return BGFX_RENDERER_TYPE_NOOP; }
static bool fixture_vsync_get(const JceRenderer *r)
{ return r && (r->reset_flags & BGFX_RESET_VSYNC) != 0; }
#define jce_window_get_flags fixture_flags
#define bgfx_get_renderer_type fixture_renderer_type
#define jce_renderer_get_vsync fixture_vsync_get
#define getenv fixture_environment
#define jce_log_write fixture_diagnostic_log
#include "renderer/jce_renderer_launch_policy.inc.h"
#undef jce_log_write
#undef getenv
#undef jce_renderer_get_vsync
#undef bgfx_get_renderer_type
#undef jce_window_get_flags

/* These ports replace external work only. The generated bodies contain the
 * unchanged production branch conditions, returns, shutdowns and finalizers. */
#define bgfx_init_ctor fixture_ctor
#define bgfx_init fixture_init
#define bgfx_shutdown fixture_shutdown
#define bgfx_render_frame(...) ((void)0)
#define from_bgfx_type fixture_backend
#define jce_renderer_caps_api_begin_attempt(...) ((void)0)
#define apply_adapter_preference(...) ((void)0)
#define jce_window_observe_display_adapter fixture_display_adapter
#define jce_window_observe_performance_adapter fixture_performance_adapter
#define apply_transient_limits(...) ((void)0)
#define gl_ladder_acquire(...) ((void)0)
#define gl_ladder_release fixture_release
#define jce_renderer_caps_api_admit_web_context() (port.web_ok)
#define jce_renderer_caps_api_accept_active_backend() (port.api_ok)
#define jce_renderer_get_api_info fixture_api
#define jce_renderer_backend_name(...) "fixture"
#define jce_graphics_api_tier_name(...) "fixture"
#define jce_renderer_caps_api_reset fixture_reset
#define jce_bgfx_trace_end(...) ((void)0)
#define jce_bgfx_allocator() NULL
#undef JCE_CALLOC
#define JCE_CALLOC(...) fixture_allocate()
#define jce_renderer_create_headless_impl fixture_headless_prefix
#define jce_renderer_create_impl fixture_window_prefix
#define jce_renderer_create_fallback_impl fixture_fallback_prefix
#if defined(JCE_TEST_BGFX_XP_ATTEMPT)
#define JCE_WINXP_LEGACY 1
#define JCE_BGFX_XP_D3D11 1
#define bgfx_get_renderer_type() (port.caps.rendererType)
#define bgfx_get_caps() (port.missing_caps ? NULL : &port.caps)
static const bgfx_internal_data_t fixture_internal_data = { .context = &port };
static bool fixture_device_is_software(void *device)
{
    TEST_ASSERT_EQUAL_PTR(&port, device);
    return port.software_device;
}
#define bgfx_get_internal_data() (&fixture_internal_data)
#define jce_window_legacy_d3d11_is_software fixture_device_is_software
#endif
#include JCE_BGFX_PROFILER_ATTEMPT_OWNER
#if defined(JCE_TEST_BGFX_XP_ATTEMPT)
#undef jce_window_legacy_d3d11_is_software
#undef bgfx_get_internal_data
#undef bgfx_get_caps
#undef bgfx_get_renderer_type
#undef JCE_BGFX_XP_D3D11
#undef JCE_WINXP_LEGACY
#endif
#undef jce_renderer_create_headless_impl
#undef jce_renderer_create_impl
#undef jce_renderer_create_fallback_impl
#undef bgfx_init_ctor
#undef bgfx_init
#undef bgfx_shutdown
#undef bgfx_render_frame
#undef from_bgfx_type
#undef jce_renderer_caps_api_begin_attempt
#undef apply_adapter_preference
#undef jce_window_observe_display_adapter
#undef jce_window_observe_performance_adapter
#undef apply_transient_limits
#undef gl_ladder_acquire
#undef gl_ladder_release
#undef jce_renderer_caps_api_admit_web_context
#undef jce_renderer_caps_api_accept_active_backend
#undef jce_renderer_get_api_info
#undef jce_renderer_backend_name
#undef jce_graphics_api_tier_name
#undef jce_renderer_caps_api_reset
#undef jce_bgfx_trace_end
#undef jce_bgfx_allocator
#undef JCE_CALLOC

static JceTraceEvent events[256];
static uint32_t event_count;
static void drain(void)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    event_count = jce_trace_read(&cursor, events, 256);
    TEST_ASSERT_TRUE(event_count < 256);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
}
static unsigned count(JceTraceEventType type, const char *name)
{
    unsigned result = 0;
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == type && !strcmp(events[i].name, name)) ++result;
    return result;
}
static void terminal(const char *name, unsigned observed)
{
    drain();
    TEST_ASSERT_EQUAL_UINT(1, count(JCE_TRACE_EVENT_TASK_END, name));
    TEST_ASSERT_EQUAL_UINT(0, count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.capability.after_backend_shutdown"));
    uint64_t id = 0;
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_END && !strcmp(events[i].name, name)) {
            id = events[i].id;
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, events[i].value_u32[0]);
        }
    unsigned counts = 0, metadata = 0, annotation_metadata = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_COUNTER || events[i].id != id) continue;
        TEST_ASSERT_EQUAL_UINT64(id, events[i].parent_id);
        if (!strcmp(events[i].name, "bgfx.cpu.begin_observed")) {
            ++counts; TEST_ASSERT_EQUAL_UINT64(observed, events[i].value_u64);
        }
        if (!strcmp(events[i].name, "bgfx.cpu.compiled")) {
            ++metadata;
            TEST_ASSERT_EQUAL_INT64(jce_rcb_cpu_profiler_witness().compiled, (int64_t)events[i].value_u64);
        }
        if (!strcmp(events[i].name, "bgfx.gpu.annotation.compiled")
            || !strcmp(events[i].name, "bgfx.gpu.annotation.request")
            || !strcmp(events[i].name, "bgfx.gpu.annotation.option"))
            ++annotation_metadata;
    }
    TEST_ASSERT_EQUAL_UINT(1, counts); TEST_ASSERT_EQUAL_UINT(1, metadata);
    TEST_ASSERT_EQUAL_UINT(3, annotation_metadata);
    TEST_ASSERT_EQUAL_UINT32(observed, jce_rcb_cpu_profiler_witness().begin_observed);
    /* A failed attempt cannot later acquire a successful shutdown witness. */
    jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_INIT_FAILED);
    jce_rcb_after_backend_shutdown();
    drain();
    TEST_ASSERT_EQUAL_UINT(1, count(JCE_TRACE_EVENT_TASK_END, name));
    TEST_ASSERT_EQUAL_UINT(0, count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.capability.after_backend_shutdown"));
}
static bool attempt(bgfx_renderer_type_t type)
{
    bgfx_platform_data_t pd = {0}; JceRendererDisplayAdapterState adapter = {0};
    return init_backend_attempt(type, &pd, 16, 16, 0, false, NULL, &adapter, NULL, NULL, NULL);
}
static JceRendererLaunchDiagnostics launch_request;
void setUp(void)
{
    jce_trace_set_enabled(false);
    jce_rcb_cpu_profiler_attempt_end(JCE_BGFX_CPU_ATTEMPT_SUPERSEDED_UNFINALIZED);
    memset(&port, 0, sizeof(port));
    memset(&launch_request, 0, sizeof launch_request);
    port.init_ok = port.api_ok = port.web_ok = port.callbacks = true;
    port.window_flags_valid = true;
    port.caps.supported = BGFX_CAPS_PRIMITIVE_ID;
    jce_trace_reset(); jce_trace_set_enabled(true);
}
void tearDown(void) { jce_trace_set_enabled(false); }
static void window_init_false_finalizes_without_shutdown(void)
{
    port.init_ok = false; TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_NOOP));
    TEST_ASSERT_EQUAL_UINT(1, port.inits); TEST_ASSERT_EQUAL_UINT(0, port.shutdowns);
    terminal("bgfx.cpu.capability.init_failed", 1);
}
static void window_api_refusal_finalizes_after_shutdown(void)
{
    port.api_ok = false; TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_NOOP));
    TEST_ASSERT_EQUAL_UINT(1, port.shutdowns);
    terminal("bgfx.cpu.capability.api_refused", 2);
}
static void zero_callback_init_false_is_still_failed(void)
{
    port.init_ok = port.callbacks = false;
    TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_NOOP));
    TEST_ASSERT_EQUAL_UINT(1, port.inits); TEST_ASSERT_EQUAL_UINT(0, port.shutdowns);
    terminal("bgfx.cpu.capability.init_failed", 0);
}
static void window_web_refusal_finalizes_after_shutdown(void)
{
    port.web_ok = false; TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_OPENGLES));
    TEST_ASSERT_EQUAL_UINT(1, port.shutdowns);
    terminal("bgfx.cpu.capability.api_refused", 2);
}
static void window_allocation_failure_finalizes_after_shutdown(void)
{
    TEST_ASSERT_TRUE(attempt(BGFX_RENDERER_TYPE_NOOP));
    TEST_ASSERT_NULL(fixture_window_allocation());
    TEST_ASSERT_EQUAL_UINT(1, port.allocations); TEST_ASSERT_EQUAL_UINT(1, port.shutdowns);
    terminal("bgfx.cpu.capability.allocation_failed", 2);
}
static void headless_init_false_finalizes_without_shutdown(void)
{
    port.init_ok = false; TEST_ASSERT_NULL(fixture_headless_prefix());
    TEST_ASSERT_EQUAL_UINT(1, port.inits); TEST_ASSERT_EQUAL_UINT(0, port.shutdowns);
    terminal("bgfx.cpu.capability.init_failed", 1);
}
static void headless_api_refusal_finalizes_after_shutdown(void)
{
    port.api_ok = false; TEST_ASSERT_NULL(fixture_headless_prefix());
    TEST_ASSERT_EQUAL_UINT(1, port.inits); TEST_ASSERT_EQUAL_UINT(1, port.shutdowns);
    TEST_ASSERT_EQUAL_UINT(0, port.allocations);
    terminal("bgfx.cpu.capability.api_refused", 2);
}
static void headless_allocation_failure_finalizes_after_shutdown(void)
{
    TEST_ASSERT_NULL(fixture_headless_prefix());
    TEST_ASSERT_EQUAL_UINT(1, port.allocations); TEST_ASSERT_EQUAL_UINT(1, port.shutdowns);
    terminal("bgfx.cpu.capability.allocation_failed", 2);
}
/* Successful publication uses the actual adjacent assignments extracted from
 * create_impl, never the allocation-failure port or a copied state initializer. */
static bool configured_attempt(bgfx_renderer_type_t type, uint32_t flags,
    JceRendererLaunchVsync *presentation, uint32_t *effective_flags)
{
    bgfx_platform_data_t pd = {0}; JceRendererDisplayAdapterState adapter = {0};
    return init_backend_attempt(type, &pd, 16, 16, flags, false,
        (JceWindow *)&port, &adapter, presentation, effective_flags, &launch_request);
}
static void initial_presentation_flags_and_cpu_publication_follow_observed_window(void)
{
    static const struct { bgfx_renderer_type_t type; bool known; uint32_t flags; bool masked; } cases[] = {
        {BGFX_RENDERER_TYPE_DIRECT3D12, true, JCE_WINDOW_HIDDEN, true},
        {BGFX_RENDERER_TYPE_DIRECT3D12, true, 0, false},
        {BGFX_RENDERER_TYPE_DIRECT3D11, true, JCE_WINDOW_HIDDEN, false},
        {BGFX_RENDERER_TYPE_DIRECT3D12, false, JCE_WINDOW_HIDDEN, false},
        {BGFX_RENDERER_TYPE_DIRECT3D12, true, JCE_WINDOW_MINIMIZED, false}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (unsigned choice = 0; choice < 2; ++choice) {
            const unsigned requested = 1 - choice;
            const unsigned observations = port.flag_queries;
            port.window_flags_valid = cases[i].known; port.window_flags = cases[i].flags;
            port.callbacks = false;
            JceRendererLaunchVsync presentation = {0};
            uint32_t flags = BGFX_RESET_MSAA_X4 | (requested ? BGFX_RESET_VSYNC : 0);
            TEST_ASSERT_TRUE(configured_attempt(cases[i].type, flags, &presentation, &flags));
            const bool masked = JCE_PLATFORM_WINDOWS && cases[i].masked;
            const uint32_t expected = BGFX_RESET_MSAA_X4 |
                ((requested && !masked) ? BGFX_RESET_VSYNC : 0);
            TEST_ASSERT_EQUAL_UINT32(expected, port.forwarded_flags);
            TEST_ASSERT_EQUAL_UINT32(expected, flags);
            TEST_ASSERT_EQUAL_UINT(observations + (JCE_PLATFORM_WINDOWS &&
                cases[i].type == BGFX_RENDERER_TYPE_DIRECT3D12 ? 1 : 0), port.flag_queries);
            TEST_ASSERT_EQUAL(requested != 0, presentation.requested);
            TEST_ASSERT_EQUAL(masked, presentation.hidden_d3d12);
            TEST_ASSERT_EQUAL(JCE_PLATFORM_WINDOWS && cases[i].type == BGFX_RENDERER_TYPE_DIRECT3D12 &&
                cases[i].known, presentation.window_flags_valid);
            JceRenderer published = {0};
            fixture_publish_presentation(&published, flags, presentation);
            TEST_ASSERT_EQUAL_UINT32(expected, published.reset_flags);
            TEST_ASSERT_EQUAL(requested != 0, published.launch_vsync.requested);
            TEST_ASSERT_EQUAL(masked, published.launch_vsync.hidden_d3d12);
            TEST_ASSERT_EQUAL(presentation.window_flags_valid, published.launch_vsync.window_flags_valid);
            TEST_ASSERT_EQUAL_UINT32(presentation.window_flags, published.launch_vsync.window_flags);
            fixture_shutdown(); jce_rcb_after_backend_shutdown();
        }
    }
}
static void refused_attempts_do_not_publish_effective_flags_or_policy(void)
{
    for (unsigned failure = 0; failure < 2; ++failure) {
        JceRendererLaunchVsync sentinel = {0}, observed;
        sentinel.requested = true; sentinel.window_flags = 0x12345678;
        memcpy(&observed, &sentinel, sizeof observed);
        uint32_t flags = UINT32_C(0x87654321);
        port.init_ok = failure != 0; port.api_ok = false;
        port.window_flags = JCE_WINDOW_HIDDEN;
        TEST_ASSERT_FALSE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D12,
            BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X8, &observed, &flags));
        TEST_ASSERT_EQUAL_MEMORY(&sentinel, &observed, sizeof sentinel);
        TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x87654321), flags);
    }
}

static JceRenderer *snapshot_window_request(void)
{
    const JceRendererConfig config = {0};
    return fixture_window_prefix((JceWindow *)&port, &config);
}
static void finish_attempt(void)
{ fixture_shutdown(); jce_rcb_after_backend_shutdown(); }
#if defined(JCE_TEST_BGFX_XP_ATTEMPT)
static void xp_d3d11_requires_an_observed_sm5_device(void)
{
    TEST_ASSERT_TRUE(attempt(BGFX_RENDERER_TYPE_DIRECT3D11));
    TEST_ASSERT_FALSE(port.forwarded_init.fallback);
    finish_attempt();
    port.caps.supported = 0;
    TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_DIRECT3D11));
    port.missing_caps = true;
    TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_DIRECT3D11));
    port.missing_caps = false;
    port.caps.supported = BGFX_CAPS_PRIMITIVE_ID;
    port.software_device = true;
    TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_DIRECT3D11));
    TEST_ASSERT_EQUAL_UINT(4, port.shutdowns);
    TEST_ASSERT_EQUAL_UINT(3, port.resets);
    TEST_ASSERT_TRUE(attempt(BGFX_RENDERER_TYPE_OPENGL));
    TEST_ASSERT_FALSE(port.forwarded_init.fallback);
    finish_attempt();
}

static void xp_unrequested_noop_does_not_stop_jce_fallback(void)
{
    port.override_backend = true;
    port.caps.rendererType = BGFX_RENDERER_TYPE_NOOP;
    TEST_ASSERT_FALSE(attempt(BGFX_RENDERER_TYPE_DIRECT3D11));
    TEST_ASSERT_EQUAL_UINT(1, port.shutdowns);
    TEST_ASSERT_EQUAL_UINT(1, port.resets);
    TEST_ASSERT_TRUE(attempt(BGFX_RENDERER_TYPE_NOOP));
    finish_attempt();
}
#endif
static void launch_defaults_preserve_all_forwarded_bytes_and_silence(void)
{
    TEST_ASSERT_NOT_NULL(snapshot_window_request());
    TEST_ASSERT_EQUAL_UINT(2, port.env_reads);
    launch_request = port.snapshot;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, BGFX_RESET_MSAA_X4, NULL, NULL));
    bgfx_init_t original = port.forwarded_init; finish_attempt();
    port.flip_env = port.latency_env = "";
    TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, BGFX_RESET_MSAA_X4, NULL, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&original, &port.forwarded_init, sizeof original);
    TEST_ASSERT_EQUAL_UINT(0, port.diagnostic_logs);
    TEST_ASSERT_EQUAL_UINT(4, port.env_reads); finish_attempt();
}
static void launch_strict_invalid_refuses_before_backend_or_allocation(void)
{
    static const char *invalid[] = {"00", "01", "4", "-1", "+1", " 1", "1 ", "1\n", "true", "\xff", "12345678901234567890"};
    for (unsigned field = 0; field < 2; ++field) {
        for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
            port.flip_env = field == 0 ? invalid[i] : NULL;
            port.latency_env = field == 1 ? invalid[i] : NULL;
            TEST_ASSERT_NULL(snapshot_window_request());
            TEST_ASSERT_EQUAL_UINT(0, port.inits); TEST_ASSERT_EQUAL_UINT(0, port.allocations);
            TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "phase=0"));
            TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "cpu_init=-1"));
        }
    }
    port.flip_env = "2"; port.latency_env = "3";
    TEST_ASSERT_NULL(snapshot_window_request());
    port.flip_env = NULL; port.latency_env = NULL;
    TEST_ASSERT_NOT_NULL(snapshot_window_request());
    TEST_ASSERT_FALSE(port.snapshot.flip_present); TEST_ASSERT_FALSE(port.snapshot.latency_present);
}
static void launch_explicit_matrix_forwards_only_supported_requests(void)
{
    static const bgfx_renderer_type_t types[] = {BGFX_RENDERER_TYPE_DIRECT3D11,
        BGFX_RENDERER_TYPE_VULKAN, BGFX_RENDERER_TYPE_DIRECT3D12,
        BGFX_RENDERER_TYPE_OPENGL, BGFX_RENDERER_TYPE_OPENGLES,
        BGFX_RENDERER_TYPE_METAL, BGFX_RENDERER_TYPE_NOOP};
    for (unsigned t = 0; t < sizeof types / sizeof types[0]; ++t) {
        for (unsigned flip = 0; flip < 2; ++flip) {
            for (unsigned latency = 0; latency < 4; ++latency) {
                char flip_text[2] = {(char)('0' + flip), 0};
                char latency_text[2] = {(char)('0' + latency), 0};
                port.flip_env = flip_text; port.latency_env = latency_text;
                TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
                const unsigned reads = port.env_reads;
                port.ctor_latency = 9;
                uint32_t effective = 0; JceRendererLaunchVsync presentation = {0};
                const uint32_t flags = BGFX_RESET_MSAA_X4 | BGFX_RESET_VSYNC | BGFX_RESET_FLIP_AFTER_RENDER;
                TEST_ASSERT_TRUE(configured_attempt(types[t], flags, &presentation, &effective));
                const bool flip_supported = types[t] != BGFX_RENDERER_TYPE_NOOP;
                const bool latency_supported = t < 2;
                const uint32_t expected = (flags & ~BGFX_RESET_FLIP_AFTER_RENDER) |
                    ((!flip_supported || flip) ? BGFX_RESET_FLIP_AFTER_RENDER : 0);
                TEST_ASSERT_EQUAL_UINT32(expected, port.forwarded_flags);
                TEST_ASSERT_EQUAL_UINT32(expected, effective);
                TEST_ASSERT_EQUAL_UINT(latency_supported ? latency : 9, port.forwarded_init.resolution.maxFrameLatency);
                TEST_ASSERT_EQUAL_UINT(reads, port.env_reads);
                TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "phase=2"));
                TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "backend_ack=unknown gpu_completion=unknown"));
                JceRenderer published = {0}; fixture_publish_presentation(&published, effective, presentation);
                TEST_ASSERT_EQUAL_UINT32(expected, published.reset_flags); finish_attempt();
            }
        }
    }
}
static void launch_snapshot_is_stable_across_failed_fallback_attempts(void)
{
    port.flip_env = "1"; port.latency_env = "1";
    TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
    port.flip_env = "0"; port.latency_env = "3";
    port.init_ok = false;
    uint32_t effective = UINT32_C(0xdeadbeef); JceRendererLaunchVsync presentation = {0};
    TEST_ASSERT_FALSE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, 0, &presentation, &effective));
    TEST_ASSERT_EQUAL_UINT(1, port.forwarded_init.resolution.maxFrameLatency);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER, port.forwarded_flags);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xdeadbeef), effective);
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "phase=1"));
    port.init_ok = true; port.api_ok = false;
    TEST_ASSERT_FALSE(configured_attempt(BGFX_RENDERER_TYPE_VULKAN, 0, &presentation, &effective));
    TEST_ASSERT_EQUAL_UINT(1, port.forwarded_init.resolution.maxFrameLatency);
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "phase=3"));
    port.api_ok = true;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D12, 0, &presentation, &effective));
    TEST_ASSERT_EQUAL_UINT(0, port.forwarded_init.resolution.maxFrameLatency);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER, effective);
    TEST_ASSERT_EQUAL_UINT(2, port.env_reads); finish_attempt();
    TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_VULKAN, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(3, port.forwarded_init.resolution.maxFrameLatency);
    TEST_ASSERT_EQUAL_UINT32(0, port.forwarded_flags); finish_attempt();
}


static void check_preferred_adapter_retry(bool fallback_succeeds)
{
    bgfx_platform_data_t pd = {0};
    JceRendererDisplayAdapterState adapter = {0};
    port.retry_adapter = true;
    port.init_ok = fallback_succeeds;
    TEST_ASSERT_EQUAL_INT(fallback_succeeds,
        init_backend_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, &pd, 16, 16, 0, false,
            (JceWindow *)&port, &adapter, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(2, port.inits);
    TEST_ASSERT_EQUAL_UINT(fallback_succeeds ? 1 : 2, port.releases);
    TEST_ASSERT_EQUAL_UINT(0, port.shutdowns);
    TEST_ASSERT_EQUAL_UINT(1, port.display_queries);
    TEST_ASSERT_EQUAL_UINT(1, port.performance_queries);
    TEST_ASSERT_TRUE(adapter.performance_blocked);
    TEST_ASSERT_FALSE(adapter.performance_applied);
    TEST_ASSERT_TRUE(adapter.applied);
    drain();
    TEST_ASSERT_EQUAL_UINT(fallback_succeeds ? 1 : 2,
        count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.capability.init_failed"));
    TEST_ASSERT_EQUAL_UINT(0, count(JCE_TRACE_EVENT_TASK_END, "bgfx.cpu.capability.superseded_unfinalized"));
    if (fallback_succeeds) finish_attempt();
}

static void preferred_adapter_init_failure_retries_same_backend_once(void)
{ check_preferred_adapter_retry(true); }

static void failed_display_retry_returns_without_loop(void)
{ check_preferred_adapter_retry(false); }

static void launch_controls_are_independent_and_headless_software_are_inapplicable(void)
{
    port.flip_env = "1"; port.latency_env = NULL;
    TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_VULKAN, BGFX_RESET_VSYNC, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_VSYNC | BGFX_RESET_FLIP_AFTER_RENDER, port.forwarded_flags);
    TEST_ASSERT_EQUAL_UINT(0, port.forwarded_init.resolution.maxFrameLatency); finish_attempt();
    port.flip_env = NULL; port.latency_env = "2";
    TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, BGFX_RESET_VSYNC, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_VSYNC, port.forwarded_flags);
    TEST_ASSERT_EQUAL_UINT(2, port.forwarded_init.resolution.maxFrameLatency); finish_attempt();
    port.flip_env = "1"; port.latency_env = "1"; port.ctor_latency = 9;
    /* This fixture reaches real headless allocation refusal after init. */
    TEST_ASSERT_NULL(fixture_headless_prefix());
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_NONE, port.forwarded_flags);
    TEST_ASSERT_EQUAL_UINT(9, port.forwarded_init.resolution.maxFrameLatency);
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "phase=4"));
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "flip_forwarded=0"));
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "latency_forwarded=0"));
    const unsigned inits = port.inits, allocations = port.allocations;
    TEST_ASSERT_NOT_NULL(fixture_fallback_prefix((JceWindow *)&port));
    TEST_ASSERT_EQUAL_UINT(inits, port.inits); TEST_ASSERT_EQUAL_UINT(allocations, port.allocations);
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "phase=5"));
    TEST_ASSERT_NOT_NULL(strstr(port.diagnostic_log, "cpu_init=-1"));
}
static void launch_observer_uses_owned_bounded_scope_and_respects_trace_off(void)
{
    port.callbacks = false; port.flip_env = "1"; port.latency_env = "1";
    TEST_ASSERT_NOT_NULL(snapshot_window_request()); launch_request = port.snapshot;
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, 0, NULL, NULL));
    drain();
    TEST_ASSERT_EQUAL_UINT(1, count(JCE_TRACE_EVENT_TASK_BEGIN, "renderer.launch_gfx"));
    TEST_ASSERT_EQUAL_UINT(1, count(JCE_TRACE_EVENT_TASK_END, "renderer.launch_gfx"));
    static const struct { const char *name; uint64_t value; } details[] = {
        {"launch.gfx.flip_forwarded", 1}, {"launch.gfx.latency_forwarded", 1},
        {"launch.gfx.cpu_init", 1}, {"launch.gfx.backend_ack_known", 0},
        {"launch.gfx.gpu_completion_known", 0}};
    uint64_t id = 0;
    for (unsigned i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_TASK_BEGIN && !strcmp(events[i].name, "renderer.launch_gfx")) id = events[i].id;
    for (unsigned d = 0; d < sizeof details / sizeof details[0]; ++d) {
        unsigned found = 0;
        for (unsigned i = 0; i < event_count; ++i) {
            if (events[i].type != JCE_TRACE_EVENT_COUNTER || strcmp(events[i].name, details[d].name)) continue;
            ++found; TEST_ASSERT_EQUAL_UINT64(id, events[i].id);
            TEST_ASSERT_EQUAL_UINT64(id, events[i].parent_id);
            TEST_ASSERT_EQUAL_UINT64(details[d].value, events[i].value_u64);
        }
        TEST_ASSERT_EQUAL_UINT(1, found);
    }
    finish_attempt(); jce_trace_set_enabled(false); jce_trace_reset();
    TEST_ASSERT_TRUE(configured_attempt(BGFX_RENDERER_TYPE_DIRECT3D11, 0, NULL, NULL));
    drain(); TEST_ASSERT_EQUAL_UINT(0, event_count); finish_attempt();
}

int main(void)
{
    UNITY_BEGIN();
#if defined(JCE_TEST_BGFX_XP_ATTEMPT)
    RUN_TEST(xp_d3d11_requires_an_observed_sm5_device);
    RUN_TEST(xp_unrequested_noop_does_not_stop_jce_fallback);
#endif
    RUN_TEST(window_init_false_finalizes_without_shutdown);
    RUN_TEST(window_api_refusal_finalizes_after_shutdown);
    RUN_TEST(zero_callback_init_false_is_still_failed);
    RUN_TEST(window_web_refusal_finalizes_after_shutdown);
    RUN_TEST(window_allocation_failure_finalizes_after_shutdown);
    RUN_TEST(headless_init_false_finalizes_without_shutdown);
    RUN_TEST(headless_api_refusal_finalizes_after_shutdown);
    RUN_TEST(headless_allocation_failure_finalizes_after_shutdown);
    RUN_TEST(initial_presentation_flags_and_cpu_publication_follow_observed_window);
    RUN_TEST(refused_attempts_do_not_publish_effective_flags_or_policy);
    RUN_TEST(launch_defaults_preserve_all_forwarded_bytes_and_silence);
    RUN_TEST(launch_strict_invalid_refuses_before_backend_or_allocation);
    RUN_TEST(launch_explicit_matrix_forwards_only_supported_requests);
    RUN_TEST(launch_snapshot_is_stable_across_failed_fallback_attempts);
    RUN_TEST(preferred_adapter_init_failure_retries_same_backend_once);
    RUN_TEST(failed_display_retry_returns_without_loop);
    RUN_TEST(launch_controls_are_independent_and_headless_software_are_inapplicable);
    RUN_TEST(launch_observer_uses_owned_bounded_scope_and_respects_trace_off);
    return UNITY_END();
}
