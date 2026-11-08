/* Actual reset entry bodies and trace helper; typed backend/window ports only.
 * Real Core ring observes CPU calls. No window, bgfx device or GPU is created. */
#include <jce/renderer/jce_renderer.h>
#include <jce/os/platform/jce_window.h>
#include <jce/os/core/jce_log.h>
#include <bgfx/c99/bgfx.h>
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include "renderer/jce_renderer_launch_policy_internal.h"
#include "unity.h"
#include <string.h>

struct JceRenderer { bool is_fallback; uint32_t reset_flags; bool headless; JceRendererLaunchVsync launch_vsync; };
static unsigned resets, queries, native_queries, platform_sets, flag_queries;
static bool flags_known;
static uint32_t window_flags;
static bgfx_renderer_type_t renderer_type;
static uint32_t last_w, last_h, last_flags;
static bgfx_texture_format_t last_format;
static bgfx_stats_t stats;
static bool admitted;
static JceNativeWindow native;
static uint64_t reset_parent;
static void reset_port(uint32_t w, uint32_t h, uint32_t flags, bgfx_texture_format_t format)
{
    ++resets; last_w = w; last_h = h; last_flags = flags; last_format = format;
    reset_parent = jce_trace_task_current_id();
}
static const bgfx_stats_t *stats_port(void) { ++queries; return &stats; }
static bgfx_renderer_type_t renderer_type_port(void) { return renderer_type; }
static bool flags_port(const JceWindow *win, uint32_t *out)
{ (void)win; ++flag_queries; *out = window_flags; return flags_known; }
static bool admission_port(const JceRenderer *r) { return r && admitted; }
static void native_port(const JceWindow *win, JceNativeWindow *out)
{ (void)win; ++native_queries; *out = native; }
static void size_port(JceWindow *win, uint32_t *w, uint32_t *h)
{ (void)win; *w = 188; *h = 244; }
static void platform_port(const bgfx_platform_data_t *data)
{ ++platform_sets; TEST_ASSERT_EQUAL_PTR(native.nwh, data->nwh); }
#define bgfx_reset reset_port
#define bgfx_get_stats stats_port
#define bgfx_get_renderer_type renderer_type_port
#define jce_window_get_flags flags_port
#define bgfx_set_platform_data platform_port
#define jce_renderer_frame_admission_current_internal admission_port
#define jce_window_get_native native_port
#define jce_window_get_size size_port
#undef LOG_WARN
#define LOG_WARN(...) ((void)0)
#define LOG_TAG "reset_fixture"
#ifndef JCE_TEST_RESET_TRACE_HELPER
#define JCE_TEST_RESET_TRACE_HELPER "renderer/jce_renderer_reset_trace_internal.h"
#endif
#include JCE_TEST_RESET_TRACE_HELPER
bool fixture_vsync_get(const JceRenderer *);
#define jce_renderer_get_vsync fixture_vsync_get
#ifndef JCE_TEST_RENDERER_LAUNCH_POLICY
#define JCE_TEST_RENDERER_LAUNCH_POLICY "renderer/jce_renderer_launch_policy.inc.h"
#endif
#include JCE_TEST_RENDERER_LAUNCH_POLICY
/* Keep this fixture's entries separate from public linker symbols. */
#define jce_renderer_resize fixture_resize
#define jce_renderer_rebind_platform fixture_rebind
#define jce_renderer_set_vsync_for_size fixture_vsync_for_size
#define jce_renderer_set_vsync fixture_vsync
#define jce_renderer_set_msaa fixture_msaa
#include JCE_TEST_RESET_TRACE_ENTRIES
#undef jce_renderer_resize
#undef jce_renderer_rebind_platform
#undef jce_renderer_set_vsync_for_size
#undef jce_renderer_set_vsync
#undef jce_renderer_set_msaa
#undef jce_renderer_get_vsync
#undef LOG_WARN

static JceTraceEvent events[256];
static unsigned event_count;
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
    resets = queries = native_queries = platform_sets = flag_queries = 0;
    flags_known = true; window_flags = 0; renderer_type = BGFX_RENDERER_TYPE_NOOP;
    admitted = true; reset_parent = 0;
    memset(&stats, 0, sizeof stats); stats.width = 1643; stats.height = 987;
    memset(&native, 0, sizeof native); native.nwh = &native;
}
void tearDown(void) { jce_trace_set_enabled(false); jce_trace_task_restore_id(0); }
static void read_events(void)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    event_count = jce_trace_read(&cursor, events, 256);
    TEST_ASSERT_LESS_THAN_UINT(256, event_count);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats observed; jce_trace_get_stats(&observed);
    TEST_ASSERT_EQUAL_UINT(0, observed.active_work_items);
    TEST_ASSERT_EQUAL_UINT(0, observed.active_waits);
}
static void detail(const char *name, uint64_t id, uint64_t parent, uint64_t value)
{
    unsigned found = 0;
    for (unsigned i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && strcmp(events[i].name, name) == 0) {
            ++found;
            TEST_ASSERT_EQUAL_UINT64(id, events[i].id);
            TEST_ASSERT_EQUAL_UINT64(parent, events[i].parent_id);
            TEST_ASSERT_EQUAL_UINT64(value, events[i].value_u64);
        }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, found, name);
}
static void observed_reset(JceRendererResetReason reason, bool request_known, uint32_t rw, uint32_t rh,
    uint32_t cw, uint32_t ch, uint32_t flags)
{
    TEST_ASSERT_EQUAL_UINT(1, resets);
    TEST_ASSERT_EQUAL_UINT(cw, last_w); TEST_ASSERT_EQUAL_UINT(ch, last_h);
    TEST_ASSERT_EQUAL_UINT(flags, last_flags);
    TEST_ASSERT_EQUAL_UINT(BGFX_TEXTURE_FORMAT_COUNT, last_format);
    TEST_ASSERT_EQUAL_UINT64(901, jce_trace_task_current_id());
    read_events();
    unsigned submits = 0, begins = 0, ends = 0;
    uint64_t id = 0;
    for (unsigned i = 0; i < event_count; ++i) {
        if (strcmp(events[i].name, "renderer.reset_cpu") != 0) continue;
        if (events[i].type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            ++submits; id = events[i].id;
            TEST_ASSERT_EQUAL_UINT64(901, events[i].parent_id);
        } else if (events[i].type == JCE_TRACE_EVENT_TASK_BEGIN) {
            ++begins; TEST_ASSERT_EQUAL_UINT64(id, events[i].id);
        } else if (events[i].type == JCE_TRACE_EVENT_TASK_END) {
            ++ends; TEST_ASSERT_EQUAL_UINT64(id, events[i].id);
            TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SUCCEEDED, events[i].value_u32[0]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, submits); TEST_ASSERT_EQUAL_UINT(1, begins); TEST_ASSERT_EQUAL_UINT(1, ends);
    TEST_ASSERT_EQUAL_UINT64(id, reset_parent);
    detail("renderer.reset.reason", id, 901, reason);
    detail("renderer.reset.requested_width", id, 901, rw);
    detail("renderer.reset.requested_height", id, 901, rh);
    detail("renderer.reset.requested_extent_known", id, 901, request_known);
    detail("renderer.reset.call_width", id, 901, cw);
    detail("renderer.reset.call_height", id, 901, ch);
    detail("renderer.reset.call_flags", id, 901, flags);
    detail("renderer.reset.backend_result_known", id, 901, 0);
    detail("renderer.reset.gpu_completion_known", id, 901, 0);
    detail("renderer.reset.call_returned", id, 901, 1);
    TEST_ASSERT_EQUAL_UINT(13, event_count); /* S/B/E plus ten fixed details. */
}
static void enable(void) { jce_trace_set_enabled(true); jce_trace_task_restore_id(901); }
static void test_resize_preserves_request_and_exact_sanitized_call(void)
{
    JceRenderer renderer = {false, BGFX_RESET_VSYNC}; enable();
    fixture_resize(&renderer, 1643, 987);
    observed_reset(JCE_RENDERER_RESET_RESIZE, true, 1643, 987, 1644, 987, BGFX_RESET_VSYNC);
    TEST_ASSERT_EQUAL_UINT(0, queries);
}
static void test_zero_request_is_known_separately_from_forwarded_and_backend_extent(void)
{
    JceRenderer renderer = {false, 0}; enable(); fixture_resize(&renderer, 0, 0);
    observed_reset(JCE_RENDERER_RESET_RESIZE, true, 0, 0, 4, 1, 0);
}
static void test_vsync_retains_partial_request_and_existing_stats_fallback(void)
{
    JceRenderer renderer = {false, 0}; enable();
    fixture_vsync_for_size(&renderer, true, 123, 0);
    observed_reset(JCE_RENDERER_RESET_VSYNC, true, 123, 0, 1643, 987, BGFX_RESET_VSYNC);
    TEST_ASSERT_EQUAL_UINT(1, queries);
}
static void test_vsync_explicit_extent_needs_no_stats_query(void)
{
    JceRenderer renderer = {false, 0}; enable(); fixture_vsync_for_size(&renderer, true, 188, 244);
    observed_reset(JCE_RENDERER_RESET_VSYNC, true, 188, 244, 188, 244, BGFX_RESET_VSYNC);
    TEST_ASSERT_EQUAL_UINT(0, queries);
}
static void test_msaa_uses_existing_flags_and_stats_extent(void)
{
    JceRenderer renderer = {false, BGFX_RESET_VSYNC}; enable(); fixture_msaa(&renderer, 4);
    observed_reset(JCE_RENDERER_RESET_MSAA, false, 0, 0, 1643, 987, BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4);
}
static void test_rebind_records_existing_native_size_after_platform_call(void)
{
    JceRenderer renderer = {false, 0}; enable(); fixture_rebind(&renderer, (JceWindow *)&native);
    observed_reset(JCE_RENDERER_RESET_REBIND, true, 188, 244, 188, 244, 0);
    TEST_ASSERT_EQUAL_UINT(1, native_queries); TEST_ASSERT_EQUAL_UINT(1, platform_sets);
}
static void test_guards_and_unchanged_preferences_emit_no_reset(void)
{
    JceRenderer renderer = {false, 0}, fallback = {true, 0}; enable();
    fixture_resize(NULL, 8, 8); fixture_resize(&fallback, 8, 8);
    admitted = false; fixture_resize(&renderer, 8, 8); admitted = true;
    fixture_vsync(NULL, true); fixture_vsync(&fallback, true); fixture_vsync(&renderer, false);
    fixture_msaa(&renderer, 0); fixture_msaa(NULL, 4); fixture_msaa(&fallback, 4);
    fixture_rebind(NULL, (JceWindow *)&native); native.nwh = NULL;
    fixture_rebind(&renderer, (JceWindow *)&native);
    read_events(); TEST_ASSERT_EQUAL_UINT(0, event_count);
    TEST_ASSERT_EQUAL_UINT(0, resets); TEST_ASSERT_EQUAL_UINT(0, queries);
}
static void test_disabled_and_suppressed_tracing_keep_backend_call_and_parent(void)
{
    JceRenderer renderer = {false, 0}; jce_trace_task_restore_id(901);
    fixture_resize(&renderer, 9, 7);
    TEST_ASSERT_EQUAL_UINT(1, resets); TEST_ASSERT_EQUAL_UINT64(901, reset_parent);
    TEST_ASSERT_EQUAL_UINT64(901, jce_trace_task_current_id());
    read_events(); TEST_ASSERT_EQUAL_UINT(0, event_count);
    enable(); TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    fixture_resize(&renderer, 9, 7);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_EQUAL_UINT(2, resets); TEST_ASSERT_EQUAL_UINT64(901, reset_parent);
    TEST_ASSERT_EQUAL_UINT64(901, jce_trace_task_current_id());
    read_events(); TEST_ASSERT_EQUAL_UINT(0, event_count);
}
static void test_launch_off_blocks_later_requests_and_preserves_other_reset_bits(void)
{
    JceRenderer renderer = {false, BGFX_RESET_MSAA_X4}; enable();
    jce_renderer_launch_vsync_bind_internal(&renderer, true, false, true, true);
    fixture_vsync(&renderer, true);
    TEST_ASSERT_FALSE(fixture_vsync_get(&renderer));
    TEST_ASSERT_EQUAL_UINT(0, resets); TEST_ASSERT_EQUAL_UINT(0, queries);
    JceTraceStats before, after; jce_trace_get_stats(&before);
    fixture_vsync(&renderer, true); jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    fixture_msaa(&renderer, 8);
    TEST_ASSERT_EQUAL_UINT(1, resets); TEST_ASSERT_EQUAL_UINT(BGFX_RESET_MSAA_X8, last_flags);
    fixture_resize(&renderer, 800, 600);
    TEST_ASSERT_EQUAL_UINT(2, resets); TEST_ASSERT_EQUAL_UINT(BGFX_RESET_MSAA_X8, last_flags);
    jce_renderer_launch_vsync_observe_internal(&renderer, 3);
    read_events();
    unsigned effective = 0, unavailable_ack = 0;
    for (unsigned i = 0; i < event_count; ++i) {
        if (strcmp(events[i].name, "launch.vsync.owner_flag_effective") == 0) {
            TEST_ASSERT_EQUAL_UINT64(0, events[i].value_u64); ++effective;
            TEST_ASSERT_EQUAL_UINT64(events[i].id, events[i].parent_id);
        }
        if (strcmp(events[i].name, "launch.vsync.backend_ack_known") == 0) {
            TEST_ASSERT_EQUAL_UINT64(0, events[i].value_u64); ++unavailable_ack;
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, effective); TEST_ASSERT_EQUAL_UINT(2, unavailable_ack);
}
static void test_launch_on_and_absent_override_use_the_actual_flag_getter(void)
{
    JceRenderer on = {false, BGFX_RESET_VSYNC};
    jce_renderer_launch_vsync_bind_internal(&on, true, true, false, true);
    fixture_vsync(&on, false);
    TEST_ASSERT_TRUE(fixture_vsync_get(&on)); TEST_ASSERT_EQUAL_UINT(0, resets);
    JceRenderer ordinary = {false, 0};
    jce_renderer_launch_vsync_bind_internal(&ordinary, false, false, false, true);
    fixture_vsync(&ordinary, true);
    TEST_ASSERT_TRUE(fixture_vsync_get(&ordinary)); TEST_ASSERT_EQUAL_UINT(1, resets);
    // Observation must report the real getter even if a caller supplied an
    // inconsistent initial flag. Binding itself never invents a reset.
    JceRenderer inconsistent = {false, BGFX_RESET_VSYNC}; enable();
    jce_renderer_launch_vsync_bind_internal(&inconsistent, true, false, true, true);
    read_events();
    unsigned observed = 0;
    for (unsigned i = 0; i < event_count; ++i)
        if (strcmp(events[i].name, "launch.vsync.owner_flag_effective") == 0) {
            TEST_ASSERT_EQUAL_UINT64(1, events[i].value_u64); ++observed;
        }
    TEST_ASSERT_EQUAL_UINT(1, observed);
}
static void test_headless_and_fallback_presentation_remain_unavailable(void)
{
    JceRenderer headless = {false, 0, true}; enable();
    jce_renderer_launch_vsync_bind_internal(&headless, true, true, false, true);
    read_events();
    unsigned availability = 0;
    for (unsigned i = 0; i < event_count; ++i)
        if (strcmp(events[i].name, "launch.vsync.presentation_available") == 0) {
            TEST_ASSERT_EQUAL_UINT64(0, events[i].value_u64); ++availability;
        }
    TEST_ASSERT_EQUAL_UINT(1, availability);
    JceRenderer fallback = {true, 0};
    jce_renderer_launch_vsync_bind_internal(&fallback, true, true, true, true);
    TEST_ASSERT_FALSE(fallback.launch_vsync.available);
    fixture_vsync(&fallback, true); TEST_ASSERT_EQUAL_UINT(0, resets);
    // New objects carry no previous launch state.
    JceRenderer next = {false, 0}; fixture_vsync(&next, true);
    TEST_ASSERT_TRUE(fixture_vsync_get(&next)); TEST_ASSERT_EQUAL_UINT(1, resets);
}
/* Mirror the successful init caller's input, then run the actual presentation
 * owner. The fixture supplies observations; it never decides eligibility. */
static JceRenderer presentation_renderer(bool requested)
{
    JceRenderer r = {0}; r.launch_vsync.requested = requested;
    r.reset_flags = renderer_presentation_vsync_refresh(&r.launch_vsync, renderer_type,
        (JceWindow *)&native, BGFX_RESET_MSAA_X4 | (requested ? BGFX_RESET_VSYNC : 0));
    return r;
}
static void test_presentation_matrix_preserves_nonhidden_backends_and_other_bits(void)
{
    static const struct { bgfx_renderer_type_t type; bool known; uint32_t flags; bool hidden; } cases[] = {
        {BGFX_RENDERER_TYPE_DIRECT3D12, true, JCE_WINDOW_HIDDEN, true},
        {BGFX_RENDERER_TYPE_DIRECT3D12, true, 0, false},
        {BGFX_RENDERER_TYPE_DIRECT3D11, true, JCE_WINDOW_HIDDEN, false},
        {BGFX_RENDERER_TYPE_DIRECT3D12, false, JCE_WINDOW_HIDDEN, false},
        {BGFX_RENDERER_TYPE_DIRECT3D12, true, JCE_WINDOW_MINIMIZED, false}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        renderer_type = cases[i].type; flags_known = cases[i].known; window_flags = cases[i].flags;
        JceRenderer r = presentation_renderer(false);
        const bool masked = JCE_PLATFORM_WINDOWS && cases[i].hidden;
        const unsigned before = resets, dimensions = queries, observations = flag_queries;
        fixture_vsync_for_size(&r, true, 188, 244);
        TEST_ASSERT_EQUAL(!masked, fixture_vsync_get(&r));
        TEST_ASSERT_EQUAL(true, r.launch_vsync.requested);
        TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_MSAA_X4 | (masked ? 0 : BGFX_RESET_VSYNC), r.reset_flags);
        TEST_ASSERT_EQUAL_UINT(before + (masked ? 0 : 1), resets);
        TEST_ASSERT_EQUAL_UINT(dimensions, queries);
        TEST_ASSERT_EQUAL_UINT(observations, flag_queries); /* no per-request native query */
        fixture_rebind(&r, (JceWindow *)&native);
        TEST_ASSERT_EQUAL_UINT(before + (masked ? 0 : 1) + 1, resets);
        TEST_ASSERT_EQUAL(!masked, fixture_vsync_get(&r));
        TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_MSAA_X4 | (masked ? 0 : BGFX_RESET_VSYNC), last_flags);
        TEST_ASSERT_EQUAL_UINT(dimensions, queries);
    }
}
#if JCE_PLATFORM_WINDOWS
static void test_hidden_d3d12_masks_all_launch_choices_but_retains_latest_intent(void)
{
    renderer_type = BGFX_RENDERER_TYPE_DIRECT3D12; window_flags = JCE_WINDOW_HIDDEN;
    for (unsigned choice = 0; choice < 3; ++choice) { /* absent, forced off, forced on */
        JceRenderer r = presentation_renderer(choice != 1);
        jce_renderer_launch_vsync_bind_internal(&r, choice != 0, choice == 2, true, true);
        unsigned before = resets, dimensions = queries;
        fixture_vsync(&r, true);
        TEST_ASSERT_FALSE(fixture_vsync_get(&r)); TEST_ASSERT_TRUE(r.launch_vsync.requested);
        fixture_vsync(&r, false);
        TEST_ASSERT_FALSE(fixture_vsync_get(&r)); TEST_ASSERT_FALSE(r.launch_vsync.requested);
        TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_MSAA_X4, r.reset_flags);
        TEST_ASSERT_EQUAL_UINT(before, resets); TEST_ASSERT_EQUAL_UINT(dimensions, queries);
        window_flags = 0; fixture_rebind(&r, (JceWindow *)&native);
        TEST_ASSERT_EQUAL(choice == 2, fixture_vsync_get(&r));
        TEST_ASSERT_FALSE(r.launch_vsync.hidden_d3d12);
        TEST_ASSERT_EQUAL_UINT(before + 1, resets); /* original rebind: exactly one */
        TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_MSAA_X4 | (choice == 2 ? BGFX_RESET_VSYNC : 0), last_flags);
        window_flags = JCE_WINDOW_HIDDEN;
    }
}
static void test_rebind_rechecks_constraints_and_preserves_unbound_latest_request(void)
{
    renderer_type = BGFX_RENDERER_TYPE_DIRECT3D12; window_flags = JCE_WINDOW_HIDDEN;
    JceRenderer r = presentation_renderer(true);
    TEST_ASSERT_FALSE(r.launch_vsync.bound); TEST_ASSERT_FALSE(fixture_vsync_get(&r));
    fixture_vsync(&r, false); TEST_ASSERT_FALSE(r.launch_vsync.requested);
    window_flags = 0; fixture_rebind(&r, (JceWindow *)&native);
    TEST_ASSERT_EQUAL_UINT(1, resets); TEST_ASSERT_FALSE(fixture_vsync_get(&r));
    fixture_vsync_for_size(&r, true, 188, 244);
    TEST_ASSERT_EQUAL_UINT(2, resets); TEST_ASSERT_TRUE(r.launch_vsync.requested);
    TEST_ASSERT_TRUE(fixture_vsync_get(&r));
    window_flags = JCE_WINDOW_HIDDEN; fixture_rebind(&r, (JceWindow *)&native);
    TEST_ASSERT_EQUAL_UINT(3, resets); TEST_ASSERT_FALSE(fixture_vsync_get(&r));
    TEST_ASSERT_TRUE(r.launch_vsync.requested); TEST_ASSERT_TRUE(r.launch_vsync.hidden_d3d12);
    window_flags = 0; fixture_rebind(&r, (JceWindow *)&native);
    TEST_ASSERT_EQUAL_UINT(4, resets); TEST_ASSERT_TRUE(fixture_vsync_get(&r));
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4, last_flags);
    TEST_ASSERT_EQUAL_UINT(0, queries);
    TEST_ASSERT_EQUAL_UINT(3, native_queries); TEST_ASSERT_EQUAL_UINT(3, platform_sets);
    TEST_ASSERT_EQUAL_UINT(4, flag_queries); /* construction plus explicit rebinds only */
}
static void test_hidden_unchanged_setter_dedupes_observation_and_queries(void)
{
    renderer_type = BGFX_RENDERER_TYPE_DIRECT3D12; window_flags = JCE_WINDOW_HIDDEN;
    JceRenderer r = presentation_renderer(true); enable();
    jce_renderer_launch_vsync_bind_internal(&r, false, false, true, true);
    JceTraceStats before, after; jce_trace_get_stats(&before);
    fixture_vsync(&r, true); fixture_vsync(&r, true);
    jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    fixture_vsync(&r, false); jce_trace_get_stats(&before);
    fixture_vsync(&r, false); jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    TEST_ASSERT_EQUAL_UINT(0, resets); TEST_ASSERT_EQUAL_UINT(0, queries);
    TEST_ASSERT_EQUAL_UINT(1, flag_queries);
    read_events();
    unsigned known = 0, constrained = 0, gpu = 0;
    for (unsigned i = 0; i < event_count; ++i) {
        if (!strcmp(events[i].name, "launch.vsync.window_flags_valid")) {
            ++known; TEST_ASSERT_EQUAL_UINT64(1, events[i].value_u64);
        }
        if (!strcmp(events[i].name, "launch.vsync.hidden_d3d12")) {
            ++constrained; TEST_ASSERT_EQUAL_UINT64(1, events[i].value_u64);
        }
        if (!strcmp(events[i].name, "launch.vsync.gpu_completion_known")) {
            ++gpu; TEST_ASSERT_EQUAL_UINT64(0, events[i].value_u64);
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, known); TEST_ASSERT_EQUAL_UINT(2, constrained); TEST_ASSERT_EQUAL_UINT(2, gpu);
}
#endif

static void test_launch_flip_survives_every_reset_and_hidden_constraint(void)
{
    renderer_type = BGFX_RENDERER_TYPE_DIRECT3D12;
    window_flags = JCE_WINDOW_HIDDEN;
    const JceRendererLaunchDiagnostics request = {true, true, 1, 1, 0};
    bgfx_init_t init; memset(&init, 0, sizeof init); init.type = renderer_type;
    init.resolution.reset = BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4;
    renderer_launch_diagnostics_apply(&request, &init);
    JceRenderer r = {0}; r.launch_vsync.requested = true;
    r.reset_flags = renderer_presentation_vsync_refresh(&r.launch_vsync, renderer_type,
        (JceWindow *)&native, init.resolution.reset);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X4 |
        (JCE_PLATFORM_WINDOWS ? 0 : BGFX_RESET_VSYNC), r.reset_flags);
    fixture_vsync_for_size(&r, false, 800, 600);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X4, r.reset_flags);
    fixture_msaa(&r, 8);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X8, last_flags);
    fixture_resize(&r, 1000, 700);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X8, last_flags);
    fixture_vsync_for_size(&r, true, 800, 600);
    window_flags = 0; fixture_rebind(&r, (JceWindow *)&native);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X8 | BGFX_RESET_VSYNC, last_flags);
    window_flags = JCE_WINDOW_HIDDEN; fixture_rebind(&r, (JceWindow *)&native);
    TEST_ASSERT_EQUAL_UINT32(BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X8 |
        (JCE_PLATFORM_WINDOWS ? 0 : BGFX_RESET_VSYNC), last_flags);
    TEST_ASSERT_EQUAL_UINT(0, init.resolution.maxFrameLatency); /* D3D12 is not forwarded. */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_launch_flip_survives_every_reset_and_hidden_constraint);
    RUN_TEST(test_resize_preserves_request_and_exact_sanitized_call);
    RUN_TEST(test_zero_request_is_known_separately_from_forwarded_and_backend_extent);
    RUN_TEST(test_vsync_retains_partial_request_and_existing_stats_fallback);
    RUN_TEST(test_vsync_explicit_extent_needs_no_stats_query);
    RUN_TEST(test_msaa_uses_existing_flags_and_stats_extent);
    RUN_TEST(test_rebind_records_existing_native_size_after_platform_call);
    RUN_TEST(test_guards_and_unchanged_preferences_emit_no_reset);
    RUN_TEST(test_disabled_and_suppressed_tracing_keep_backend_call_and_parent);
    RUN_TEST(test_launch_off_blocks_later_requests_and_preserves_other_reset_bits);
    RUN_TEST(test_launch_on_and_absent_override_use_the_actual_flag_getter);
    RUN_TEST(test_headless_and_fallback_presentation_remain_unavailable);
    RUN_TEST(test_presentation_matrix_preserves_nonhidden_backends_and_other_bits);
#if JCE_PLATFORM_WINDOWS
    RUN_TEST(test_hidden_d3d12_masks_all_launch_choices_but_retains_latest_intent);
    RUN_TEST(test_rebind_rechecks_constraints_and_preserves_unbound_latest_request);
    RUN_TEST(test_hidden_unchanged_setter_dedupes_observation_and_queries);
#endif
    return UNITY_END();
}
