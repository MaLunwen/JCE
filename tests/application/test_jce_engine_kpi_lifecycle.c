/* Public real headless engine callback lifetime; no private engine copy. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_renderer.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_events.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JCE_ENGINE_KPI_TEST_OUTPUT
#error "JCE_ENGINE_KPI_TEST_OUTPUT must name an ignored test-output directory"
#endif

const unsigned char assets_pak_data[1] = {0};
const size_t assets_pak_data_size = 0;

typedef struct {
    JceEngine *engine;
    JceRenderer *renderer;
    JceAsyncExecutor *executor;
    unsigned inits, updates, draws, events, exits, completions;
    SDL_AtomicInt work_runs;
    bool actual_headless, request_exit_reentry, exit_reentry_entered;
    bool completion_preserved_callbacks, exit_preserved_callbacks;
    bool destroy_on_update, destroy_on_event, destroy_on_completion;
    bool destroy_request_entered, destroy_retained_owner, destroy_retained_services;
    bool try_nested_iterate;
    bool observe_nested_iterate;
    unsigned nested_observations;
    JceAppResult observed_nested_iterate;
    JceAppResult destroy_nested_iterate;
    JceAppResult completion_event, completion_iterate;
    JceAppResult exit_event, exit_iterate;
} LifecycleProbe;
static LifecycleProbe probe;

void setUp(void) { memset(&probe, 0, sizeof probe); }
void tearDown(void) {}

static SDL_Event quit_event(void)
{
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_QUIT;
    return event;
}

static void request_callback_destroy(LifecycleProbe *state)
{
    state->destroy_request_entered = true;
    unsigned updates = state->updates, events = state->events;
    jce_engine_destroy(state->engine);
    state->destroy_retained_owner = state->exits == 0;
    /* Observe premature exit before touching a potentially released service. */
    TEST_ASSERT_TRUE_MESSAGE(state->destroy_retained_owner,
        "Callback destroy must retain the engine until the caller returns");
    state->destroy_retained_services = jce_renderer_is_headless(state->renderer);
    if (state->try_nested_iterate)
        state->destroy_nested_iterate = jce_engine_iterate(state->engine);
    TEST_ASSERT_EQUAL_UINT(updates, state->updates);
    TEST_ASSERT_EQUAL_UINT(events, state->events);
    TEST_ASSERT_EQUAL_UINT(0, state->exits);
}

static bool app_init(const JceServices *services, void *user)
{
    LifecycleProbe *state = user;
    ++state->inits;
    state->renderer = services ? services->renderer : NULL;
    state->actual_headless = services && !services->window && !services->input &&
        !services->audio && jce_renderer_is_headless(services->renderer);
    return state->actual_headless;
}
static void app_update(float dt, void *user)
{
    LifecycleProbe *state = user;
    (void)dt;
    ++state->updates;
    if (state->observe_nested_iterate) {
        ++state->nested_observations;
        state->observed_nested_iterate = jce_engine_iterate(state->engine);
    }
    if (state->destroy_on_update && !state->destroy_request_entered)
        request_callback_destroy(state);
}
static void app_draw(const JceServices *services, void *user)
{
    LifecycleProbe *state = user;
    (void)services;
    ++state->draws;
}
static void app_event(const JceEvent *event, void *user)
{
    LifecycleProbe *state = user;
    (void)event;
    ++state->events;
    if (state->destroy_on_event && !state->destroy_request_entered)
        request_callback_destroy(state);
}
static void app_exit(void *user)
{
    LifecycleProbe *state = user;
    ++state->exits;
    if (state->request_exit_reentry && !state->exit_reentry_entered && state->engine) {
        state->exit_reentry_entered = true;
        unsigned updates = state->updates, events = state->events, exits = state->exits;
        SDL_Event event = quit_event();
        state->exit_event = jce_engine_event(state->engine, &event);
        state->exit_iterate = jce_engine_iterate(state->engine);
        jce_engine_destroy(state->engine);
        state->exit_preserved_callbacks = state->updates == updates &&
            state->events == events && state->exits == exits;
    }
}

static JceEngine *boot_engine(const char *mode, char *config_path, size_t capacity)
{
    char expected[1024];
    int length = snprintf(expected, sizeof expected, "%s/%s.csv",
                          JCE_ENGINE_KPI_TEST_OUTPUT, mode);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof expected);
    TEST_ASSERT_EQUAL_STRING(mode, getenv("JCE_ENGINE_KPI_LIFECYCLE_CASE"));
    TEST_ASSERT_EQUAL_STRING(expected, getenv("JCE_KPI_FRAME_LOG"));
    TEST_ASSERT_EQUAL_STRING("1", getenv("JCE_HEADLESS"));
    jce_fs_host_remove_file(expected); /* Only this fixture's declared output. */
    length = snprintf(expected, sizeof expected, "%s/%s.log",
                      JCE_ENGINE_KPI_TEST_OUTPUT, mode);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof expected);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, getenv("JCE_LOG_FILE"),
        "Fixture may reset only its compile-time declared session log");
    /* The logger normally appends. Start this owned test session empty so a
     * repeat neither counts old summaries nor grows the bounded read budget.
     * Never reset an arbitrary environment-supplied path or archived reports. */
    TEST_ASSERT_TRUE(jce_fs_host_write_all(expected, "", 0));
    length = snprintf(config_path, capacity, "%s/%s.ini", JCE_ENGINE_KPI_TEST_OUTPUT, mode);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < capacity);
    static const char config[] = "[performance]\njob_workers=1\n";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(config_path, config, sizeof config - 1u));
    jce_engine_set_config_path(config_path);
    JceAppDesc app = {0};
    app.name = "kpi-public-headless-lifecycle";
    app.headless = true;
    app.init = app_init; app.update = app_update; app.draw = app_draw;
    app.on_event = app_event; app.exit = app_exit; app.user_data = &probe;
    jce_engine_set_app_desc(&app);
    char argument[] = "test_jce_engine_kpi_lifecycle";
    char *argv[] = {argument, NULL};
    JceEngine *engine = jce_engine_create(1, argv);
    TEST_ASSERT_NOT_NULL(engine);
    probe.engine = engine;
    TEST_ASSERT_TRUE(probe.actual_headless);
    TEST_ASSERT_EQUAL_UINT(1, probe.inits);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(engine));
    TEST_ASSERT_EQUAL_UINT(1, probe.updates);
    TEST_ASSERT_EQUAL_UINT(0, probe.draws);
    return engine;
}

static void verify_closed_headless_capture_and_cleanup(const char *config_path)
{
    const char *path = getenv("JCE_KPI_FRAME_LOG");
    uint64_t size = 0;
    void *bytes = jce_fs_host_read_all(path, &size);
    static const char header[] = "frame_index,phase,ms\n";
    bool exact = bytes && size == sizeof header - 1u &&
        memcmp(bytes, header, sizeof header - 1u) == 0;
    jce_fs_buffer_free(bytes);
    bool removed = jce_fs_host_remove_file(path);
    bool config_removed = jce_fs_host_remove_file(config_path);
    jce_engine_set_config_path(NULL);
    TEST_ASSERT_TRUE_MESSAGE(exact,
        "Headless fast path must retain exact initialized CSV header, not fake render rows");
    TEST_ASSERT_TRUE(removed); TEST_ASSERT_TRUE(config_removed);
}

static JceAsyncRunResult ready_work(JceAsyncContext *context, void *user)
{
    LifecycleProbe *state = user;
    (void)context;
    SDL_AddAtomicInt(&state->work_runs, 1);
    return JCE_ASYNC_RUN_SUCCESS;
}
static void ready_completion(JceAsyncTask *task, void *user)
{
    LifecycleProbe *state = user;
    ++state->completions;
    if (state->destroy_on_completion) {
        request_callback_destroy(state);
        return;
    }
    unsigned updates = state->updates, events = state->events, exits = state->exits;
    SDL_Event event = quit_event();
    state->completion_event = jce_engine_event(state->engine, &event);
    state->completion_iterate = jce_engine_iterate(state->engine);
    jce_engine_destroy(state->engine);
    state->completion_preserved_callbacks = state->updates == updates &&
        state->events == events && state->exits == exits;
    (void)task; /* Borrowed actual callback handle; caller ref released outside. */
}

static bool settle_existing_shared_work(JceAsyncExecutor *executor)
{
    for (unsigned attempt = 0; attempt < 5000; ++attempt) {
        JceAsyncExecutorStats status;
        jce_async_executor_get_stats(executor, &status);
        if (!status.live_tasks) return true;
        jce_async_executor_pump(executor, NULL);
        jce_thread_sleep_ms(1);
    }
    return false;
}

static JceAsyncTask *prepare_terminal_work_without_owner_completion(void)
{
    JceAsyncExecutor *executor = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(executor);
    probe.executor = executor;
    TEST_ASSERT_TRUE(settle_existing_shared_work(executor));
    JceAsyncTaskDesc task;
    jce_async_task_desc_init(&task);
    task.work = ready_work; task.complete = ready_completion;
    task.user_data = &probe; task.debug_name = "engine-callback-lifetime";
    task.priority = JCE_ASYNC_PRIORITY_HIGH;
    JceAsyncTask *handle = jce_async_submit(executor, &task);
    TEST_ASSERT_NOT_NULL(handle);
    if (jce_async_executor_mode(executor) == JCE_ASYNC_EXECUTION_COOPERATIVE) {
        /* Capability-based path: run work with actual completion budget zero.
         * No platform macro and no fake terminal state or private executor. */
        JceAsyncPumpBudget budget;
        jce_async_pump_budget_init(&budget);
        budget.max_work_items = 1; budget.max_completions = 0; budget.max_time_us = 0;
        jce_async_executor_pump(executor, &budget);
    }
    JceAsyncWaitResult waited = jce_async_task_wait_timeout(handle, 5000);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED, waited);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_SUCCEEDED, jce_async_task_state(handle));
    TEST_ASSERT_TRUE(jce_async_task_is_terminal(handle));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&probe.work_runs));
    TEST_ASSERT_EQUAL_UINT(0, probe.completions);
    JceAsyncExecutorStats ready;
    jce_async_executor_get_stats(executor, &ready);
    TEST_ASSERT_EQUAL_UINT(1, ready.pending_completions);
    return handle;
}

static void test_terminal_quit_finalization_rejects_shared_completion_engine_reentry(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("terminal", config_path, sizeof config_path);
    JceAsyncTask *handle = prepare_terminal_work_without_owner_completion();
    unsigned updates = probe.updates, events = probe.events;
    SDL_Event event = quit_event();
    JceAppResult result = jce_engine_event(engine, &event);
    unsigned completions_after_event = probe.completions;
    unsigned exits_before_destroy = probe.exits;
    unsigned updates_after_event = probe.updates, events_after_event = probe.events;
    bool callbacks_preserved = probe.completion_preserved_callbacks;
    JceAppResult nested_event = probe.completion_event;
    JceAppResult nested_iterate = probe.completion_iterate;
    jce_async_task_release(handle);
    /* Recursive destroy in the real completion must not have freed this owner. */
    jce_engine_destroy(engine);
    probe.engine = NULL;
    verify_closed_headless_capture_and_cleanup(config_path);

    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result);
    TEST_ASSERT_EQUAL_UINT(1, completions_after_event);
    TEST_ASSERT_TRUE(callbacks_preserved);
    TEST_ASSERT_NOT_EQUAL(JCE_APP_SUCCESS, nested_event);
    TEST_ASSERT_NOT_EQUAL(JCE_APP_SUCCESS, nested_iterate);
    TEST_ASSERT_EQUAL_UINT(0, exits_before_destroy);
    TEST_ASSERT_EQUAL_UINT(updates, updates_after_event);
    TEST_ASSERT_EQUAL_UINT(events + 1u, events_after_event); /* Only outer quit. */
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);
    TEST_ASSERT_EQUAL_UINT(0, probe.draws);
}

static void test_exit_callback_recursive_destroy_iterate_event_runs_only_once(void)
{
    probe.request_exit_reentry = true;
    char config_path[1024];
    JceEngine *engine = boot_engine("exit", config_path, sizeof config_path);
    unsigned updates = probe.updates, events = probe.events;
    jce_engine_destroy(engine);
    probe.engine = NULL;
    verify_closed_headless_capture_and_cleanup(config_path);

    TEST_ASSERT_TRUE(probe.exit_reentry_entered);
    TEST_ASSERT_TRUE(probe.exit_preserved_callbacks);
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);
    TEST_ASSERT_EQUAL_UINT(updates, probe.updates);
    TEST_ASSERT_EQUAL_UINT(events, probe.events);
    TEST_ASSERT_NOT_EQUAL(JCE_APP_SUCCESS, probe.exit_event);
    TEST_ASSERT_NOT_EQUAL(JCE_APP_SUCCESS, probe.exit_iterate);
    TEST_ASSERT_EQUAL_UINT(0, probe.draws);
}

/* All owners remain alive through the actual public outer call. The host
 * consumes the quit result, then performs the one final reverse teardown. */
static void verify_callback_destroy_and_close(JceEngine *engine,
                                              const char *config_path)
{
    TEST_ASSERT_TRUE(probe.destroy_request_entered);
    TEST_ASSERT_TRUE(probe.destroy_retained_owner);
    TEST_ASSERT_TRUE(probe.destroy_retained_services);
    TEST_ASSERT_EQUAL_UINT(0, probe.exits);
    TEST_ASSERT_TRUE(jce_engine_quit_requested());
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, probe.destroy_nested_iterate);
    unsigned updates = probe.updates;
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_iterate(engine));
    TEST_ASSERT_EQUAL_UINT(updates, probe.updates);
    TEST_ASSERT_EQUAL_UINT(0, probe.exits);
    jce_engine_destroy(engine);
    probe.engine = NULL;
    verify_closed_headless_capture_and_cleanup(config_path);
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);
    TEST_ASSERT_EQUAL_UINT(0, probe.draws);
}

static void test_ordinary_completion_destroy_retains_frame_owner(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("completion", config_path, sizeof config_path);
    probe.destroy_on_completion = true;
    probe.try_nested_iterate = true;
    JceAsyncTask *handle = prepare_terminal_work_without_owner_completion();
    unsigned updates = probe.updates;
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_iterate(engine));
    TEST_ASSERT_EQUAL_UINT(1, probe.completions);
    TEST_ASSERT_EQUAL_UINT(updates, probe.updates);
    TEST_ASSERT_EQUAL_PTR(probe.executor, jce_async_default_executor());
    jce_async_task_release(handle);
    verify_callback_destroy_and_close(engine, config_path);
}

static void test_update_destroy_retains_frame_owner(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("update", config_path, sizeof config_path);
    probe.destroy_on_update = true;
    probe.try_nested_iterate = true;
    unsigned updates = probe.updates;
    JceAppResult result = jce_engine_iterate(engine);
    TEST_ASSERT_TRUE(result == JCE_APP_CONTINUE || result == JCE_APP_SUCCESS);
    TEST_ASSERT_EQUAL_UINT(updates + 1u, probe.updates);
    verify_callback_destroy_and_close(engine, config_path);
}

static void test_event_destroy_retains_dispatch_owner(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("event", config_path, sizeof config_path);
    probe.destroy_on_event = true;
    probe.try_nested_iterate = true;
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_USER;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_event(engine, &event));
    TEST_ASSERT_EQUAL_UINT(1, probe.events);
    verify_callback_destroy_and_close(engine, config_path);
}

static void test_manual_completion_destroy_retains_executor_and_services(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("manual", config_path, sizeof config_path);
    probe.destroy_on_completion = true;
    JceAsyncTask *handle = prepare_terminal_work_without_owner_completion();
    TEST_ASSERT_EQUAL_UINT(1, jce_async_default_pump(NULL));
    TEST_ASSERT_EQUAL_UINT(1, probe.completions);
    TEST_ASSERT_EQUAL_PTR(probe.executor, jce_async_default_executor());
    jce_async_task_release(handle);
    verify_callback_destroy_and_close(engine, config_path);
}

static unsigned occurrences(const char *text, const char *needle)
{
    unsigned count = 0;
    while ((text = strstr(text, needle)) != NULL) { ++count; text += strlen(needle); }
    return count;
}

/* Public complete Engine behavior, including its real native counter, logger
 * shutdown and writer finalization. No new private declarations are needed:
 * the old owner compiles and fails the missing-evidence assertion ordinarily. */
static void verify_headless_disposition_log(void)
{
    TEST_ASSERT_EQUAL_UINT(3, probe.updates);
    TEST_ASSERT_EQUAL_UINT(2, probe.nested_observations);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, probe.observed_nested_iterate);
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);

    uint64_t size = 0;
    char *log = jce_fs_host_read_all(getenv("JCE_LOG_FILE"), &size);
    TEST_ASSERT_NOT_NULL(log);
    if (size > 65536u) { jce_fs_buffer_free(log); TEST_FAIL_MESSAGE("Bounded headless log exceeded fixture budget"); }
    const char *summary = strstr(log, "frame disposition v1: ");
    const char *reason = strstr(log, "frame disposition reason v1: reason=headless ");
    unsigned summary_count = occurrences(log, "frame disposition v1: ");
    unsigned reason_count = occurrences(log, "frame disposition reason v1: ");
    unsigned long long frequency = 0, begin = 0, end = 0, intervals = 0, full = 0, nonfull = 0;
    unsigned valid = 0, errors = 0;
    int summary_fields = summary ? sscanf(summary,
        "frame disposition v1: frequency=%llu begin=%llu end=%llu intervals=%llu full=%llu nonfull=%llu valid=%u errors=%u",
        &frequency, &begin, &end, &intervals, &full, &nonfull, &valid, &errors) : 0;
    unsigned long long count = 0, ticks = 0, first_interval = 0, first_row = 0;
    unsigned long long first_begin = 0, first_end = 0, last_interval = 0, last_row = 0;
    unsigned long long last_begin = 0, last_end = 0;
    int reason_fields = reason ? sscanf(reason,
        "frame disposition reason v1: reason=headless count=%llu ticks=%llu first_interval=%llu first_row=%llu first_begin=%llu first_end=%llu last_interval=%llu last_row=%llu last_begin=%llu last_end=%llu",
        &count, &ticks, &first_interval, &first_row, &first_begin, &first_end,
        &last_interval, &last_row, &last_begin, &last_end) : 0;
    const char *clock = strstr(log, "frame KPI clock v1: ");
    unsigned clock_count = occurrences(log, "frame KPI clock v1: ");
    bool deadline_absent = strstr(log, "JCE_MAX_SECONDS reached") == NULL;
    bool clock_unchecked = clock && strstr(clock, "timer_checked=0 ") != NULL;
    bool clock_invalid = clock && strstr(clock, "valid=0 errors=32") != NULL;
    bool writer_empty = strstr(log, "frame KPI capture complete: accepted=0 written=0 ") != NULL;
    bool untruncated = strstr(log, "jce_log_truncated") == NULL;
    jce_fs_buffer_free(log);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, summary_count, "Missing once-only actual Engine disposition summary");
    TEST_ASSERT_EQUAL_UINT(1, reason_count);
    TEST_ASSERT_EQUAL_INT(8, summary_fields);
    TEST_ASSERT_EQUAL_INT(10, reason_fields);
    TEST_ASSERT_TRUE(frequency > 0 && end >= begin);
    TEST_ASSERT_EQUAL_UINT64(3, intervals);
    TEST_ASSERT_EQUAL_UINT64(0, full);
    TEST_ASSERT_EQUAL_UINT64(3, nonfull);
    TEST_ASSERT_EQUAL_UINT(1, valid); TEST_ASSERT_EQUAL_UINT(0, errors);
    TEST_ASSERT_EQUAL_UINT64(3, count);
    TEST_ASSERT_EQUAL_UINT64(end - begin, ticks);
    TEST_ASSERT_EQUAL_UINT64(0, first_interval); TEST_ASSERT_EQUAL_UINT64(2, last_interval);
    TEST_ASSERT_EQUAL_UINT64(0, first_row); TEST_ASSERT_EQUAL_UINT64(0, last_row);
    TEST_ASSERT_EQUAL_UINT64(begin, first_begin); TEST_ASSERT_EQUAL_UINT64(end, last_end);
    TEST_ASSERT_TRUE(first_end >= first_begin && first_end <= last_begin && last_end >= last_begin);
    TEST_ASSERT_TRUE(deadline_absent);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, clock_count, "Missing once-only actual Engine clock witness");
    TEST_ASSERT_TRUE(clock_unchecked); TEST_ASSERT_TRUE(clock_invalid);
    TEST_ASSERT_TRUE(writer_empty); TEST_ASSERT_TRUE(untruncated);
}

static void test_headless_disposition_closes_raw_intervals_once_without_inventing_deadline(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("disposition", config_path, sizeof config_path);
    probe.observe_nested_iterate = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(engine));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(engine));
    SDL_Event event = quit_event();
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_event(engine, &event));
    /* A repeated public terminal path must not count or publish the tail twice. */
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_event(engine, &event));
    jce_engine_destroy(engine);
    probe.engine = NULL;
    verify_closed_headless_capture_and_cleanup(config_path);
    verify_headless_disposition_log();
}

static void test_direct_destroy_first_terminal_closes_the_same_raw_disposition(void)
{
    char config_path[1024];
    JceEngine *engine = boot_engine("disposition-direct", config_path, sizeof config_path);
    probe.observe_nested_iterate = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(engine));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(engine));
    TEST_ASSERT_EQUAL_UINT(0, probe.events);
    jce_engine_destroy(engine); /* First terminal owner, with no prior quit event. */
    probe.engine = NULL;
    verify_closed_headless_capture_and_cleanup(config_path);
    verify_headless_disposition_log();
}

int main(void)
{
    const char *mode = getenv("JCE_ENGINE_KPI_LIFECYCLE_CASE");
    if (!mode) { fputs("missing isolated CTest lifecycle case\n", stderr); return 2; }
    UNITY_BEGIN();
    if (strcmp(mode, "terminal") == 0)
        RUN_TEST(test_terminal_quit_finalization_rejects_shared_completion_engine_reentry);
    else if (strcmp(mode, "exit") == 0)
        RUN_TEST(test_exit_callback_recursive_destroy_iterate_event_runs_only_once);
    else if (strcmp(mode, "completion") == 0)
        RUN_TEST(test_ordinary_completion_destroy_retains_frame_owner);
    else if (strcmp(mode, "update") == 0)
        RUN_TEST(test_update_destroy_retains_frame_owner);
    else if (strcmp(mode, "event") == 0)
        RUN_TEST(test_event_destroy_retains_dispatch_owner);
    else if (strcmp(mode, "manual") == 0)
        RUN_TEST(test_manual_completion_destroy_retains_executor_and_services);
    else if (strcmp(mode, "disposition") == 0)
        RUN_TEST(test_headless_disposition_closes_raw_intervals_once_without_inventing_deadline);
    else if (strcmp(mode, "disposition-direct") == 0)
        RUN_TEST(test_direct_destroy_first_terminal_closes_the_same_raw_disposition);
    else { fputs("unknown isolated CTest lifecycle case\n", stderr); return 2; }
    return UNITY_END();
}
