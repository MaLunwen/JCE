/* Actual headless Engine and default executor. The only subsystem port adds
 * a real test descriptor before the real registry's initial init_all call. */
#include "unity.h"
#include <jce/application/jce_engine.h>
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_subsystem.h>
#include <jce/os/core/jce_async.h>
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const unsigned char assets_pak_data[1] = {0};
const size_t assets_pak_data_size = 0;
static bool retirement_init_subsystems(jce_subsystem_registry_t *registry,
                                       const JceServices *services);
#define jce_subsystem_init_all retirement_init_subsystems
#ifndef JCE_TEST_ASYNC_RETIREMENT_ENGINE
#define JCE_TEST_ASYNC_RETIREMENT_ENGINE "application/jce_engine.c"
#endif
#include JCE_TEST_ASYNC_RETIREMENT_ENGINE
#undef jce_subsystem_init_all

typedef struct AsyncRetirementProbe {
    bool fail_init, close_during_init;
    bool initialized, registered, initial_executor, initial_shared_pool;
    bool borrow_retained, borrow_refused, constructor_stopped, constructor_denied;
    bool constructor_shared_denied;
    unsigned exit_count, quiesce_count, shutdown_count;
    unsigned unexpected_executor, emergency_shutdown_failure;
    unsigned unexpected_shared_pool;
} AsyncRetirementProbe;
static AsyncRetirementProbe retirement_probe;
static JceEngine *retirement_owner;
static int retirement_set_fps_env(const char *value)
{
#ifdef _WIN32
    return _putenv_s("JCE_LAUNCH_TARGET_FPS", value);
#else
    return setenv("JCE_LAUNCH_TARGET_FPS", value, 1);
#endif
}

/* A behavioral mutant may recreate a pool. Drain that actual pool immediately
 * so an ordinary assertion, never leaked workers/process exit, is the oracle. */
static void retirement_try_recreate(void)
{
    JceAsyncExecutor *unexpected = jce_async_default_executor();
    if (unexpected) {
        ++retirement_probe.unexpected_executor;
        if (!jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_CANCEL_ALL,
                                        JCE_ASYNC_WAIT_INFINITE))
            ++retirement_probe.emergency_shutdown_failure;
    }
    /* The real shared pool must not reappear after its actual join either. */
    if (jce_thread_pool_shared()) {
        ++retirement_probe.unexpected_shared_pool;
        jce_thread_pool_shared_shutdown();
    }
}
static void retirement_subsystem_quiesce(void *context)
{
    AsyncRetirementProbe *probe = context;
    ++probe->quiesce_count;
    retirement_try_recreate();
}
static void retirement_subsystem_shutdown(void *context)
{
    AsyncRetirementProbe *probe = context;
    ++probe->shutdown_count;
    retirement_try_recreate();
}
static bool retirement_init_subsystems(jce_subsystem_registry_t *registry,
                                       const JceServices *services)
{
    jce_subsystem_desc_t desc = {0};
    desc.name = "async-retirement-probe";
    desc.priority = 300;
    desc.ctx = &retirement_probe;
    desc.quiesce = retirement_subsystem_quiesce;
    desc.shutdown = retirement_subsystem_shutdown;
    retirement_probe.registered = jce_subsystem_register(registry, &desc);
    return retirement_probe.registered && jce_subsystem_init_all(registry, services);
}
static bool retirement_app_init(const JceServices *services, void *context)
{
    AsyncRetirementProbe *probe = context;
    (void)services;
    probe->initialized = true;
    probe->initial_executor = jce_async_default_executor() != NULL;
    probe->initial_shared_pool = jce_thread_pool_shared() != NULL;
    if (probe->close_during_init) {
        JceEngine *before = retirement_owner;
        probe->borrow_refused = !jce_engine_try_destroy(&retirement_owner);
        probe->borrow_retained = before && retirement_owner == before;
        probe->constructor_stopped = jce_async_default_shutdown(
            JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE);
        probe->constructor_denied = jce_async_default_executor() == NULL;
        if (!probe->constructor_denied) retirement_try_recreate();
        jce_thread_pool_shared_shutdown();
        probe->constructor_shared_denied = jce_thread_pool_shared() == NULL;
        if (!probe->constructor_shared_denied) jce_thread_pool_shared_shutdown();
    }
    return !probe->fail_init;
}
static void retirement_app_exit(void *context)
{
    AsyncRetirementProbe *probe = context;
    ++probe->exit_count;
    /* Teardown may retire the frame pool before callbacks finish. The session
     * latch must outlive this join until a genuinely new Engine is admitted. */
    jce_thread_pool_shared_shutdown();
    retirement_try_recreate();
}
static JceAppResult retirement_create(void)
{
    JceAppDesc app = {0};
    char arg[] = "test-engine-async-retirement";
    char *argv[] = {arg, NULL};
    app.name = "async-retirement";
    app.headless = true;
    app.init = retirement_app_init;
    app.exit = retirement_app_exit;
    app.user_data = &retirement_probe;
    jce_engine_set_app_desc(&app);
    return jce_engine_create_owned(1, argv, &retirement_owner);
}
void setUp(void)
{
    memset(&retirement_probe, 0, sizeof(retirement_probe));
    retirement_owner = NULL;
}
void tearDown(void)
{
    if (retirement_owner) (void)jce_engine_try_destroy(&retirement_owner);
    (void)jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_CANCEL_ALL,
                                    JCE_ASYNC_WAIT_INFINITE);
    jce_thread_pool_shared_shutdown();
    (void)retirement_set_fps_env("0");
}
static void retirement_assert_callbacks(void)
{
    TEST_ASSERT_TRUE(retirement_probe.initialized);
    TEST_ASSERT_TRUE(retirement_probe.registered);
    TEST_ASSERT_TRUE(retirement_probe.initial_executor);
    TEST_ASSERT_TRUE(retirement_probe.initial_shared_pool);
    TEST_ASSERT_EQUAL_UINT(1, retirement_probe.exit_count);
    TEST_ASSERT_EQUAL_UINT(1, retirement_probe.quiesce_count);
    TEST_ASSERT_EQUAL_UINT(1, retirement_probe.shutdown_count);
    TEST_ASSERT_EQUAL_UINT(0, retirement_probe.emergency_shutdown_failure);
    TEST_ASSERT_EQUAL_UINT(0, retirement_probe.unexpected_executor);
    TEST_ASSERT_EQUAL_UINT(0, retirement_probe.unexpected_shared_pool);
    TEST_ASSERT_NULL(retirement_owner);
}
static void retirement_assert_gap_and_next_boot(void)
{
    char arg[] = "test-engine-async-reject";
    char *argv[] = {arg, NULL};
    JceEngine *occupied = (JceEngine *)(uintptr_t)1;
    TEST_ASSERT_NULL(jce_async_default_executor());
    TEST_ASSERT_NULL(jce_thread_pool_shared());
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_create_owned(1, argv, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_create_owned(1, argv, &occupied));
    TEST_ASSERT_EQUAL_PTR((JceEngine *)(uintptr_t)1, occupied);
    TEST_ASSERT_NULL(jce_async_default_executor());
    TEST_ASSERT_NULL(jce_thread_pool_shared());
    TEST_ASSERT_EQUAL_INT(0, retirement_set_fps_env("invalid"));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, retirement_create());
    TEST_ASSERT_NULL(retirement_owner);
    TEST_ASSERT_NULL(jce_async_default_executor());
    TEST_ASSERT_NULL(jce_thread_pool_shared());
    TEST_ASSERT_EQUAL_INT(0, retirement_set_fps_env("0"));
    memset(&retirement_probe, 0, sizeof(retirement_probe));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, retirement_create());
    TEST_ASSERT_NOT_NULL(jce_async_default_executor());
    TEST_ASSERT_NOT_NULL(jce_thread_pool_shared());
    JceEngine *duplicate = NULL;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_create_owned(1, argv, &duplicate));
    TEST_ASSERT_NULL(duplicate);
    TEST_ASSERT_NOT_NULL(jce_async_default_executor());
    TEST_ASSERT_NOT_NULL(jce_thread_pool_shared());
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&retirement_owner));
    retirement_assert_callbacks();
    TEST_ASSERT_NULL(jce_async_default_executor());
    TEST_ASSERT_NULL(jce_thread_pool_shared());
}
static void test_exit_and_subsystem_callbacks_cannot_restart_default_pool(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, retirement_create());
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&retirement_owner));
    retirement_assert_callbacks();
    retirement_assert_gap_and_next_boot();
}
static void test_failed_init_closes_default_admission_until_next_owner(void)
{
    retirement_probe.fail_init = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, retirement_create());
    retirement_assert_callbacks();
    retirement_assert_gap_and_next_boot();
}
static void test_constructor_borrow_closes_admission_before_deferred_teardown(void)
{
    retirement_probe.close_during_init = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, retirement_create());
    TEST_ASSERT_TRUE(retirement_probe.borrow_refused);
    TEST_ASSERT_TRUE(retirement_probe.borrow_retained);
    TEST_ASSERT_TRUE(retirement_probe.constructor_stopped);
    TEST_ASSERT_TRUE(retirement_probe.constructor_denied);
    TEST_ASSERT_TRUE(retirement_probe.constructor_shared_denied);
    retirement_assert_callbacks();
    retirement_assert_gap_and_next_boot();
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exit_and_subsystem_callbacks_cannot_restart_default_pool);
    RUN_TEST(test_failed_init_closes_default_admission_until_next_owner);
    RUN_TEST(test_constructor_borrow_closes_admission_before_deferred_teardown);
    return UNITY_END();
}
