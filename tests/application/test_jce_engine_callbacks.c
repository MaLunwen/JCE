/* Production host dispatch: draw does not require an update callback. */
#include "unity.h"
#include "application/jce_engine_callbacks.h"
#include <string.h>

typedef struct Probe {
    int updates, draws, order;
    float dt;
    const JceServices *services;
} Probe;

void setUp(void) { jce_perf_phase_set_enabled(1); }
void tearDown(void) { jce_perf_phase_set_enabled(0); }

static void update(float dt, void *user)
{
    Probe *probe = user;
    ++probe->updates;
    probe->order = probe->order * 10 + 1;
    probe->dt = dt;
}

static void draw(const JceServices *services, void *user)
{
    Probe *probe = user;
    ++probe->draws;
    probe->order = probe->order * 10 + 2;
    probe->services = services;
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_draw_only); RUN_TEST(test_update_only);
    RUN_TEST(test_both_keep_update_before_draw);
    RUN_TEST(test_no_callbacks_and_null_descriptor);
    return UNITY_END();
}
