/* Real headless engine teardown with the actual sampler and controlled OS results. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_renderer.h>
#include <SDL3/SDL.h>
#include "jce_sampler_test_owner.h"

#ifndef JCE_ENGINE_SAMPLER_TEST_OUTPUT
#error "JCE_ENGINE_SAMPLER_TEST_OUTPUT must name ignored test output"
#endif

const unsigned char assets_pak_data[1] = {0};
const size_t assets_pak_data_size = 0;

typedef struct {
    JceEngine *engine;
    JceRenderer *renderer;
    unsigned inits, exits, updates, draws, events, resizes;
    unsigned init_stop_queries;
    bool exit_saw_sampler_stopped;
    bool fail_init;
    bool fail_resume;
    bool cancel_init;
    bool cancel_update, update_destroy_refused, update_owner_retained;
    bool init_owner_visible, init_destroy_refused, init_owner_retained;
    int init_fps;
    HANDLE acquired;
} EngineSamplerProbe;
static EngineSamplerProbe probe;

static void set_trace_environment(const char *name, const char *value)
{
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(name, value));
#else
    TEST_ASSERT_EQUAL_INT(0, setenv(name, value, 1));
#endif
}

/* Fixture-only stream selection. Production already snapshots these public
 * capture options per boot; never reuse the failed owner's output for reboot. */
static void select_trace_output(char path[1024], const char *ownership,
                                const char *mode, const char *session)
{
    int length = snprintf(path, 1024, "%s/%s-%s.%s.json",
                          JCE_ENGINE_SAMPLER_TEST_OUTPUT, ownership, mode, session);
    TEST_ASSERT_TRUE(length > 0 && length < 1024);
    if (jce_fs_host_exists_file(path))
        TEST_ASSERT_TRUE(jce_fs_host_remove_file(path));
    set_trace_environment("JCE_TRACE_STREAM", path);
    set_trace_environment("JCE_TRACE_STREAM_MODE", "threaded");
    set_trace_environment("JCE_TRACE_EXPORT", "");
}

static void *read_complete_trace_bytes(const char *path, uint64_t *size)
{
    uint64_t total = 0;
    void *bytes = jce_fs_host_read_capped(path, 4u * 1024u * 1024u, size, &total);
    TEST_ASSERT_NOT_NULL_MESSAGE(bytes, "actual continuous capture output is missing");
    TEST_ASSERT_TRUE(*size > 0 && *size == total);
    return bytes;
}

void setUp(void) { port_reset(); memset(&probe, 0, sizeof probe); }
void tearDown(void) {}

static bool app_init(const JceServices *services, void *user)
{
    EngineSamplerProbe *state = user;
    ++state->inits;
    state->renderer = services ? services->renderer : NULL;
    return services && !services->window && !services->input && !services->audio &&
        jce_renderer_is_headless(state->renderer);
}

/* Faults enter the real constructor after its services have been acquired. */
static bool failing_app_init(const JceServices *services, void *user)
{
    EngineSamplerProbe *state = user;
    bool ready = app_init(services, user);
    jce_engine_set_target_fps(90);
    state->init_fps = jce_engine_get_target_fps();
    if ((state->fail_init || state->cancel_init) && state->fail_resume &&
        g_s.worker && g_s.slots) {
        port_roster(1, 11, 100);
        s_resume_failure = true;
        samp_thread(NULL);
        state->acquired = g_thr[0].h;
    }
    if (state->cancel_init) {
        JceEngine *identity = state->engine;
        unsigned waits_before = s_waits;
        state->init_owner_visible = identity != NULL;
        state->init_destroy_refused = state->init_owner_retained = true;
        for (unsigned i = 0; i < 2; ++i) {
            state->init_destroy_refused &= !jce_engine_try_destroy(&state->engine);
            state->init_owner_retained &= state->engine == identity;
        }
        /* A state-only borrow check lets the second request enter teardown.
         * The sampler's timeout keeps that old path readable for an ordinary
         * assertion failure instead of requiring a use-after-free crash. */
        state->init_stop_queries = s_waits - waits_before;
    }
    return ready && !state->fail_init;
}

static void app_update(float dt, void *user)
{
    (void)dt;
    EngineSamplerProbe *state = user;
    ++state->updates;
    if (state->cancel_update) {
        JceEngine *identity = state->engine;
        state->update_destroy_refused = !jce_engine_try_destroy(&state->engine);
        state->update_owner_retained = state->engine == identity;
    }
}
static void app_draw(const JceServices *services, void *user)
{ (void)services; ++((EngineSamplerProbe *)user)->draws; }
static void app_event(const JceEvent *event, void *user)
{ (void)event; ++((EngineSamplerProbe *)user)->events; }
static void app_resize(uint32_t width, uint32_t height, void *user)
{ (void)width; (void)height; ++((EngineSamplerProbe *)user)->resizes; }

static void app_exit(void *user)
{
    EngineSamplerProbe *state = user;
    ++state->exits;
    state->exit_saw_sampler_stopped = !g_s.worker && !g_s.slots;
}

static void test_failed_sampler_stop_retains_real_engine_until_safe_retry(void)
{
    const char *mode = getenv("JCE_ENGINE_SAMPLER_CASE");
    TEST_ASSERT_NOT_NULL(mode);
    bool timeout = !strcmp(mode, "timeout");
    bool failed = !strcmp(mode, "wait_failed");
    bool resume = !strcmp(mode, "resume_failed");
    TEST_ASSERT_TRUE(timeout || failed || resume);
    char config_path[1024];
    int length = snprintf(config_path, sizeof config_path, "%s/%s.ini",
                          JCE_ENGINE_SAMPLER_TEST_OUTPUT, mode);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof config_path);
    static const char config[] = "[performance]\njob_workers=1\n";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(config_path, config, sizeof config - 1u));
    jce_engine_set_config_path(config_path);
    JceAppDesc app = {0};
    app.name = "sampler-engine-lifetime";
    app.headless = true; app.init = app_init; app.exit = app_exit;
    app.user_data = &probe;
    jce_engine_set_app_desc(&app);
    char argument[] = "test_jce_engine_sampler_lifetime";
    char *argv[] = {argument, NULL};
    probe.engine = jce_engine_create(1, argv);
    TEST_ASSERT_NOT_NULL(probe.engine);
    TEST_ASSERT_EQUAL_UINT(1, probe.inits);
    TEST_ASSERT_TRUE(jce_renderer_is_headless(probe.renderer));
    JceAsyncExecutor *executor = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(executor);
    port_start(); /* Idempotent init also captures the actual retained slot owner. */
    HANDLE worker = g_s.worker, target = g_s.target;
    HANDLE acquired = NULL;
    if (resume) {
        port_roster(1, 11, 100);
        s_resume_failure = true;
        samp_thread(NULL);
        TEST_ASSERT_EQUAL_UINT(1, s_suspends);
        acquired = g_thr[0].h;
        TEST_ASSERT_NOT_NULL(acquired);
        TEST_ASSERT_EQUAL_UINT(1, acquired->lifetime->suspend_count);
    }
    s_wait_result = timeout ? WAIT_TIMEOUT : failed ? WAIT_FAILED : WAIT_OBJECT_0;
    jce_engine_destroy(probe.engine);
    /* This assertion precedes all borrowed-pointer reads on the old unsafe path. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, probe.exits,
        "Sampler stop refusal must precede application and service teardown");
    TEST_ASSERT_EQUAL_PTR(executor, jce_async_default_executor());
    TEST_ASSERT_TRUE(jce_renderer_is_headless(probe.renderer));
    TEST_ASSERT_EQUAL_PTR(worker, g_s.worker);
    TEST_ASSERT_EQUAL_PTR(target, g_s.target);
    TEST_ASSERT_EQUAL_PTR(s_initial_slots, g_s.slots);
    TEST_ASSERT_EQUAL_UINT(0, worker->closes + target->closes + s_slot_frees + s_writes);
    jce_engine_destroy(probe.engine); /* A repeated refusal still retains everything. */
    TEST_ASSERT_EQUAL_UINT(0, probe.exits);
    TEST_ASSERT_EQUAL_PTR(executor, jce_async_default_executor());
    TEST_ASSERT_EQUAL_UINT(0, s_slot_frees);
    s_wait_result = WAIT_OBJECT_0; s_resume_failure = false;
    jce_engine_destroy(probe.engine);
    probe.engine = NULL;
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);
    TEST_ASSERT_TRUE(probe.exit_saw_sampler_stopped);
    TEST_ASSERT_EQUAL_UINT(1, worker->closes);
    TEST_ASSERT_EQUAL_UINT(1, target->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
    TEST_ASSERT_EQUAL_UINT(1, s_created_workers);
    if (acquired) {
        TEST_ASSERT_EQUAL_UINT(1, acquired->closes);
        TEST_ASSERT_EQUAL_UINT(0, acquired->lifetime->suspend_count);
    }
    TEST_ASSERT_TRUE(jce_sampler_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(config_path));
    jce_engine_set_config_path(NULL);
}

static void assert_cleanup_trace(unsigned minimum_refusals, unsigned completions,
                                 unsigned minimum_constructing)
{
    JceTraceEvent events[256];
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    uint64_t active = 0;
    unsigned begins = 0, ends = 0, refused = 0, completed = 0, constructing = 0;
    int reason = -1, result = -1;
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 256)) != 0) {
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type == JCE_TRACE_EVENT_TASK_BEGIN &&
                strcmp(event->name, "engine.cleanup_attempt") == 0) {
                TEST_ASSERT_EQUAL_UINT64(0, active);
                active = event->id; reason = result = -1; ++begins;
            } else if (active && event->type == JCE_TRACE_EVENT_COUNTER &&
                       strcmp(event->name, "cleanup.reason") == 0) {
                TEST_ASSERT_EQUAL_UINT64(active, event->id);
                TEST_ASSERT_EQUAL_UINT64(active, event->parent_id);
                reason = (int)event->value_u64;
            } else if (active && event->type == JCE_TRACE_EVENT_COUNTER &&
                       strcmp(event->name, "cleanup.completed") == 0) {
                TEST_ASSERT_EQUAL_UINT64(active, event->id);
                TEST_ASSERT_EQUAL_UINT64(active, event->parent_id);
                result = (int)event->value_u64;
            } else if (event->type == JCE_TRACE_EVENT_TASK_END &&
                       strcmp(event->name, "engine.cleanup_attempt") == 0) {
                TEST_ASSERT_EQUAL_UINT64(active, event->id);
                TEST_ASSERT_TRUE_MESSAGE(reason >= 0, "cleanup attempt needs a reason");
                TEST_ASSERT_TRUE_MESSAGE(result >= 0, "cleanup attempt needs completion state");
                if (reason == 3) {
                    TEST_ASSERT_EQUAL_INT(0, result);
                    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_FAILED, event->value_u32[0]);
                    ++refused;
                } else if (reason == 0) {
                    TEST_ASSERT_EQUAL_INT(1, result);
                    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SUCCEEDED, event->value_u32[0]);
                    ++completed;
                } else if (reason == 1) {
                    TEST_ASSERT_EQUAL_INT(0, result);
                    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_FAILED, event->value_u32[0]);
                    ++constructing;
                }
                active = 0; ++ends;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT64(0, active);
    TEST_ASSERT_EQUAL_UINT(begins, ends);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(minimum_refusals, refused);
    TEST_ASSERT_EQUAL_UINT(completions, completed);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(minimum_constructing, constructing);
}

static void assert_retained_services(JceAsyncExecutor *executor,
                                     HANDLE worker, HANDLE target)
{
    /* This comes before borrowed renderer and service reads on unsafe paths. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, probe.exits,
        "Failed boot must retain services until the sampler can release them");
    TEST_ASSERT_EQUAL_PTR(executor, jce_async_default_executor());
    TEST_ASSERT_TRUE(jce_renderer_is_headless(probe.renderer));
    TEST_ASSERT_EQUAL_PTR(worker, g_s.worker);
    TEST_ASSERT_EQUAL_PTR(target, g_s.target);
    TEST_ASSERT_EQUAL_PTR(s_initial_slots, g_s.slots);
    TEST_ASSERT_EQUAL_UINT(0, worker->closes + target->closes + s_slot_frees + s_writes);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    TEST_ASSERT_EQUAL_UINT(1, probe.inits);
    TEST_ASSERT_EQUAL_UINT(probe.cancel_update ? 1u : 0u, probe.updates);
    TEST_ASSERT_EQUAL_UINT(0, probe.draws + probe.events + probe.resizes);
    TEST_ASSERT_EQUAL_UINT(1, s_created_workers);
}

static void assert_released_services(HANDLE worker, HANDLE target)
{
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);
    TEST_ASSERT_TRUE(probe.exit_saw_sampler_stopped);
    TEST_ASSERT_EQUAL_UINT(1, worker->closes);
    TEST_ASSERT_EQUAL_UINT(1, target->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
    TEST_ASSERT_NULL(g_s.worker);
    TEST_ASSERT_NULL(g_s.target);
    TEST_ASSERT_NULL(g_s.slots);
    /* Latest ordinary rate, preserved while the launch override owned it. */
    TEST_ASSERT_EQUAL_INT(72, jce_engine_get_target_fps());
    if (probe.acquired) {
        TEST_ASSERT_EQUAL_UINT(1, probe.acquired->closes);
        TEST_ASSERT_EQUAL_UINT(0, probe.acquired->lifetime->suspend_count);
    }
}

static void assert_next_boot_works(char *argv[])
{
    unsigned updates_before = probe.updates;
    probe.fail_init = false;
    probe.cancel_init = false;
    probe.cancel_update = false;
    probe.acquired = NULL;
    port_reset();
    JceEngine *next = NULL;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_create_owned(1, argv, &next));
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_EQUAL_UINT(2, probe.inits);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(next));
    TEST_ASSERT_EQUAL_UINT(updates_before + 1u, probe.updates);
    TEST_ASSERT_EQUAL_UINT(0, probe.draws); /* The real headless host never draws. */
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&next));
    TEST_ASSERT_NULL(next);
    TEST_ASSERT_EQUAL_UINT(2, probe.exits);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
    TEST_ASSERT_EQUAL_INT(90, jce_engine_get_target_fps());
}

static void test_failed_boot_retains_cleanup_owner_and_recovers(void)
{
    const char *mode_env = getenv("JCE_ENGINE_SAMPLER_CASE");
    const char *ownership_env = getenv("JCE_ENGINE_CLEANUP_CASE");
    const char *trace_option = getenv("JCE_ENGINE_CLEANUP_TRACE");
    bool export_trace = trace_option && trace_option[0];
    char mode[32], ownership[32];
    char failure_trace[1024], reboot_trace[1024];
    uint64_t failure_trace_size = 0;
    void *failure_trace_bytes = NULL;
    TEST_ASSERT_NOT_NULL(mode_env);
    TEST_ASSERT_NOT_NULL(ownership_env);
    TEST_ASSERT_TRUE(strlen(mode_env) < sizeof mode);
    TEST_ASSERT_TRUE(strlen(ownership_env) < sizeof ownership);
    memcpy(mode, mode_env, strlen(mode_env) + 1u);
    memcpy(ownership, ownership_env, strlen(ownership_env) + 1u);
    bool timeout = strcmp(mode, "timeout") == 0;
    bool failed = strcmp(mode, "wait_failed") == 0;
    bool resume = strcmp(mode, "resume_failed") == 0;
    bool legacy = strcmp(ownership, "legacy") == 0;
    bool constructing = strcmp(ownership, "constructing") == 0;
    bool callback = strcmp(ownership, "callback") == 0;
    TEST_ASSERT_TRUE(timeout || failed || resume);
    TEST_ASSERT_TRUE(legacy || constructing || callback || strcmp(ownership, "owned") == 0);
    if (export_trace) {
        TEST_ASSERT_EQUAL_STRING("1", trace_option);
        select_trace_output(failure_trace, ownership, mode, "cleanup");
    }
    char config_path[1024];
    int length = snprintf(config_path, sizeof config_path, "%s/%s-%s.ini",
                          JCE_ENGINE_SAMPLER_TEST_OUTPUT, ownership, mode);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof config_path);
    static const char config[] = "[performance]\njob_workers=1\n";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(config_path, config, sizeof config - 1u));
    jce_engine_set_config_path(config_path);
    JceAppDesc app = {0};
    app.name = "sampler-failed-boot-owner";
    app.headless = true; app.init = failing_app_init; app.exit = app_exit;
    app.update = app_update; app.draw = app_draw;
    app.on_event = app_event; app.on_resize = app_resize;
    app.user_data = &probe;
    jce_engine_set_app_desc(&app);
    probe.fail_init = !(constructing || callback); probe.fail_resume = resume;
    probe.cancel_init = constructing;
    probe.cancel_update = callback;
    s_wait_result = timeout ? WAIT_TIMEOUT : failed ? WAIT_FAILED : WAIT_OBJECT_0;
    jce_engine_set_target_fps(165);
    jce_trace_reset();
    char argument[] = "test_jce_engine_sampler_lifetime";
    char *argv[] = {argument, NULL};
    TEST_ASSERT_FALSE(jce_engine_try_destroy(NULL));
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&probe.engine));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_create_owned(1, argv, NULL));
    TEST_ASSERT_EQUAL_UINT(0, s_created_workers);
    if (legacy) {
        probe.engine = jce_engine_create(1, argv);
        TEST_ASSERT_NULL(probe.engine);
    } else {
        TEST_ASSERT_EQUAL_INT(callback ? JCE_APP_CONTINUE : JCE_APP_FAILURE,
            jce_engine_create_owned(1, argv, &probe.engine));
        /* The lost-owner negative must fail here, before any borrowed access. */
        TEST_ASSERT_NOT_NULL_MESSAGE(probe.engine,
            "Failed initialization with deferred cleanup must return its retained owner");
    }
    if (callback) {
        TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_iterate(probe.engine));
        TEST_ASSERT_TRUE(probe.update_destroy_refused);
        TEST_ASSERT_TRUE(probe.update_owner_retained);
    }
    TEST_ASSERT_EQUAL_INT(0, probe.init_fps);
    if (constructing) {
        TEST_ASSERT_TRUE(probe.init_owner_visible);
        TEST_ASSERT_TRUE(probe.init_destroy_refused);
        TEST_ASSERT_TRUE(probe.init_owner_retained);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, probe.init_stop_queries,
            "Repeated constructor close must not enter sampler/service teardown");
    }
    TEST_ASSERT_EQUAL_UINT(1, probe.inits);
    JceAsyncExecutor *executor = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(executor);
    HANDLE worker = g_s.worker, target = g_s.target;
    TEST_ASSERT_NOT_NULL(worker);
    TEST_ASSERT_NOT_NULL(target);
    if (resume) {
        TEST_ASSERT_NOT_NULL(probe.acquired);
        TEST_ASSERT_EQUAL_UINT(1, probe.acquired->lifetime->suspend_count);
    }
    assert_retained_services(executor, worker, target);
    jce_engine_set_target_fps(72);
    EngineSamplerProbe replacement_probe = {0};
    JceAppDesc replacement_app = app;
    replacement_app.name = "denied-replacement";
    replacement_app.user_data = &replacement_probe;
    jce_engine_set_app_desc(&replacement_app);
    if (legacy) {
        /* Both documented legacy retry routes preserve the failed session. */
        TEST_ASSERT_NULL(jce_engine_create(1, argv));
        assert_retained_services(executor, worker, target);
        jce_engine_destroy(NULL);
        assert_retained_services(executor, worker, target);
    } else {
        JceEngine *identity = probe.engine;
        TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE,
            jce_engine_create_owned(1, argv, &probe.engine));
        TEST_ASSERT_EQUAL_PTR(identity, probe.engine); /* Occupied slot is unchanged. */
        JceEngine *replacement = NULL;
        TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE,
            jce_engine_create_owned(1, argv, &replacement));
        TEST_ASSERT_NULL(replacement);
        TEST_ASSERT_NULL(jce_engine_create(1, argv));
        JceAppResult terminal = callback ? JCE_APP_SUCCESS : JCE_APP_FAILURE;
        SDL_Event event = {0}; event.type = SDL_EVENT_KEY_DOWN;
        TEST_ASSERT_EQUAL_INT(terminal, jce_engine_event(identity, &event));
        event.type = SDL_EVENT_WINDOW_RESIZED;
        event.window.data1 = 640; event.window.data2 = 480;
        TEST_ASSERT_EQUAL_INT(terminal, jce_engine_event(identity, &event));
        TEST_ASSERT_EQUAL_INT(terminal, jce_engine_iterate(identity));
        for (unsigned i = 0; i < (callback ? 3u : 2u); ++i) {
            TEST_ASSERT_FALSE(jce_engine_try_destroy(&probe.engine));
            TEST_ASSERT_EQUAL_PTR(identity, probe.engine);
            assert_retained_services(executor, worker, target);
        }
    }
    assert_cleanup_trace(3, 0, constructing ? 2u : callback ? 1u : 0u);
    s_wait_result = WAIT_OBJECT_0; s_resume_failure = false;
    if (legacy) TEST_ASSERT_NULL(jce_engine_create(1, argv));
    else TEST_ASSERT_TRUE(jce_engine_try_destroy(&probe.engine));
    TEST_ASSERT_NULL(probe.engine);
    assert_released_services(worker, target);
    TEST_ASSERT_EQUAL_UINT(1, probe.inits); /* A legacy retry only drains. */
    TEST_ASSERT_EQUAL_UINT(0, replacement_probe.inits + replacement_probe.exits);
    assert_cleanup_trace(3, 1, constructing ? 2u : callback ? 1u : 0u);
    /* Idempotence must not re-run application exit or sampler release. */
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&probe.engine));
    jce_engine_destroy(NULL);
    TEST_ASSERT_TRUE(jce_sampler_shutdown());
    assert_released_services(worker, target);
    if (export_trace) {
        failure_trace_bytes = read_complete_trace_bytes(failure_trace, &failure_trace_size);
        select_trace_output(reboot_trace, ownership, mode, "reboot");
    }
    jce_engine_set_app_desc(&app);
    assert_next_boot_works(argv);
    if (export_trace) {
        uint64_t after_size = 0, reboot_size = 0;
        void *after = read_complete_trace_bytes(failure_trace, &after_size);
        void *reboot = read_complete_trace_bytes(reboot_trace, &reboot_size);
        TEST_ASSERT_EQUAL_UINT64(failure_trace_size, after_size);
        TEST_ASSERT_TRUE_MESSAGE(memcmp(failure_trace_bytes, after, (size_t)after_size) == 0,
            "next boot overwrote the failed owner's continuous cleanup evidence");
        jce_fs_buffer_free(after);
        jce_fs_buffer_free(reboot);
        jce_fs_buffer_free(failure_trace_bytes);
        set_trace_environment("JCE_TRACE_STREAM", "");
        set_trace_environment("JCE_TRACE_STREAM_MODE", "");
        printf("CLEANUP_TRACE_CAPTURE first=%s reboot=%s first_bytes=%llu reboot_bytes=%llu\n",
               failure_trace, reboot_trace, (unsigned long long)failure_trace_size,
               (unsigned long long)reboot_size);
    }
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(config_path));
    jce_engine_set_config_path(NULL);
    jce_engine_set_target_fps(0);
    jce_trace_set_enabled(false);
}

int main(void)
{
    UNITY_BEGIN();
    const char *ownership = getenv("JCE_ENGINE_CLEANUP_CASE");
    if (ownership && ownership[0])
        RUN_TEST(test_failed_boot_retains_cleanup_owner_and_recovers);
    else
        RUN_TEST(test_failed_sampler_stop_retains_real_engine_until_safe_retry);
    return UNITY_END();
}
