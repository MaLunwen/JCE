/* test_jce_scene_async_progress.c
 *
 * The parse-phase progress ramp must be a function of ELAPSED TIME, not of how
 * often the progress getter happens to be called.  The original form added a
 * fixed increment inside jce_scene_async_progress(), so two panels polling the
 * same handle advanced the bar twice as fast as one, and a caller that stopped
 * polling froze it -- the bar reported polling frequency rather than load
 * progress.  These assertions pin the timing contract itself, which is why the
 * ramp is factored out: it can be exercised without a live worker thread.
 */

#include <jce/middleware/scene/jce_scene_async.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_thread.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void)
{
    jce_scene_async_shutdown();
    (void)jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE);
}

static void test_ramp_depends_on_elapsed_time_not_call_count(void)
{
    /* Polling many times at the same instant must not advance anything. */
    float polled = 0.0f;
    int i;
    for (i = 0; i < 50; ++i)
        polled = jce_async_parse_ramp(polled, 500u);

    const float once = jce_async_parse_ramp(0.0f, 500u);
    TEST_ASSERT_EQUAL_FLOAT(once, polled);
}

static void test_ramp_reaches_the_ceiling_on_schedule(void)
{
    const uint64_t full_ms = (uint64_t)(JCE_ASYNC_PARSE_RAMP_SECONDS * 1000.0f);

    /* Half way through the window, half the ceiling. */
    const float half = jce_async_parse_ramp(0.0f, full_ms / 2u);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, JCE_ASYNC_PARSE_CEILING * 0.5f, half);

    /* At the window, exactly the ceiling -- and never past it afterwards. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, JCE_ASYNC_PARSE_CEILING,
                             jce_async_parse_ramp(0.0f, full_ms));
    TEST_ASSERT_EQUAL_FLOAT(JCE_ASYNC_PARSE_CEILING,
                            jce_async_parse_ramp(0.0f, full_ms * 100u));
}

static void test_ramp_never_moves_backwards(void)
{
    /* A caller that already reported more progress (real load milestones push
     * the bar past the ramp) must not be dragged back by the time ramp. */
    const float ahead = 0.30f;
    TEST_ASSERT_EQUAL_FLOAT(ahead, jce_async_parse_ramp(ahead, 0u));
    TEST_ASSERT_EQUAL_FLOAT(ahead, jce_async_parse_ramp(ahead, 100u));
    TEST_ASSERT_TRUE(jce_async_parse_ramp(ahead, 5000u) >= ahead);
}

typedef struct CallbackCapture {
    int           count;
    JceLoadStatus status;
    JceLoadResult result;
} CallbackCapture;

static void capture_load(JceLoadHandle handle, JceLoadStatus status,
                         const JceLoadResult *result, void *user)
{
    CallbackCapture *capture = (CallbackCapture *)user;
    (void)handle;
    capture->count++;
    capture->status = status;
    capture->result = *result;
}

static void test_missing_scene_fails_once_on_owner_dispatch(void)
{
    CallbackCapture capture = { 0 };
    JceLoadHandle handle;
    int guard;

    TEST_ASSERT_TRUE(jce_scene_async_init(NULL, NULL));
    handle = jce_scene_instantiate_async(
        "__jce_missing_async_scene__.scene.json",
        JCE_LOAD_MODE_ADDITIVE, capture_load, &capture);
    TEST_ASSERT_NOT_EQUAL(JCE_LOAD_HANDLE_INVALID, handle);

    for (guard = 0; guard < 2000 && capture.count == 0; ++guard) {
        jce_scene_async_dispatch_main();
        jce_async_default_pump(NULL);
        jce_thread_sleep_ms(1);
    }

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    TEST_ASSERT_EQUAL_INT(JCE_LOAD_FAILED, capture.status);
    TEST_ASSERT_EQUAL_UINT32(1, capture.result.error_code);
    jce_scene_async_dispatch_main();
    TEST_ASSERT_EQUAL_INT(1, capture.count);
}

static void test_pending_cancel_survives_until_callback_dispatch(void)
{
    CallbackCapture capture = { 0 };
    JceLoadHandle handle;

    TEST_ASSERT_TRUE(jce_scene_async_init(NULL, NULL));
    handle = jce_scene_instantiate_async(
        "__never_started__.scene.json", JCE_LOAD_MODE_ADDITIVE,
        capture_load, &capture);
    TEST_ASSERT_NOT_EQUAL(JCE_LOAD_HANDLE_INVALID, handle);

    jce_scene_async_cancel(handle);
    TEST_ASSERT_EQUAL_INT(JCE_LOAD_CANCELLED,
                          jce_scene_async_status(handle));
    jce_scene_async_dispatch_main();

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    TEST_ASSERT_EQUAL_INT(JCE_LOAD_CANCELLED, capture.status);
    jce_scene_async_dispatch_main();
    TEST_ASSERT_EQUAL_INT(1, capture.count);
}

int main(void)
{
    jce_thread_mark_main();
    UNITY_BEGIN();
    RUN_TEST(test_ramp_depends_on_elapsed_time_not_call_count);
    RUN_TEST(test_ramp_reaches_the_ceiling_on_schedule);
    RUN_TEST(test_ramp_never_moves_backwards);
    RUN_TEST(test_missing_scene_fails_once_on_owner_dispatch);
    RUN_TEST(test_pending_cancel_survives_until_callback_dispatch);
    return UNITY_END();
}
