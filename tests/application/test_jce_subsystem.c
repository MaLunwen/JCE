/* test_jce_subsystem.c
 *
 * Unit tests for the pluggable subsystem registry.  Verifies:
 *   - create / destroy lifecycle (incl. NULL safety)
 *   - register rejects bad inputs / late registration
 *   - init_all sorts by priority (ascending) before invoking init
 *   - update_all runs only after a successful init_all
 *   - shutdown_all runs in reverse priority + quiesces first
 *   - init_all rollback path (quiesce + shutdown of already-init'd
 *     subsystems when a later init() returns false)
 */

#include <jce/application/jce_subsystem.h>
#include <jce/os/core/jce_allocator.h>

#include <string.h>

#include "unity.h"

#define MAX_EVENTS 64

typedef enum { EV_INIT, EV_UPDATE, EV_SHUTDOWN, EV_QUIESCE } EventKind;

typedef struct Event {
    int       owner;
    EventKind kind;
} Event;

static Event g_events[MAX_EVENTS];
static int   g_event_count;

static void clear_events(void)
{
    g_event_count = 0;
    memset(g_events, 0, sizeof(g_events));
}

static void push_event(int owner, EventKind kind)
{
    if (g_event_count < MAX_EVENTS) {
        g_events[g_event_count].owner = owner;
        g_events[g_event_count].kind  = kind;
        g_event_count++;
    }
}

typedef struct SubCtx {
    int  id;
    bool fail_init;
    int  init_count;
    int  update_count;
    int  shutdown_count;
    int  quiesce_count;
    float last_dt;
} SubCtx;

static bool sub_init(const JceServices *svc, void *user)
{
    (void)svc;
    SubCtx *c = (SubCtx *)user;
    c->init_count++;
    push_event(c->id, EV_INIT);
    return !c->fail_init;
}

static void sub_update(float dt, void *user)
{
    SubCtx *c = (SubCtx *)user;
    c->update_count++;
    c->last_dt = dt;
    push_event(c->id, EV_UPDATE);
}

static void sub_shutdown(void *user)
{
    SubCtx *c = (SubCtx *)user;
    c->shutdown_count++;
    push_event(c->id, EV_SHUTDOWN);
}

static void sub_quiesce(void *user)
{
    SubCtx *c = (SubCtx *)user;
    c->quiesce_count++;
    push_event(c->id, EV_QUIESCE);
}

static jce_subsystem_desc_t make_desc(const char *name, int priority,
                                      SubCtx *ctx)
{
    jce_subsystem_desc_t d;
    memset(&d, 0, sizeof(d));
    d.name     = name;
    d.priority = priority;
    d.init     = sub_init;
    d.update   = sub_update;
    d.shutdown = sub_shutdown;
    d.quiesce  = sub_quiesce;
    d.ctx      = ctx;
    return d;
}

void setUp(void)    { clear_events(); }
void tearDown(void) {}

static void test_create_destroy_basic(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(r);
    jce_subsystem_registry_destroy(r);
    jce_subsystem_registry_destroy(NULL);
}

static void test_register_rejects_bad_inputs(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(r);

    jce_subsystem_desc_t d = {0};
    TEST_ASSERT_FALSE(jce_subsystem_register(r, &d));
    TEST_ASSERT_FALSE(jce_subsystem_register(NULL, &d));
    TEST_ASSERT_FALSE(jce_subsystem_register(r, NULL));

    jce_subsystem_registry_destroy(r);
}

static void test_init_sorts_by_priority(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());

    SubCtx a = { .id = 1 };
    SubCtx b = { .id = 2 };
    SubCtx c = { .id = 3 };
    jce_subsystem_desc_t da = make_desc("a", 300, &a);
    jce_subsystem_desc_t db = make_desc("b", 100, &b);
    jce_subsystem_desc_t dc = make_desc("c", 200, &c);
    TEST_ASSERT_TRUE(jce_subsystem_register(r, &da));
    TEST_ASSERT_TRUE(jce_subsystem_register(r, &db));
    TEST_ASSERT_TRUE(jce_subsystem_register(r, &dc));

    TEST_ASSERT_TRUE(jce_subsystem_init_all(r, NULL));

    int seen = 0;
    int order[3] = {0, 0, 0};
    for (int i = 0; i < g_event_count; ++i) {
        if (g_events[i].kind == EV_INIT) {
            TEST_ASSERT_TRUE(seen < 3);
            order[seen++] = g_events[i].owner;
        }
    }
    TEST_ASSERT_EQUAL_INT(3, seen);
    TEST_ASSERT_EQUAL_INT(2, order[0]);
    TEST_ASSERT_EQUAL_INT(3, order[1]);
    TEST_ASSERT_EQUAL_INT(1, order[2]);

    jce_subsystem_shutdown_all(r);
    jce_subsystem_registry_destroy(r);
}

static void test_register_rejected_after_init(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());
    SubCtx a = { .id = 1 };
    jce_subsystem_desc_t da = make_desc("a", 10, &a);
    TEST_ASSERT_TRUE(jce_subsystem_register(r, &da));
    TEST_ASSERT_TRUE(jce_subsystem_init_all(r, NULL));

    SubCtx b = { .id = 2 };
    jce_subsystem_desc_t db = make_desc("b", 20, &b);
    TEST_ASSERT_FALSE(jce_subsystem_register(r, &db));
    TEST_ASSERT_FALSE(jce_subsystem_init_all(r, NULL));

    jce_subsystem_shutdown_all(r);
    jce_subsystem_registry_destroy(r);
}

static void test_update_runs_only_after_init(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());
    SubCtx a = { .id = 1 };
    jce_subsystem_desc_t da = make_desc("a", 10, &a);
    TEST_ASSERT_TRUE(jce_subsystem_register(r, &da));

    jce_subsystem_update_all(r, 0.016f);
    TEST_ASSERT_EQUAL_INT(0, a.update_count);

    TEST_ASSERT_TRUE(jce_subsystem_init_all(r, NULL));
    jce_subsystem_update_all(r, 0.033f);
    TEST_ASSERT_EQUAL_INT(1, a.update_count);
    TEST_ASSERT_EQUAL_FLOAT(0.033f, a.last_dt);

    jce_subsystem_shutdown_all(r);
    jce_subsystem_registry_destroy(r);
}

static void test_shutdown_runs_in_reverse_order_after_quiesce(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());
    SubCtx a = { .id = 1 };
    SubCtx b = { .id = 2 };
    SubCtx c = { .id = 3 };
    jce_subsystem_desc_t da = make_desc("a", 10, &a);
    jce_subsystem_desc_t db = make_desc("b", 20, &b);
    jce_subsystem_desc_t dc = make_desc("c", 30, &c);
    jce_subsystem_register(r, &da);
    jce_subsystem_register(r, &db);
    jce_subsystem_register(r, &dc);
    TEST_ASSERT_TRUE(jce_subsystem_init_all(r, NULL));
    clear_events();

    jce_subsystem_shutdown_all(r);

    EventKind expected[6] = {
        EV_QUIESCE, EV_QUIESCE, EV_QUIESCE,
        EV_SHUTDOWN, EV_SHUTDOWN, EV_SHUTDOWN
    };
    int expected_owner[6] = { 3, 2, 1, 3, 2, 1 };
    TEST_ASSERT_EQUAL_INT(6, g_event_count);
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_INT(expected[i],       g_events[i].kind);
        TEST_ASSERT_EQUAL_INT(expected_owner[i], g_events[i].owner);
    }

    clear_events();
    jce_subsystem_shutdown_all(r);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);

    jce_subsystem_registry_destroy(r);
}

static void test_init_rollback_on_failure(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());
    SubCtx a = { .id = 1 };
    SubCtx b = { .id = 2, .fail_init = true };
    SubCtx c = { .id = 3 };
    jce_subsystem_desc_t da = make_desc("a", 10, &a);
    jce_subsystem_desc_t db = make_desc("b", 20, &b);
    jce_subsystem_desc_t dc = make_desc("c", 30, &c);
    jce_subsystem_register(r, &da);
    jce_subsystem_register(r, &db);
    jce_subsystem_register(r, &dc);

    TEST_ASSERT_FALSE(jce_subsystem_init_all(r, NULL));

    TEST_ASSERT_EQUAL_INT(1, a.init_count);
    TEST_ASSERT_EQUAL_INT(1, b.init_count);
    TEST_ASSERT_EQUAL_INT(0, c.init_count);
    TEST_ASSERT_EQUAL_INT(1, a.quiesce_count);
    TEST_ASSERT_EQUAL_INT(0, b.quiesce_count);
    TEST_ASSERT_EQUAL_INT(0, c.quiesce_count);
    TEST_ASSERT_EQUAL_INT(1, a.shutdown_count);
    TEST_ASSERT_EQUAL_INT(0, b.shutdown_count);
    TEST_ASSERT_EQUAL_INT(0, c.shutdown_count);

    jce_subsystem_shutdown_all(r);
    TEST_ASSERT_EQUAL_INT(1, a.shutdown_count);

    jce_subsystem_registry_destroy(r);
}

static void test_growth_past_initial_capacity(void)
{
    jce_subsystem_registry_t *r =
        jce_subsystem_registry_create(jce_allocator_default());

    enum { N = 24 };
    SubCtx ctxs[N];
    jce_subsystem_desc_t descs[N];
    memset(ctxs,  0, sizeof(ctxs));
    static const char *names[N] = {
        "s0","s1","s2","s3","s4","s5","s6","s7","s8","s9",
        "s10","s11","s12","s13","s14","s15","s16","s17","s18","s19",
        "s20","s21","s22","s23"
    };
    for (int i = 0; i < N; ++i) {
        ctxs[i].id = i + 1;
        descs[i] = make_desc(names[i], (i * 37) % 1000, &ctxs[i]);
        TEST_ASSERT_TRUE(jce_subsystem_register(r, &descs[i]));
    }
    TEST_ASSERT_TRUE(jce_subsystem_init_all(r, NULL));
    for (int i = 0; i < N; ++i)
        TEST_ASSERT_EQUAL_INT(1, ctxs[i].init_count);

    jce_subsystem_shutdown_all(r);
    jce_subsystem_registry_destroy(r);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy_basic);
    RUN_TEST(test_register_rejects_bad_inputs);
    RUN_TEST(test_init_sorts_by_priority);
    RUN_TEST(test_register_rejected_after_init);
    RUN_TEST(test_update_runs_only_after_init);
    RUN_TEST(test_shutdown_runs_in_reverse_order_after_quiesce);
    RUN_TEST(test_init_rollback_on_failure);
    RUN_TEST(test_growth_past_initial_capacity);
    return UNITY_END();
}
