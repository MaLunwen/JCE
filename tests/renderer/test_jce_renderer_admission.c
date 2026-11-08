/* Exact fixed admission owner plus actual Noop public lifecycle.
 * Atomic admission is not permission to issue bgfx APIs from arbitrary threads.
 * Noop validates CPU/backend ownership only; no GPU/pixel claim is made. */
#include "unity.h"
#include "renderer/jce_renderer_admission_internal.h"
#include "renderer/jce_renderer_bgfx_callback.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_texture.h>
#include <string.h>

static JceRendererAdmission admission;
static JceRenderer *renderer;
static JceThread *workers[2];
static JceAtomicI32 accepted;
static bool foreign_created;

void setUp(void)
{
    memset(&admission, 0, sizeof(admission));
    memset(&accepted, 0, sizeof(accepted));
    memset(workers, 0, sizeof(workers));
    renderer = NULL; foreign_created = false;
    jce_thread_mark_main();
    jce_trace_set_enabled(false);
    jce_trace_reset(); /* Previous case has joined/destroyed all producers. */
}

void tearDown(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (workers[i]) { jce_thread_join(workers[i]); workers[i] = NULL; }
    if (renderer) { jce_renderer_destroy(renderer); renderer = NULL; }
}

static void test_creating_and_active_are_held_until_their_actual_boundary(void)
{
    TEST_ASSERT_TRUE(jce_renderer_admission_try_begin(&admission));
    TEST_ASSERT_FALSE(jce_renderer_admission_try_begin(&admission));
    /* A joined release cannot accidentally release an unfinished creation. */
    jce_renderer_admission_release_joined(&admission);
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_ADMISSION_CREATING,
                          jce_atomic_i32_load(&admission.phase));
    jce_renderer_admission_finish(&admission, true);
    TEST_ASSERT_FALSE(jce_renderer_admission_try_begin(&admission));
    /* A stale failed-finish call cannot release an active backend. */
    jce_renderer_admission_finish(&admission, false);
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_ADMISSION_ACTIVE,
                          jce_atomic_i32_load(&admission.phase));
    jce_renderer_admission_release_joined(&admission);
    TEST_ASSERT_TRUE(jce_renderer_admission_try_begin(&admission));
}

static void test_failed_creation_and_invalid_finish_preserve_phase(void)
{
    jce_renderer_admission_finish(&admission, true);
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_ADMISSION_IDLE,
                          jce_atomic_i32_load(&admission.phase));
    TEST_ASSERT_TRUE(jce_renderer_admission_try_begin(&admission));
    jce_renderer_admission_finish(&admission, false);
    TEST_ASSERT_TRUE(jce_renderer_admission_try_begin(&admission));
    TEST_ASSERT_FALSE(jce_renderer_admission_try_begin(NULL));
}

static void attempt_admission(void *arg)
{
    (void)arg;
    if (jce_renderer_admission_try_begin(&admission))
        jce_atomic_i32_add(&accepted, 1);
}

static void test_overlapping_atomic_admission_has_one_winner_without_bgfx_calls(void)
{
    workers[0] = jce_thread_create(attempt_admission, NULL, "admission-control-a");
    TEST_ASSERT_NOT_NULL(workers[0]);
    workers[1] = jce_thread_create(attempt_admission, NULL, "admission-control-b");
    TEST_ASSERT_NOT_NULL(workers[1]);
    for (unsigned i = 0; i < 2; ++i) { jce_thread_join(workers[i]); workers[i] = NULL; }
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&accepted));
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_ADMISSION_CREATING,
                          jce_atomic_i32_load(&admission.phase));
}

static void test_actual_noop_duplicate_rejection_preserves_backend_and_recreates(void)
{
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_TRUE(jce_renderer_is_headless(renderer));
    JceSubmittedPayloadStats before, after, recreated;
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &before));
    const JceRendererApiInfo api_before = jce_renderer_get_api_info();
    TEST_ASSERT_NULL(jce_renderer_create_headless());
    JceRendererConfig config = {0};
    TEST_ASSERT_NULL(jce_renderer_create(NULL, &config));
    const JceRendererApiInfo api_after = jce_renderer_get_api_info();
    TEST_ASSERT_EQUAL_INT(api_before.backend, api_after.backend);
    TEST_ASSERT_EQUAL_INT(api_before.runtime_version_verified,
                          api_after.runtime_version_verified);
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &after));
    TEST_ASSERT_EQUAL_UINT64(before.context_generation, after.context_generation);
    /* Existing device must still admit actual resource work after rejection. */
    const unsigned char pixel[] = {1, 2, 3, 255};
    JceTexture texture = jce_texture_from_rgba(pixel, 1, 1);
    TEST_ASSERT_TRUE(jce_texture_valid(texture));
    jce_texture_destroy(texture);
    jce_renderer_destroy(renderer); renderer = NULL;
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &recreated));
    TEST_ASSERT_TRUE(recreated.context_generation > before.context_generation);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events); /* Refusal is quiet with trace off. */
}

static void test_actual_failed_window_creation_releases_before_noop_creation(void)
{
    JceRendererConfig config = {0};
    TEST_ASSERT_NULL(jce_renderer_create(NULL, &config));
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_TRUE(jce_renderer_is_headless(renderer));
}

static void foreign_attempt(void *arg)
{
    (void)arg;
    JceRenderer *r = jce_renderer_create_headless();
    foreign_created = r != NULL;
    /* If policy regresses, retire on that same API thread before reporting. */
    if (r) jce_renderer_destroy(r);
}

static void test_registered_main_rejects_foreign_create_before_backend_initialization(void)
{
    workers[0] = jce_thread_create(foreign_attempt, NULL, "foreign-create-control");
    TEST_ASSERT_NOT_NULL(workers[0]);
    jce_thread_join(workers[0]); workers[0] = NULL;
    TEST_ASSERT_FALSE(foreign_created);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
}

static void test_actual_noop_refusal_reasons_bind_their_create_scopes(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset(); /* No backend exists yet; never reset a live producer. */
    JceTraceCursor cursor;
    JceTraceEvent batch[64], captured[128];
    unsigned captured_count = 0;
    uint32_t count;
    jce_trace_cursor_init(&cursor);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    /* Keep the same cursor/capture. Drain startup without resetting live bgfx. */
    while (jce_trace_read(&cursor, batch, 64) != 0) {}
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_NULL(jce_renderer_create_headless());
    workers[0] = jce_thread_create(foreign_attempt, NULL, "refusal-reason-control");
    TEST_ASSERT_NOT_NULL(workers[0]);
    jce_thread_join(workers[0]); workers[0] = NULL;
    TEST_ASSERT_FALSE(foreign_created);
    while ((count = jce_trace_read(&cursor, batch, 64)) != 0) {
        TEST_ASSERT_TRUE(captured_count + count <= sizeof(captured) / sizeof(captured[0]));
        memcpy(captured + captured_count, batch, count * sizeof(*batch));
        captured_count += count;
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    unsigned busy = 0, foreign = 0;
    for (unsigned i = 0; i < captured_count; ++i) {
        const JceTraceEvent *event = &captured[i];
        if (strcmp(event->name, "renderer.create.rejection_reason") != 0) continue;
        TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_COUNTER, event->type);
        TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, event->category);
        TEST_ASSERT_TRUE(event->id != 0);
        TEST_ASSERT_EQUAL_UINT64(event->id, event->parent_id);
        if (event->value_u64 == JCE_RENDERER_ADMISSION_REFUSED_BUSY) ++busy;
        else if (event->value_u64 == JCE_RENDERER_ADMISSION_REFUSED_FOREIGN) ++foreign;
        else TEST_FAIL_MESSAGE("Unknown renderer create refusal");
        unsigned starts = 0, failed_ends = 0;
        for (unsigned j = 0; j < captured_count; ++j) {
            const JceTraceEvent *scope = &captured[j];
            if (scope->id != event->id ||
                strcmp(scope->name, "renderer.create_headless") != 0) continue;
            if (scope->type == JCE_TRACE_EVENT_TASK_BEGIN) ++starts;
            if (scope->type == JCE_TRACE_EVENT_TASK_END &&
                scope->value_u32[0] == JCE_TRACE_TASK_FAILED) ++failed_ends;
        }
        TEST_ASSERT_EQUAL_UINT32(1, starts);
        TEST_ASSERT_EQUAL_UINT32(1, failed_ends);
    }
    TEST_ASSERT_EQUAL_UINT32(1, busy);
    TEST_ASSERT_EQUAL_UINT32(1, foreign);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
}

static void test_actual_noop_reports_compiled_bridge_and_observed_callback_facts(void)
{
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    const JceBgfxCpuProfilerWitness initialized = jce_rcb_cpu_profiler_witness();
    for (unsigned i = 0; i < 3; ++i) jce_renderer_submit_frame(renderer);
    const JceBgfxCpuProfilerWitness framed = jce_rcb_cpu_profiler_witness();
    jce_renderer_destroy(renderer); renderer = NULL;
    const JceBgfxCpuProfilerWitness joined = jce_rcb_cpu_profiler_witness();
    TEST_ASSERT_TRUE(initialized.compiled >= -1 && initialized.compiled <= 1);
    TEST_ASSERT_EQUAL_INT(initialized.compiled, framed.compiled);
    TEST_ASSERT_EQUAL_INT(initialized.compiled, joined.compiled);
    TEST_ASSERT_EQUAL_INT(initialized.bridge_supported, joined.bridge_supported);
    TEST_ASSERT_TRUE(framed.begin_observed >= initialized.begin_observed);
    TEST_ASSERT_TRUE(joined.begin_observed >= framed.begin_observed);
    /* Unknown package metadata does not become "off" from a zero sample.
     * An explicitly enabled package must exercise real Noop callbacks. */
    if (joined.compiled == 1) TEST_ASSERT_TRUE(joined.begin_observed > 0);
    if (joined.compiled == 0) TEST_ASSERT_EQUAL_UINT32(0, joined.begin_observed);
    TEST_ASSERT_FALSE(joined.begin_count_saturated);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_creating_and_active_are_held_until_their_actual_boundary);
    RUN_TEST(test_failed_creation_and_invalid_finish_preserve_phase);
    RUN_TEST(test_overlapping_atomic_admission_has_one_winner_without_bgfx_calls);
    RUN_TEST(test_actual_noop_duplicate_rejection_preserves_backend_and_recreates);
    RUN_TEST(test_actual_failed_window_creation_releases_before_noop_creation);
    RUN_TEST(test_registered_main_rejects_foreign_create_before_backend_initialization);
    RUN_TEST(test_actual_noop_refusal_reasons_bind_their_create_scopes);
    RUN_TEST(test_actual_noop_reports_compiled_bridge_and_observed_callback_facts);
    return UNITY_END();
}
