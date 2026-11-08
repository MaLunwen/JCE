/* Production host dispatch: draw does not require an update callback. */
#include "unity.h"
/* Exercise the shipping profiling-off inline path regardless of native cache flags. */
#ifdef JCE_TRACY_ENABLED
#undef JCE_TRACY_ENABLED
#endif
#define JCE_TRACY_ENABLED 0
#include "application/jce_engine_callbacks.h"
#include <jce/os/core/jce_trace.h>
#include <string.h>

typedef struct Probe {
    int updates, draws, order;
    float dt;
    const JceServices *services;
    uint64_t update_task, draw_task;
} Probe;

void setUp(void)
{
    jce_perf_phase_set_enabled(1);
    jce_trace_set_enabled(false);
    jce_trace_reset();
}
void tearDown(void)
{
    jce_perf_phase_set_enabled(0);
    jce_trace_thread_unregister();
    jce_trace_set_enabled(false);
}

static void update(float dt, void *user)
{
    Probe *probe = user;
    ++probe->updates;
    probe->order = probe->order * 10 + 1;
    probe->dt = dt;
    probe->update_task = jce_trace_task_current_id();
}

static void draw(const JceServices *services, void *user)
{
    Probe *probe = user;
    ++probe->draws;
    probe->order = probe->order * 10 + 2;
    probe->services = services;
    probe->draw_task = jce_trace_task_current_id();
}

static void test_draw_only(void)
{
    Probe probe = {0}; JceServices services = {0}; JceAppDesc app = {0};
    app.draw = draw; app.user_data = &probe;
    engine_app_update_and_draw(&app, .25f, &services);
    TEST_ASSERT_EQUAL_INT(0, probe.updates);
    TEST_ASSERT_EQUAL_INT(1, probe.draws);
    TEST_ASSERT_EQUAL_PTR(&services, probe.services);
}

static void test_update_only(void)
{
    Probe probe = {0}; JceAppDesc app = {0};
    app.update = update; app.user_data = &probe;
    engine_app_update_and_draw(&app, .125f, NULL);
    TEST_ASSERT_EQUAL_INT(1, probe.updates);
    TEST_ASSERT_EQUAL_INT(0, probe.draws);
    TEST_ASSERT_EQUAL_FLOAT(.125f, probe.dt);
}

static void test_both_keep_update_before_draw(void)
{
    Probe probe = {0}; JceAppDesc app = {0};
    app.update = update; app.draw = draw; app.user_data = &probe;
    engine_app_update_and_draw(&app, .125f, NULL);
    TEST_ASSERT_EQUAL_INT(12, probe.order);
}

static void test_no_callbacks_and_null_descriptor(void)
{
    Probe probe = {0}; JceAppDesc app = {0}; app.user_data = &probe;
    engine_app_update_and_draw(&app, .125f, NULL);
    engine_app_update_and_draw(NULL, .125f, NULL);
    TEST_ASSERT_EQUAL_INT(0, probe.order);
}

static void test_actual_callbacks_emit_services_without_profiling(void)
{
    Probe probe = {0};
    JceServices services = {0};
    JceAppDesc app = {0};
    JceTraceEvent events[32];
    JceTraceCursor cursor;
    JceTraceStats stats;
    uint32_t count, i;
    uint64_t parent;
    int update_submit = -1, update_begin = -1, update_end = -1;
    int draw_submit = -1, draw_begin = -1, draw_end = -1;
    app.update = update;
    app.draw = draw;
    app.user_data = &probe;
    jce_trace_set_enabled(true);
    parent = jce_trace_next_id();
    jce_trace_task_submit(parent, 0, JCE_TRACE_TASK_SERVICE, "test.callback.owner");
    jce_trace_task_begin(parent, parent, "test.callback.owner", 0);
    engine_app_update_and_draw(&app, .125f, &services);
    TEST_ASSERT_EQUAL_INT(12, probe.order);
    TEST_ASSERT_EQUAL_FLOAT(.125f, probe.dt);
    TEST_ASSERT_EQUAL_PTR(&services, probe.services);
    TEST_ASSERT_TRUE(probe.update_task != 0 && probe.update_task != parent);
    TEST_ASSERT_TRUE(probe.draw_task != 0 && probe.draw_task != parent);
    TEST_ASSERT_TRUE(probe.draw_task != probe.update_task);
    TEST_ASSERT_EQUAL_UINT64(parent, jce_trace_task_current_id());
    jce_trace_task_end(parent, parent, "test.callback.owner", JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_trace_task_restore_id(0);
    jce_trace_thread_unregister();
    jce_trace_cursor_init(&cursor);
    count = jce_trace_read(&cursor, events, 32);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    for (i = 0; i < count; ++i) {
        JceTraceEvent *event = &events[i];
        bool is_update = strcmp(event->name, "engine.app_update") == 0;
        bool is_draw = strcmp(event->name, "engine.app_draw") == 0;
        if (!is_update && !is_draw) continue;
        TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_TASK, event->category);
        if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            TEST_ASSERT_EQUAL_UINT64(parent, event->parent_id);
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_TASK_SERVICE, event->value_u32[0]);
            if (is_update) update_submit = (int)i;
            else draw_submit = (int)i;
        } else {
            TEST_ASSERT_EQUAL_UINT64(is_update ? probe.update_task : probe.draw_task, event->id);
            TEST_ASSERT_EQUAL_UINT64(event->id, event->parent_id);
            if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
                if (is_update) update_begin = (int)i;
                else draw_begin = (int)i;
            } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
                TEST_ASSERT_EQUAL_INT(JCE_TRACE_TASK_SUCCEEDED, event->value_u32[0]);
                if (is_update) update_end = (int)i;
                else draw_end = (int)i;
            }
        }
    }
    TEST_ASSERT_TRUE(update_submit >= 0 && update_begin > update_submit && update_end > update_begin);
    TEST_ASSERT_TRUE(draw_submit > update_end && draw_begin > draw_submit && draw_end > draw_begin);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
}

static void test_disabled_callbacks_and_absent_callbacks_emit_no_services(void)
{
    Probe probe = {0};
    JceAppDesc app = {0};
    JceTraceStats stats;
    app.update = update;
    app.draw = draw;
    app.user_data = &probe;
    engine_app_update_and_draw(&app, .125f, NULL);
    TEST_ASSERT_EQUAL_INT(12, probe.order);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    jce_trace_set_enabled(true);
    app.update = NULL;
    app.draw = NULL;
    engine_app_update_and_draw(&app, .125f, NULL);
    engine_app_update_and_draw(NULL, .125f, NULL);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_INT(12, probe.order);
}

static void test_cpu_dispatch_never_draws_even_with_both_callbacks(void)
{
    Probe probe={0}; JceAppDesc app={0}; app.update=update; app.draw=draw; app.user_data=&probe;
    engine_app_update_only(&app,.034f);
    TEST_ASSERT_EQUAL_INT(1,probe.updates); TEST_ASSERT_EQUAL_INT(0,probe.draws);
    TEST_ASSERT_EQUAL_FLOAT(.034f,probe.dt); TEST_ASSERT_EQUAL_INT(1,probe.order);
    engine_app_draw_only(&app,NULL);
    TEST_ASSERT_EQUAL_INT(12,probe.order); TEST_ASSERT_EQUAL_INT(1,probe.draws);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cpu_dispatch_never_draws_even_with_both_callbacks);
    RUN_TEST(test_draw_only); RUN_TEST(test_update_only);
    RUN_TEST(test_both_keep_update_before_draw);
    RUN_TEST(test_no_callbacks_and_null_descriptor);
    RUN_TEST(test_actual_callbacks_emit_services_without_profiling);
    RUN_TEST(test_disabled_callbacks_and_absent_callbacks_emit_no_services);
    return UNITY_END();
}
