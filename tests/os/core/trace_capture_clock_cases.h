/* Private cases for the complete capture-owner fixture. These controls are
 * selected by the native clock registration, not by the generic/Web suite. */
static uint64_t capture_real_perf_ns(void)
{
    uint64_t value = 0;
    TEST_ASSERT_TRUE(jce_trace_clock_convert_internal(
        jce_time_perf_counter(), jce_time_perf_freq(), &value));
    return value;
}

static void test_payload_clock_stays_with_trace_across_real_sdl_restart(void)
{
    TEST_ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS));
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "writer-before-restart", 1);
    SDL_Quit();
    TEST_ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS));
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "writer-after-restart", 2);
    JceTraceCursor cursor;
    JceTraceEvent events[2];
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 2);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    uint64_t before = capture_real_perf_ns();
    bool drained = capture_drain(capture, true);
    uint64_t after = capture_real_perf_ns();
    uint64_t started = capture->slowest_append_started_ns;
    uint64_t finished = capture->slowest_append_finished_ns;
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    bool prefix = json && strstr(json, "writer-before-restart") &&
        strstr(json, "writer-after-restart");
    bool tagged = json && strstr(json, "\"clock_domain\":\"process-perf-ns\"");
    jce_fs_buffer_free(json);
    SDL_Quit();
    /* Release all native owners before assertions, including ordinary mutants. */
    TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_TRUE(result.capture_complete); TEST_ASSERT_TRUE(prefix);
    TEST_ASSERT_TRUE(tagged); TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(events[0].timestamp_ns, events[1].timestamp_ns);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(events[1].timestamp_ns, started);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(before, started);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(after, finished);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(started, finished);
}

static void test_payload_clock_failures_never_replay_or_publish(void)
{
    const struct { enum CaptureClockFault fault; unsigned read; bool threaded; } cases[] = {
        {CAPTURE_CLOCK_FREQUENCY, 1, false}, {CAPTURE_CLOCK_FREQUENCY, 2, true},
        {CAPTURE_CLOCK_OVERFLOW, 1, true}, {CAPTURE_CLOCK_OVERFLOW, 2, false},
        {CAPTURE_CLOCK_BACKWARDS, 2, false}, {CAPTURE_CLOCK_BACKWARDS, 2, true}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (i) next_output();
        jce_trace_reset();
        policy_clock = true;
        policy_thread_port = cases[i].threaded;
        policy_now = UINT64_C(1000000000);
        policy_write_ns = UINT64_C(1000);
        writer_clock_fault = CAPTURE_CLOCK_OK;
        writer_clock_reads = writer_clock_fault_at = 0;
        unsigned closed_before = partial_close_calls, freed_before = capture_heap_frees;
        unsigned staging_before = staging_frees, joined_before = policy_joins;
        JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
            cases[i].threaded ? JCE_TRACE_CAPTURE_THREADED : JCE_TRACE_CAPTURE_COOPERATIVE);
        TEST_ASSERT_NOT_NULL(capture);
        int writes_before = SDL_GetAtomicInt(&write_calls);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "capture-clock-failure", i);
        writer_clock_fault = cases[i].fault;
        writer_clock_fault_at = cases[i].read;
        bool drained = capture_drain(capture, true);
        observed_staging = capture->staging.data;
        uint64_t attempts = capture->append_calls;
        bool retained = capture->staging.size > 0 && capture->staged_events > 0;
        int writes_failed = SDL_GetAtomicInt(&write_calls);
        /* Recover the provider before poll/stop: only the failure latch prevents
         * the bytes already written from being replayed as a successful retry. */
        writer_clock_fault = CAPTURE_CLOCK_OK;
        controlled_capture_poll(capture);
        JceTraceCaptureResult result;
        bool stopped = controlled_capture_stop(capture, &result);
        int writes_stopped = SDL_GetAtomicInt(&write_calls);
        uint64_t size = 0;
        char *partial = jce_fs_host_read_all(partial_paths[partial_count - 1], &size);
        bool payload = partial && strstr(partial, "capture-clock-failure");
        bool duplicate = payload && strstr(strstr(partial, "capture-clock-failure") + 1,
                                           "capture-clock-failure");
        jce_fs_buffer_free(partial);
        TEST_ASSERT_FALSE(drained); TEST_ASSERT_TRUE(retained);
        TEST_ASSERT_FALSE(stopped); TEST_ASSERT_FALSE(result.write_ok);
        TEST_ASSERT_FALSE(result.published); TEST_ASSERT_FALSE(result.capture_complete);
        TEST_ASSERT_EQUAL_UINT64(0, result.captured_events);
        TEST_ASSERT_GREATER_THAN_UINT64(0, result.unexported_events);
        TEST_ASSERT_EQUAL_UINT64(cases[i].read == 2 ? 1 : 0, attempts);
        TEST_ASSERT_EQUAL_INT(cases[i].read == 2 ? 1 : 0, writes_failed - writes_before);
        TEST_ASSERT_EQUAL_INT(writes_failed, writes_stopped);
        TEST_ASSERT_EQUAL_INT(cases[i].read == 2, payload);
        TEST_ASSERT_FALSE(duplicate);
        TEST_ASSERT_EQUAL_UINT(closed_before + 1, partial_close_calls);
        TEST_ASSERT_EQUAL_UINT(freed_before + 4, capture_heap_frees);
        TEST_ASSERT_EQUAL_UINT(staging_before + 1, staging_frees);
        TEST_ASSERT_EQUAL_UINT(joined_before + (cases[i].threaded ? 1 : 0), policy_joins);
        TEST_ASSERT_NULL(partial_stream);
        TEST_ASSERT_TRUE(jce_trace_enabled());
        expect_pending();
    }
}
